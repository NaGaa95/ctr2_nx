#include <switch.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <string.h>
#include "config.h"
#include "android_native_unity.h"
#include "ctr_video.h"
#include "zf_input.h"

/* These values are ZRenderer.TouchSequence.TouchType ordinals, not Android's
 * MotionEvent ACTION_* constants.  CTR's Java bridge declares the enum as
 * DOWN, MOVE, UP, CANCEL and passes type.ordinal() to nativePassTouch(). */
enum { ACTION_DOWN=0, ACTION_MOVE=1, ACTION_UP=2, ACTION_CANCEL=3, MAX_TOUCH=5 };
#define TOUCH_TAP_SLOP 18.0f
#define TOUCH_MOVE_EPSILON 0.5f
typedef struct {
  int active,dragging;
  uint32_t raw_id;
  float x,y,raw_x,raw_y,start_x,start_y;
} TrackedTouch;
typedef struct { uint32_t raw_id; float x,y; } TouchSample;
static PadState g_pad;
static TrackedTouch g_touches[MAX_TOUCH];
static float g_cursor_x, g_cursor_y;
static int g_cursor_visible, g_a_down;
static uint64_t g_last_touch_sample;
static int g_have_touch_sample;
static int g_video_touch_latched,g_swallow_video_touch;

static void map_touch(float px, float py, float *x, float *y) {
  const float pw=1280.0f, ph=720.0f;
  float gw=(float)android_native_width(), gh=(float)android_native_height();
  if (config.portrait==2) { *x=(ph-py)*(gw/ph); *y=px*(gh/pw); }
  else if (config.portrait==1) { *x=py*(gw/ph); *y=(pw-px)*(gh/pw); }
  else { *x=px*(gw/pw); *y=py*(gh/ph); }
  if(*x<0)*x=0;
  if(*x>gw)*x=gw;
  if(*y<0)*y=0;
  if(*y>gh)*y=gh;
}

void zf_input_init(void) {
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  /* libnx documents this as mandatory when the touchscreen API is used. */
  hidInitializeTouchScreen();
  g_cursor_x=android_native_width()*0.5f;
  g_cursor_y=android_native_height()*0.5f;
  memset(g_touches,0,sizeof g_touches);g_have_touch_sample=0;
}

static int any_touch_active(void){
  for(int i=0;i<MAX_TOUCH;i++)if(g_touches[i].active)return 1;
  return 0;
}

static int exact_touch_slot(uint32_t raw_id,const uint8_t used[MAX_TOUCH]){
  for(int i=0;i<MAX_TOUCH;i++)
    if(g_touches[i].active&&!used[i]&&g_touches[i].raw_id==raw_id)return i;
  return -1;
}

static int nearest_touch_slot(float x,float y,const uint8_t used[MAX_TOUCH]){
  int best=-1;float best_d2=240.0f*240.0f;
  for(int i=0;i<MAX_TOUCH;i++)if(g_touches[i].active&&!used[i]){
    const float dx=x-g_touches[i].raw_x,dy=y-g_touches[i].raw_y,d2=dx*dx+dy*dy;
    if(d2<best_d2){best=i;best_d2=d2;}
  }
  return best;
}

static int free_touch_slot(const uint8_t used[MAX_TOUCH]){
  for(int i=0;i<MAX_TOUCH;i++)if(!g_touches[i].active&&!used[i])return i;
  return -1;
}

/* Android normally delivers touchscreen samples faster than the render loop.
 * Interpolate the Switch samples so a quick swipe cannot jump across a narrow
 * rope without ever presenting a point close enough for CTR's cut detector. */
static void emit_touch_move(zf_touch_fn touch,void *env,void *renderer,int id,
                            TrackedTouch *tracked,float x,float y){
  tracked->raw_x=x;tracked->raw_y=y;

  /* Switch HID reports a few pixels of contact wobble even for a stationary
   * finger.  CTR2's world map treats any delivered MOVE as a pan and cancels
   * its level click.  Hold MOVE events inside Android-like touch slop; once a
   * real drag crosses it, interpolate from the original DOWN so rope cuts and
   * deliberate map swipes still receive the complete path. */
  if(!tracked->dragging){
    const float start_dx=x-tracked->start_x,start_dy=y-tracked->start_y;
    if(start_dx*start_dx+start_dy*start_dy<TOUCH_TAP_SLOP*TOUCH_TAP_SLOP)return;
    tracked->dragging=1;
  }

  const float dx=x-tracked->x,dy=y-tracked->y,delta=sqrtf(dx*dx+dy*dy);
  if(delta<TOUCH_MOVE_EPSILON)return;
  int steps=(int)ceilf(delta/10.0f);if(steps<1)steps=1;if(steps>32)steps=32;
  if(touch)for(int step=1;step<=steps;step++){
    const float t=(float)step/(float)steps;
    touch(env,renderer,tracked->x+dx*t,tracked->y+dy*t,id,ACTION_MOVE);
  }
  tracked->x=x;tracked->y=y;
}

static void finish_touch(zf_touch_fn touch,void *env,void *renderer,int id,int action){
  TrackedTouch *tracked=&g_touches[id];if(!tracked->active)return;
  if(touch)touch(env,renderer,tracked->x,tracked->y,id,action);
  memset(tracked,0,sizeof *tracked);
}

static void cancel_all_touches(zf_touch_fn touch,void *env,void *renderer){
  for(int i=0;i<MAX_TOUCH;i++)finish_touch(touch,env,renderer,i,ACTION_CANCEL);
}

int zf_input_update(zf_touch_fn touch, zf_button_fn back, void *env, void *renderer) {
  padUpdate(&g_pad);
  u64 down=padGetButtonsDown(&g_pad);
  if(down&HidNpadButton_Plus)return 0;
  if(ctr_video_is_active()){
    cancel_all_touches(touch,env,renderer);g_a_down=0;g_cursor_visible=0;
    HidTouchScreenState video_touch={0};
    const int touching=hidGetTouchScreenStates(&video_touch,1)>0&&video_touch.count>0;
    if(touching&&!g_video_touch_latched){
      ctr_video_request_skip();g_swallow_video_touch=1;
    }
    g_video_touch_latched=touching;
    if(down&HidNpadButton_B)ctr_video_request_skip();
    return 1;
  }
  g_video_touch_latched=0;
  if(g_swallow_video_touch){
    HidTouchScreenState swallowed={0};
    if(hidGetTouchScreenStates(&swallowed,1)<=0||swallowed.count>0)return 1;
    g_swallow_video_touch=0;
    g_last_touch_sample=swallowed.sampling_number;g_have_touch_sample=1;
  }
  if((down&HidNpadButton_B)&&back){
    const uint8_t used=back(env,renderer);
    if(!used)return 0;
  }

  HidTouchScreenState state={0};
  const int have=hidGetTouchScreenStates(&state,1)>0;
  const int fresh=have&&(!g_have_touch_sample||state.sampling_number!=g_last_touch_sample);
  if(fresh){
    g_last_touch_sample=state.sampling_number;g_have_touch_sample=1;
    int count=(int)state.count;if(count<0)count=0;if(count>MAX_TOUCH)count=MAX_TOUCH;
    TouchSample current[MAX_TOUCH];int slots[MAX_TOUCH];uint8_t used[MAX_TOUCH]={0};
    for(int i=0;i<count;i++){
      current[i].raw_id=state.touches[i].finger_id;
      map_touch((float)state.touches[i].x,(float)state.touches[i].y,
                &current[i].x,&current[i].y);
      slots[i]=-1;
    }
    /* Preserve an exact hardware id first, then tolerate firmware/id churn by
     * matching an unmatched contact at nearly the same screen position. */
    for(int i=0;i<count;i++){
      slots[i]=exact_touch_slot(current[i].raw_id,used);
      if(slots[i]>=0)used[slots[i]]=1;
    }
    for(int i=0;i<count;i++)if(slots[i]<0){
      int slot=nearest_touch_slot(current[i].x,current[i].y,used);
      if(slot<0)slot=free_touch_slot(used);
      if(slot<0)continue;
      slots[i]=slot;used[slot]=1;
    }
    for(int i=0;i<count;i++)if(slots[i]>=0){
      const int id=slots[i];TrackedTouch *tracked=&g_touches[id];
      if(!tracked->active){
        *tracked=(TrackedTouch){.active=1,.raw_id=current[i].raw_id,.x=current[i].x,.y=current[i].y,
                                .raw_x=current[i].x,.raw_y=current[i].y,
                                .start_x=current[i].x,.start_y=current[i].y};
        if(touch)touch(env,renderer,tracked->x,tracked->y,id,ACTION_DOWN);
      }else{
        if(tracked->raw_id!=current[i].raw_id){
          tracked->raw_id=current[i].raw_id;
        }
        emit_touch_move(touch,env,renderer,id,tracked,current[i].x,current[i].y);
      }
    }
    for(int i=0;i<MAX_TOUCH;i++)if(g_touches[i].active&&!used[i])
      finish_touch(touch,env,renderer,i,ACTION_UP);
    if(count>0){g_cursor_visible=0;g_a_down=0;return 1;}
  }else if(any_touch_active()){
    /* A missed/duplicate HID read is not a release.  Wait for an explicit fresh
     * zero-contact state; otherwise every gap becomes DOWN/UP and kills swipes. */
    return 1;
  }

  HidAnalogStickState stick=padGetStickPos(&g_pad,0);
  float sx=(float)stick.x/32767.0f*14.0f, sy=(float)stick.y/32767.0f*14.0f;
  if(config.portrait==2){ g_cursor_x+=sy; g_cursor_y+=sx; }
  else if(config.portrait==1){ g_cursor_x-=sy; g_cursor_y-=sx; }
  else { g_cursor_x+=sx; g_cursor_y-=sy; }
  float gw=(float)android_native_width(),gh=(float)android_native_height();
  if(g_cursor_x<0)g_cursor_x=0;
  if(g_cursor_x>gw)g_cursor_x=gw;
  if(g_cursor_y<0)g_cursor_y=0;
  if(g_cursor_y>gh)g_cursor_y=gh;
  if(sx*sx+sy*sy>0.25f)g_cursor_visible=1;
  int a=(padGetButtons(&g_pad)&HidNpadButton_A)!=0;
  if(a)g_cursor_visible=1;
  if(touch&&a&&!g_a_down)touch(env,renderer,g_cursor_x,g_cursor_y,0,ACTION_DOWN);
  else if(touch&&a&&g_a_down)touch(env,renderer,g_cursor_x,g_cursor_y,0,ACTION_MOVE);
  else if(touch&&!a&&g_a_down)touch(env,renderer,g_cursor_x,g_cursor_y,0,ACTION_UP);
  g_a_down=a;
  return 1;
}

static GLuint link_cursor(void){
  static const char *vs="attribute vec2 p;attribute vec2 q;varying vec2 v;void main(){v=q;gl_Position=vec4(p,0.0,1.0);}";
  static const char *fs="precision mediump float;varying vec2 v;void main(){float d=length(v);float a=1.0-smoothstep(.70,1.0,d);gl_FragColor=vec4(1.0,.95,.2,a);}";
  GLuint v=glCreateShader(GL_VERTEX_SHADER),f=glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(v,1,&vs,0);glCompileShader(v);glShaderSource(f,1,&fs,0);glCompileShader(f);
  GLuint p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glBindAttribLocation(p,0,"p");glBindAttribLocation(p,1,"q");glLinkProgram(p);glDeleteShader(v);glDeleteShader(f);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){glDeleteProgram(p);return 0;}return p;
}

void zf_input_draw_cursor(void){
  if(!g_cursor_visible)return;
  static GLuint prog;static int tried;
  if(!tried){tried=1;prog=link_cursor();}
  if(!prog)return;
  float w=(float)android_native_width(),h=(float)android_native_height();float cx=g_cursor_x/w*2-1,cy=1-g_cursor_y/h*2,rx=22.0f/w*2,ry=22.0f/h*2;
  GLfloat p[8]={cx-rx,cy-ry,cx+rx,cy-ry,cx-rx,cy+ry,cx+rx,cy+ry};static const GLfloat q[8]={-1,-1,1,-1,-1,1,1,1};
  GLint oldprog=0,oldbuf=0,vp[4];GLboolean blend=glIsEnabled(GL_BLEND);glGetIntegerv(GL_CURRENT_PROGRAM,&oldprog);glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&oldbuf);glGetIntegerv(GL_VIEWPORT,vp);
  glBindBuffer(GL_ARRAY_BUFFER,0);glUseProgram(prog);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glViewport(0,0,(GLsizei)w,(GLsizei)h);
  glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,p);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,0,q);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glDisableVertexAttribArray(0);glDisableVertexAttribArray(1);
  glBindBuffer(GL_ARRAY_BUFFER,(GLuint)oldbuf);glUseProgram((GLuint)oldprog);glViewport(vp[0],vp[1],vp[2],vp[3]);if(!blend)glDisable(GL_BLEND);
}
