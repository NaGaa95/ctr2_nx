/* FFmpeg-backed replacement for ZeptoLab's Java ZVideoPlayer Activity.
 * Video is decoded on a worker into a bounded YUV420 queue.  The renderer
 * uploads the frame due at the SDL audio clock and draws it immediately before
 * the wrapper presents CTR's portrait EGL surface. */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <switch.h>
#include <GLES2/gl2.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "android_native_unity.h"
#include "ctr_video.h"
#include "zf_audio.h"

#define VIDEO_QUEUE_CAP 16

typedef struct {
  uint8_t *data;
  double pts;
  uint64_t serial;
} VideoSlot;

static Mutex g_lock,g_request_lock;
static CondVar g_can_produce;
static int g_initialized;

static struct {
  char name[512];
  int flag1,flag2,pending;
} g_request;

/* These flags cross the renderer and decoder threads.  Keep every access
 * atomic; the remaining decoder-owned fields are published by decode_done. */
static int g_active,g_stop,g_decode_done,g_decode_failed,g_skip_requested;
static pthread_t g_thread;
static int g_thread_started;
static char g_path[768];
static VideoSlot g_slots[VIDEO_QUEUE_CAP];
static int g_head,g_tail,g_count,g_width,g_height,g_chroma_w,g_chroma_h;
static uint64_t g_next_serial,g_uploaded_serial;
static unsigned g_frames_decoded,g_frames_shown;
static double g_pts_origin,g_last_pts,g_frame_duration;
static int g_pts_origin_set,g_have_frame,g_have_audio,g_audio_rate;
static int g_mute,g_preserve_aspect;
static uint64_t g_wall_start_ns,g_audio_tail_start_ns;
static double g_audio_tail_base;
static int g_completion_pending;

static struct {
  GLuint program,textures[3];
  GLint position,uv,samplers[3];
  int ready,failed,width,height;
} g_gl;

static uint64_t monotonic_ns(void) {
  return armTicksToNs(armGetSystemTick());
}

static int active(void) {
  return __atomic_load_n(&g_active,__ATOMIC_ACQUIRE);
}

static int stop_requested(void) {
  return __atomic_load_n(&g_stop,__ATOMIC_ACQUIRE);
}

static int have_audio(void) {
  return __atomic_load_n(&g_have_audio,__ATOMIC_ACQUIRE);
}

static int regular_file(const char *path) {
  struct stat st;
  return path&&stat(path,&st)==0&&S_ISREG(st.st_mode);
}

static int has_mp4_suffix(const char *name) {
  const size_t n=name?strlen(name):0;
  return n>=4&&!strcasecmp(name+n-4,".mp4");
}

/* CTR2 normally requests intro.mp4/outro.mp4 by resource basename. Accept the
 * original APK path and common URI forms as well, but always reduce them to a
 * safe basename before looking in the preserved movies directory. */
static int resolve_movie_path(const char *requested,char *out,size_t cap) {
  if(!requested||!requested[0])return 0;
  char clean[512];snprintf(clean,sizeof clean,"%s",requested);
  char *query=strpbrk(clean,"?#");if(query)*query=0;
  for(char *p=clean;*p;p++)if(*p=='\\')*p='/';

  if(!strstr(clean,"..")&&!strchr(clean,':')&&regular_file(clean)){
    snprintf(out,cap,"%s",clean);return 1;
  }
  const char *base=strrchr(clean,'/');base=base?base+1:clean;
  if(!base[0]||strstr(base,".."))return 0;
  if(strlen(base)>=256)return 0;
  char file[256];snprintf(file,sizeof file,"%s",base);
  if(!has_mp4_suffix(file)){
    const size_t n=strlen(file);
    if(n+4>=sizeof file)return 0;
    memcpy(file+n,".mp4",5);
  }
  const char *formats[]={"movies/%s","res/raw/%s","assets/%s","%s"};
  for(size_t i=0;i<sizeof formats/sizeof formats[0];i++){
    if(snprintf(out,cap,formats[i],file)>=(int)cap)continue;
    if(regular_file(out))return 1;
  }
  return 0;
}

static int open_codec(AVCodecContext **out,AVStream *stream,int threads) {
  const AVCodec *codec=avcodec_find_decoder(stream->codecpar->codec_id);
  if(!codec)return AVERROR_DECODER_NOT_FOUND;
  AVCodecContext *context=avcodec_alloc_context3(codec);
  if(!context)return AVERROR(ENOMEM);
  int rc=avcodec_parameters_to_context(context,stream->codecpar);
  if(rc>=0){context->thread_count=threads;rc=avcodec_open2(context,codec,NULL);}
  if(rc<0){avcodec_free_context(&context);return rc;}
  *out=context;return 0;
}

static int allocate_video_slots(int width,int height) {
  const int chroma_w=(width+1)/2,chroma_h=(height+1)/2;
  const size_t bytes=(size_t)width*height+(size_t)chroma_w*chroma_h*2;
  if(width<=0||height<=0||bytes>128u*1024u*1024u)return 0;
  for(int i=0;i<VIDEO_QUEUE_CAP;i++){
    g_slots[i].data=malloc(bytes);
    if(!g_slots[i].data)return 0;
  }
  g_width=width;g_height=height;g_chroma_w=chroma_w;g_chroma_h=chroma_h;
  return 1;
}

static int enqueue_video_frame(struct SwsContext *sws,const AVFrame *frame,double pts) {
  mutexLock(&g_lock);
  while(g_count==VIDEO_QUEUE_CAP&&!stop_requested()){
    /* Some MP4 muxers place the first audio packet after a long video run.
     * Retain the initial queue but drop excess prefill frames until audio has
     * arrived, preventing a demux deadlock at clock zero. */
    if(have_audio()&&zf_audio_movie_frames_queued()==0){
      mutexUnlock(&g_lock);return 1;
    }
    condvarWait(&g_can_produce,&g_lock);
  }
  if(stop_requested()){mutexUnlock(&g_lock);return 0;}
  const int slot_index=g_tail;
  mutexUnlock(&g_lock);

  VideoSlot *slot=&g_slots[slot_index];
  uint8_t *dst[4]={slot->data,
                    slot->data+(size_t)g_width*g_height,
                    slot->data+(size_t)g_width*g_height+(size_t)g_chroma_w*g_chroma_h,
                    NULL};
  int strides[4]={g_width,g_chroma_w,g_chroma_w,0};
  if(sws_scale(sws,(const uint8_t *const *)frame->data,frame->linesize,
               0,frame->height,dst,strides)<=0)return 0;

  if(!g_pts_origin_set){g_pts_origin=pts;g_pts_origin_set=1;}
  slot->pts=pts-g_pts_origin;if(slot->pts<0.0)slot->pts=0.0;
  slot->serial=++g_next_serial;
  mutexLock(&g_lock);
  g_tail=(g_tail+1)%VIDEO_QUEUE_CAP;g_count++;g_frames_decoded++;
  if(slot->pts>g_last_pts)g_last_pts=slot->pts;
  mutexUnlock(&g_lock);
  return 1;
}

static int queue_audio_frame(SwrContext *swr,AVCodecContext *context,
                             const AVFrame *frame,uint8_t **buffer,size_t *capacity) {
  const int wanted=(int)av_rescale_rnd(swr_get_delay(swr,context->sample_rate)+frame->nb_samples,
                                       g_audio_rate,context->sample_rate,AV_ROUND_UP);
  if(wanted<=0)return 1;
  const size_t bytes=(size_t)wanted*2*sizeof(int16_t);
  if(bytes>*capacity){
    uint8_t *grown=realloc(*buffer,bytes);
    if(!grown)return 0;
    *buffer=grown;*capacity=bytes;
  }
  uint8_t *output[1]={*buffer};
  const int frames=swr_convert(swr,output,wanted,
                               (const uint8_t **)frame->extended_data,frame->nb_samples);
  return frames>=0&&zf_audio_movie_queue((const int16_t *)*buffer,frames);
}

static int drain_video(AVCodecContext *context,struct SwsContext *sws,AVFrame *frame,
                       AVRational time_base) {
  int rc;
  while(!stop_requested()&&(rc=avcodec_receive_frame(context,frame))==0){
    int64_t timestamp=frame->best_effort_timestamp;
    if(timestamp==AV_NOPTS_VALUE)timestamp=frame->pts;
    const double pts=timestamp==AV_NOPTS_VALUE
                    ?(double)g_frames_decoded*g_frame_duration
                    :(double)timestamp*av_q2d(time_base);
    if(!enqueue_video_frame(sws,frame,pts))return 0;
  }
  return stop_requested()||rc==AVERROR(EAGAIN)||rc==AVERROR_EOF;
}

static int drain_audio(AVCodecContext *context,SwrContext *swr,AVFrame *frame,
                       uint8_t **buffer,size_t *capacity) {
  int rc;
  while(!stop_requested()&&(rc=avcodec_receive_frame(context,frame))==0)
    if(!queue_audio_frame(swr,context,frame,buffer,capacity))return 0;
  return stop_requested()||rc==AVERROR(EAGAIN)||rc==AVERROR_EOF;
}

static void *decoder_thread(void *unused) {
  (void)unused;
  AVFormatContext *format=NULL;AVCodecContext *video=NULL,*audio=NULL;
  struct SwsContext *scale=NULL;SwrContext *resample=NULL;
  AVPacket *packet=NULL;AVFrame *frame=NULL;uint8_t *audio_buffer=NULL;
  size_t audio_capacity=0;int video_stream=-1,audio_stream=-1,rc=0;
  rc=avformat_open_input(&format,g_path,NULL,NULL);
  if(rc<0)goto failed;
  rc=avformat_find_stream_info(format,NULL);
  if(rc<0)goto failed;
  video_stream=av_find_best_stream(format,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);
  audio_stream=av_find_best_stream(format,AVMEDIA_TYPE_AUDIO,-1,-1,NULL,0);
  if(video_stream<0)goto failed;
  rc=open_codec(&video,format->streams[video_stream],2);
  if(rc<0)goto failed;
  if(!allocate_video_slots(video->width,video->height))goto failed;
  scale=sws_getContext(video->width,video->height,video->pix_fmt,
                       video->width,video->height,AV_PIX_FMT_YUV420P,
                       SWS_BILINEAR,NULL,NULL,NULL);
  if(!scale)goto failed;
  AVRational guessed=av_guess_frame_rate(format,format->streams[video_stream],NULL);
  g_frame_duration=guessed.num>0&&guessed.den>0?av_q2d(av_inv_q(guessed)):1.0/30.0;
  if(g_frame_duration<=0.0||g_frame_duration>0.25)g_frame_duration=1.0/30.0;

  if(!g_mute&&audio_stream>=0&&open_codec(&audio,format->streams[audio_stream],1)>=0){
    g_audio_rate=zf_audio_movie_begin();
    if(g_audio_rate>0){
      AVChannelLayout stereo;av_channel_layout_default(&stereo,2);
      rc=swr_alloc_set_opts2(&resample,&stereo,AV_SAMPLE_FMT_S16,g_audio_rate,
                             &audio->ch_layout,audio->sample_fmt,audio->sample_rate,0,NULL);
      av_channel_layout_uninit(&stereo);
      if(rc>=0&&swr_init(resample)>=0)
        __atomic_store_n(&g_have_audio,1,__ATOMIC_RELEASE);
      else{if(resample)swr_free(&resample);zf_audio_movie_end();g_audio_rate=0;}
    }
  }
  packet=av_packet_alloc();frame=av_frame_alloc();
  if(!packet||!frame)goto failed;
  const AVRational video_tb=format->streams[video_stream]->time_base;
  while(!stop_requested()&&(rc=av_read_frame(format,packet))>=0){
    if(packet->stream_index==video_stream){
      if(avcodec_send_packet(video,packet)>=0&&!drain_video(video,scale,frame,video_tb)){
        av_packet_unref(packet);goto failed;
      }
    }else if(have_audio()&&packet->stream_index==audio_stream){
      if(avcodec_send_packet(audio,packet)>=0&&
         !drain_audio(audio,resample,frame,&audio_buffer,&audio_capacity)){
        av_packet_unref(packet);goto failed;
      }
    }
    av_packet_unref(packet);
  }
  if(!stop_requested()){
    if(avcodec_send_packet(video,NULL)>=0)
      drain_video(video,scale,frame,video_tb);
    if(have_audio()&&avcodec_send_packet(audio,NULL)>=0)
      drain_audio(audio,resample,frame,&audio_buffer,&audio_capacity);
  }
  goto done;

failed:
  if(!stop_requested())__atomic_store_n(&g_decode_failed,1,__ATOMIC_RELEASE);
done:
  free(audio_buffer);if(frame)av_frame_free(&frame);if(packet)av_packet_free(&packet);
  if(resample)swr_free(&resample);
  if(scale)sws_freeContext(scale);
  if(audio)avcodec_free_context(&audio);
  if(video)avcodec_free_context(&video);
  if(format)avformat_close_input(&format);
  __atomic_store_n(&g_decode_done,1,__ATOMIC_RELEASE);
  mutexLock(&g_lock);condvarWakeAll(&g_can_produce);mutexUnlock(&g_lock);
  return NULL;
}

static void free_video_slots(void) {
  for(int i=0;i<VIDEO_QUEUE_CAP;i++){free(g_slots[i].data);g_slots[i].data=NULL;}
}

static void finish_session(int notify) {
  if(!active())return;
  __atomic_store_n(&g_stop,1,__ATOMIC_RELEASE);
  zf_audio_movie_end();
  mutexLock(&g_lock);condvarWakeAll(&g_can_produce);mutexUnlock(&g_lock);
  if(g_thread_started){pthread_join(g_thread,NULL);g_thread_started=0;}
  free_video_slots();g_count=g_head=g_tail=0;
  __atomic_store_n(&g_active,0,__ATOMIC_RELEASE);g_have_frame=0;
  __atomic_store_n(&g_have_audio,0,__ATOMIC_RELEASE);
  if(notify)g_completion_pending++;
}

static int start_session(const char *name,int flag1,int flag2) {
  if(!resolve_movie_path(name,g_path,sizeof g_path)){
    g_completion_pending++;return 0;
  }
  __atomic_store_n(&g_stop,0,__ATOMIC_RELAXED);
  __atomic_store_n(&g_decode_done,0,__ATOMIC_RELAXED);
  __atomic_store_n(&g_decode_failed,0,__ATOMIC_RELAXED);
  __atomic_store_n(&g_skip_requested,0,__ATOMIC_RELAXED);
  g_head=g_tail=g_count=0;g_width=g_height=g_chroma_w=g_chroma_h=0;
  g_next_serial=g_uploaded_serial=0;g_frames_decoded=g_frames_shown=0;
  g_pts_origin=g_last_pts=0.0;g_pts_origin_set=0;g_frame_duration=1.0/30.0;
  g_have_frame=0;__atomic_store_n(&g_have_audio,0,__ATOMIC_RELAXED);
  g_audio_rate=0;g_wall_start_ns=g_audio_tail_start_ns=0;
  g_audio_tail_base=0.0;g_mute=flag1!=0;g_preserve_aspect=flag2!=0;
  __atomic_store_n(&g_active,1,__ATOMIC_RELEASE);
  if(pthread_create(&g_thread,NULL,decoder_thread,NULL)!=0){
    __atomic_store_n(&g_active,0,__ATOMIC_RELEASE);g_completion_pending++;return 0;
  }
  g_thread_started=1;return 1;
}

void ctr_video_init(void) {
  if(g_initialized)return;
  mutexInit(&g_lock);mutexInit(&g_request_lock);condvarInit(&g_can_produce);
  av_log_set_level(AV_LOG_QUIET);g_initialized=1;
}

void ctr_video_request(const char *name,int flag1,int flag2) {
  if(!g_initialized)ctr_video_init();
  mutexLock(&g_request_lock);
  snprintf(g_request.name,sizeof g_request.name,"%s",name?name:"");
  g_request.flag1=flag1;g_request.flag2=flag2;g_request.pending=1;
  mutexUnlock(&g_request_lock);
}

static double playback_clock(void) {
  if(have_audio()&&g_audio_rate>0){
    const uint64_t played=zf_audio_movie_frames_played();
    const uint64_t queued=zf_audio_movie_frames_queued();
    double clock=(double)played/(double)g_audio_rate;
    if(__atomic_load_n(&g_decode_done,__ATOMIC_ACQUIRE)&&played>=queued){
      const uint64_t now=monotonic_ns();
      if(!g_audio_tail_start_ns){g_audio_tail_start_ns=now;g_audio_tail_base=clock;}
      clock=g_audio_tail_base+(double)(now-g_audio_tail_start_ns)/1e9;
    }
    return clock;
  }
  return g_wall_start_ns?(double)(monotonic_ns()-g_wall_start_ns)/1e9:0.0;
}

void ctr_video_update(void) {
  if(!g_initialized)return;
  char name[512]={0};int flag1=0,flag2=0,have_request=0;
  mutexLock(&g_request_lock);
  if(g_request.pending){snprintf(name,sizeof name,"%s",g_request.name);flag1=g_request.flag1;
    flag2=g_request.flag2;g_request.pending=0;have_request=1;}
  mutexUnlock(&g_request_lock);
  if(have_request){if(active())finish_session(0);start_session(name,flag1,flag2);}
  if(!active())return;
  if(__atomic_exchange_n(&g_skip_requested,0,__ATOMIC_ACQ_REL)){
    finish_session(1);return;
  }
  if(!__atomic_load_n(&g_decode_done,__ATOMIC_ACQUIRE))return;
  if(__atomic_load_n(&g_decode_failed,__ATOMIC_ACQUIRE)){
    finish_session(1);return;
  }
  if(g_frames_decoded==0){finish_session(1);return;}
  mutexLock(&g_lock);const int remaining=g_count;mutexUnlock(&g_lock);
  const int audio_done=!have_audio()||
                       zf_audio_movie_frames_played()>=zf_audio_movie_frames_queued();
  if(remaining<=1&&audio_done&&playback_clock()>=g_last_pts+g_frame_duration)
    finish_session(1);
}

int ctr_video_is_active(void) {
  return active()!=0;
}

void ctr_video_request_skip(void) {
  if(active())__atomic_store_n(&g_skip_requested,1,__ATOMIC_RELEASE);
}

int ctr_video_take_completion(void) {
  if(g_completion_pending<=0)return 0;
  g_completion_pending--;return 1;
}

static GLuint compile_shader(GLenum type,const char *source) {
  GLuint shader=glCreateShader(type);glShaderSource(shader,1,&source,NULL);glCompileShader(shader);
  GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
  if(!ok){glDeleteShader(shader);return 0;}
  return shader;
}

static int ensure_gl(void) {
  if(g_gl.ready)return 1;
  if(g_gl.failed)return 0;
  static const char *vs=
    "attribute vec2 aPos;attribute vec2 aUV;varying vec2 vUV;"
    "void main(){vUV=aUV;gl_Position=vec4(aPos,0.0,1.0);}";
  static const char *fs=
    "precision mediump float;uniform sampler2D texY;uniform sampler2D texU;"
    "uniform sampler2D texV;varying vec2 vUV;void main(){"
    "float y=1.1643*(texture2D(texY,vUV).r-0.0625);"
    "float u=texture2D(texU,vUV).r-0.5;float v=texture2D(texV,vUV).r-0.5;"
    "gl_FragColor=vec4(y+1.5958*v,y-0.3917*u-0.8129*v,y+2.017*u,1.0);}";
  GLuint vert=compile_shader(GL_VERTEX_SHADER,vs),frag=compile_shader(GL_FRAGMENT_SHADER,fs);
  if(!vert||!frag){g_gl.failed=1;return 0;}
  g_gl.program=glCreateProgram();glAttachShader(g_gl.program,vert);glAttachShader(g_gl.program,frag);
  glLinkProgram(g_gl.program);glDeleteShader(vert);glDeleteShader(frag);
  GLint ok=0;glGetProgramiv(g_gl.program,GL_LINK_STATUS,&ok);
  if(!ok){g_gl.failed=1;return 0;}
  g_gl.position=glGetAttribLocation(g_gl.program,"aPos");g_gl.uv=glGetAttribLocation(g_gl.program,"aUV");
  g_gl.samplers[0]=glGetUniformLocation(g_gl.program,"texY");
  g_gl.samplers[1]=glGetUniformLocation(g_gl.program,"texU");
  g_gl.samplers[2]=glGetUniformLocation(g_gl.program,"texV");
  glGenTextures(3,g_gl.textures);g_gl.ready=1;return 1;
}

typedef struct {
  GLint enabled,size,type,normalized,stride,buffer;
  void *pointer;
} AttribState;

static void capture_attrib(GLuint index,AttribState *state) {
  glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_ENABLED,&state->enabled);
  glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_SIZE,&state->size);
  glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_TYPE,&state->type);
  glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,&state->normalized);
  glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_STRIDE,&state->stride);
  glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,&state->buffer);
  glGetVertexAttribPointerv(index,GL_VERTEX_ATTRIB_ARRAY_POINTER,&state->pointer);
}

static void restore_attrib(GLuint index,const AttribState *state) {
  glBindBuffer(GL_ARRAY_BUFFER,(GLuint)state->buffer);
  glVertexAttribPointer(index,state->size,(GLenum)state->type,(GLboolean)state->normalized,
                        state->stride,state->pointer);
  if(state->enabled)glEnableVertexAttribArray(index);else glDisableVertexAttribArray(index);
}

void ctr_video_render(void) {
  if(!active()||!ensure_gl())return;
  const double clock=playback_clock();const uint8_t *upload=NULL;uint64_t serial=0;
  mutexLock(&g_lock);
  while(g_count>1&&g_slots[(g_head+1)%VIDEO_QUEUE_CAP].pts<=clock+0.015){
    g_head=(g_head+1)%VIDEO_QUEUE_CAP;g_count--;condvarWakeOne(&g_can_produce);
  }
  if(g_count>0&&(!g_have_frame||g_slots[g_head].pts<=clock+0.10)){
    VideoSlot *slot=&g_slots[g_head];
    if(slot->serial!=g_uploaded_serial){upload=slot->data;serial=slot->serial;}
  }
  mutexUnlock(&g_lock);

  GLint previous_program=0,previous_buffer=0,previous_active=0,previous_fbo=0;
  GLint previous_viewport[4]={0},previous_textures[3]={0},previous_unpack=4;
  GLfloat previous_clear[4]={0};GLboolean previous_mask[4]={0};
  const GLboolean depth=glIsEnabled(GL_DEPTH_TEST),blend=glIsEnabled(GL_BLEND),
                  cull=glIsEnabled(GL_CULL_FACE),scissor=glIsEnabled(GL_SCISSOR_TEST);
  glGetIntegerv(GL_CURRENT_PROGRAM,&previous_program);glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&previous_buffer);
  glGetIntegerv(GL_ACTIVE_TEXTURE,&previous_active);glGetIntegerv(GL_FRAMEBUFFER_BINDING,&previous_fbo);
  glGetIntegerv(GL_VIEWPORT,previous_viewport);glGetIntegerv(GL_UNPACK_ALIGNMENT,&previous_unpack);
  glGetFloatv(GL_COLOR_CLEAR_VALUE,previous_clear);glGetBooleanv(GL_COLOR_WRITEMASK,previous_mask);
  for(int i=0;i<3;i++){glActiveTexture(GL_TEXTURE0+i);glGetIntegerv(GL_TEXTURE_BINDING_2D,&previous_textures[i]);}
  AttribState pos_state,uv_state;capture_attrib((GLuint)g_gl.position,&pos_state);capture_attrib((GLuint)g_gl.uv,&uv_state);

  if(upload){
    const uint8_t *planes[3]={upload,upload+(size_t)g_width*g_height,
                              upload+(size_t)g_width*g_height+(size_t)g_chroma_w*g_chroma_h};
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    for(int i=0;i<3;i++){
      const int width=i?g_chroma_w:g_width,height=i?g_chroma_h:g_height;
      glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,g_gl.textures[i]);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
      if(g_gl.width!=g_width||g_gl.height!=g_height)
        glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE,width,height,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,planes[i]);
      else glTexSubImage2D(GL_TEXTURE_2D,0,0,0,width,height,GL_LUMINANCE,GL_UNSIGNED_BYTE,planes[i]);
    }
    g_gl.width=g_width;g_gl.height=g_height;g_uploaded_serial=serial;g_have_frame=1;g_frames_shown++;
    if(g_frames_shown==1&&!have_audio())g_wall_start_ns=monotonic_ns();
  }

  glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,(GLsizei)android_native_width(),(GLsizei)android_native_height());
  glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  if(g_have_frame){
    const float video_aspect=(float)g_width/(float)g_height;
    const float screen_aspect=(float)android_native_width()/(float)android_native_height();
    float sx=1.0f,sy=1.0f;
    if(g_preserve_aspect){
      if(video_aspect<screen_aspect)sx=video_aspect/screen_aspect;
      else sy=screen_aspect/video_aspect;
    }
    const GLfloat positions[8]={-sx,-sy,sx,-sy,-sx,sy,sx,sy};
    const GLfloat texcoords[8]={0,1,1,1,0,0,1,0};
    glUseProgram(g_gl.program);glBindBuffer(GL_ARRAY_BUFFER,0);
    for(int i=0;i<3;i++){glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,g_gl.textures[i]);glUniform1i(g_gl.samplers[i],i);}
    glVertexAttribPointer(g_gl.position,2,GL_FLOAT,GL_FALSE,0,positions);glEnableVertexAttribArray(g_gl.position);
    glVertexAttribPointer(g_gl.uv,2,GL_FLOAT,GL_FALSE,0,texcoords);glEnableVertexAttribArray(g_gl.uv);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  }

  restore_attrib((GLuint)g_gl.position,&pos_state);restore_attrib((GLuint)g_gl.uv,&uv_state);
  for(int i=0;i<3;i++){glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,(GLuint)previous_textures[i]);}
  glActiveTexture((GLenum)previous_active);glUseProgram((GLuint)previous_program);
  glBindBuffer(GL_ARRAY_BUFFER,(GLuint)previous_buffer);glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)previous_fbo);
  glViewport(previous_viewport[0],previous_viewport[1],previous_viewport[2],previous_viewport[3]);
  glPixelStorei(GL_UNPACK_ALIGNMENT,previous_unpack);glClearColor(previous_clear[0],previous_clear[1],previous_clear[2],previous_clear[3]);
  glColorMask(previous_mask[0],previous_mask[1],previous_mask[2],previous_mask[3]);
  if(depth)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
  if(blend)glEnable(GL_BLEND);else glDisable(GL_BLEND);
  if(cull)glEnable(GL_CULL_FACE);else glDisable(GL_CULL_FACE);
  if(scissor)glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);
}

void ctr_video_shutdown(void) {
  if(active())finish_session(0);
  mutexLock(&g_request_lock);g_request.pending=0;mutexUnlock(&g_request_lock);
  if(g_gl.ready){glDeleteTextures(3,g_gl.textures);glDeleteProgram(g_gl.program);memset(&g_gl,0,sizeof g_gl);}
  g_completion_pending=0;
}
