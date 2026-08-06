#include <SDL2/SDL.h>
#include <tremor/ivorbisfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "config.h"
#include "jni_fake.h"
#include "zf_audio.h"

#define MAX_SAMPLES 512
#define MAX_VOICES 64
#define MOVIE_RING_FRAMES (1u<<17)
typedef struct { int used,id1,id2; int16_t *pcm; uint32_t frames; char path[192]; } Sample;
typedef struct { int used; Sample *sample; uint32_t frame; int loop; float volume; } Voice;
typedef union { uint8_t z; int32_t i; int64_t j; float f; double d; void *l; } JValue;
static Sample g_samples[MAX_SAMPLES];
static Voice g_voices[MAX_VOICES];
static SDL_AudioDeviceID g_device;
static float g_music_volume=1.0f;
static int g_output_rate=48000;
static int16_t *g_movie_pcm;
static uint64_t g_movie_read,g_movie_write;
static int g_movie_active;
static int g_game_suspended;

/* CTR2's ZSoundPlayer type enum is explicit in the APK: 1 uses the music
 * player, while 0 and 2 use SoundPool. The bridge must not infer
 * this classification. */
static int sample_is_music(const Sample *sample){return sample&&sample->id2==1;}

static int16_t clamp16(int v){ if(v>32767)return 32767;if(v<-32768)return -32768;return (int16_t)v; }
static void audio_callback(void *opaque, Uint8 *stream, int bytes){
  (void)opaque; int16_t *out=(int16_t *)stream; int frames=bytes/4; memset(stream,0,(size_t)bytes);
  const int movie_active=__atomic_load_n(&g_movie_active,__ATOMIC_ACQUIRE);
  const uint64_t movie_read=__atomic_load_n(&g_movie_read,__ATOMIC_RELAXED);
  const uint64_t movie_write=__atomic_load_n(&g_movie_write,__ATOMIC_ACQUIRE);
  uint64_t movie_available=movie_write-movie_read;
  if(movie_available>(uint64_t)frames)movie_available=(uint64_t)frames;
  for(int f=0;f<frames;f++){
    int l=0,r=0;
    if(!__atomic_load_n(&g_game_suspended,__ATOMIC_ACQUIRE))
      for(int i=0;i<MAX_VOICES;i++)if(g_voices[i].used){ Voice *v=&g_voices[i]; if(v->frame>=v->sample->frames){if(v->loop)v->frame=0;else{v->used=0;continue;}} const int16_t *p=v->sample->pcm+v->frame*2; float vol=v->volume*(sample_is_music(v->sample)?g_music_volume:1.0f); l+=(int)(p[0]*vol);r+=(int)(p[1]*vol);v->frame++; }
    if(movie_active&&g_movie_pcm&&(uint64_t)f<movie_available){const int16_t *p=&g_movie_pcm[((movie_read+(uint64_t)f)&(MOVIE_RING_FRAMES-1))*2];l+=p[0];r+=p[1];}
    out[f*2]=clamp16(l);out[f*2+1]=clamp16(r);
  }
  if(movie_active&&movie_available)
    __atomic_store_n(&g_movie_read,movie_read+movie_available,__ATOMIC_RELEASE);
}

int zf_audio_init(void){
  SDL_AudioSpec want={0},got={0}; want.freq=48000;want.format=AUDIO_S16SYS;want.channels=2;want.samples=1024;want.callback=audio_callback;
  g_device=SDL_OpenAudioDevice(NULL,0,&want,&got,0);if(!g_device)return 0;
  g_output_rate=got.freq>0?got.freq:48000;
  g_movie_pcm=malloc((size_t)MOVIE_RING_FRAMES*2*sizeof(*g_movie_pcm));
  SDL_PauseAudioDevice(g_device,0);return 1;
}

int zf_audio_movie_begin(void){
  if(!g_device||!g_movie_pcm)return 0;
  SDL_LockAudioDevice(g_device);
  __atomic_store_n(&g_movie_read,0,__ATOMIC_RELAXED);
  __atomic_store_n(&g_movie_write,0,__ATOMIC_RELAXED);
  __atomic_store_n(&g_movie_active,1,__ATOMIC_RELEASE);
  SDL_UnlockAudioDevice(g_device);
  return g_output_rate;
}

int zf_audio_movie_queue(const int16_t *stereo,int frames){
  if(!stereo||frames<=0)return 1;
  int offset=0;
  while(offset<frames){
    if(!__atomic_load_n(&g_movie_active,__ATOMIC_ACQUIRE))return 0;
    const uint64_t read=__atomic_load_n(&g_movie_read,__ATOMIC_ACQUIRE);
    uint64_t write=__atomic_load_n(&g_movie_write,__ATOMIC_RELAXED);
    const uint64_t used=write-read;
    if(used>=MOVIE_RING_FRAMES){SDL_Delay(1);continue;}
    uint64_t count=(uint64_t)(frames-offset);
    const uint64_t free_frames=MOVIE_RING_FRAMES-used;
    if(count>free_frames)count=free_frames;
    const uint64_t index=write&(MOVIE_RING_FRAMES-1);
    uint64_t first=MOVIE_RING_FRAMES-index;
    if(first>count)first=count;
    memcpy(&g_movie_pcm[index*2],&stereo[(size_t)offset*2],(size_t)first*2*sizeof(*stereo));
    if(first<count)
      memcpy(g_movie_pcm,&stereo[(size_t)(offset+(int)first)*2],(size_t)(count-first)*2*sizeof(*stereo));
    write+=count;offset+=(int)count;
    __atomic_store_n(&g_movie_write,write,__ATOMIC_RELEASE);
  }
  return 1;
}

void zf_audio_movie_end(void){
  if(g_device)SDL_LockAudioDevice(g_device);
  __atomic_store_n(&g_movie_active,0,__ATOMIC_RELEASE);
  __atomic_store_n(&g_movie_read,0,__ATOMIC_RELEASE);
  __atomic_store_n(&g_movie_write,0,__ATOMIC_RELEASE);
  if(g_device)SDL_UnlockAudioDevice(g_device);
}

uint64_t zf_audio_movie_frames_queued(void){return __atomic_load_n(&g_movie_write,__ATOMIC_ACQUIRE);}
uint64_t zf_audio_movie_frames_played(void){return __atomic_load_n(&g_movie_read,__ATOMIC_ACQUIRE);}

static Sample *sample_for(int a,int b){(void)b;for(int i=0;i<MAX_SAMPLES;i++)if(g_samples[i].used&&g_samples[i].id1==a)return &g_samples[i];return NULL;}
static Sample *sample_slot(void){for(int i=0;i<MAX_SAMPLES;i++)if(!g_samples[i].used)return &g_samples[i];return NULL;}

static int decode_ogg(const char *name,int16_t **pcm,uint32_t *frames){
  char path[768]; while(name&&*name=='/')name++; if(!name||strstr(name,"..")||strchr(name,':'))return 0;
  if(!strncmp(name,"assets/",7))snprintf(path,sizeof path,"%s/%s",GAME_HOME,name);else snprintf(path,sizeof path,"%s/assets/%s",GAME_HOME,name);
  size_t name_len=strlen(name);
  if(name_len>=4&&!strcasecmp(name+name_len-4,".wav")){
    SDL_AudioSpec spec;Uint8 *wave=NULL;Uint32 wave_len=0;
    if(!SDL_LoadWAV(path,&spec,&wave,&wave_len))return 0;
    SDL_AudioCVT cvt;if(SDL_BuildAudioCVT(&cvt,spec.format,spec.channels,spec.freq,AUDIO_S16SYS,2,48000)<0){SDL_FreeWAV(wave);return 0;}
    size_t cap=(size_t)wave_len*(size_t)(cvt.needed?cvt.len_mult:1);Uint8 *raw=malloc(cap?cap:1);if(!raw){SDL_FreeWAV(wave);return 0;}memcpy(raw,wave,wave_len);SDL_FreeWAV(wave);size_t len=wave_len;
    if(cvt.needed){cvt.buf=raw;cvt.len=(int)wave_len;if(SDL_ConvertAudio(&cvt)<0){free(raw);return 0;}len=(size_t)cvt.len_cvt;}
    *pcm=(int16_t*)raw;*frames=(uint32_t)(len/4);return *frames>0;
  }
  FILE *f=fopen(path,"rb");if(!f)return 0;
  OggVorbis_File vf;memset(&vf,0,sizeof vf);if(ov_open(f,&vf,NULL,0)<0){fclose(f);return 0;}
  vorbis_info *info=ov_info(&vf,-1);if(!info||info->channels<1||info->channels>2||info->rate<=0){ov_clear(&vf);return 0;}
  size_t cap=65536,len=0;uint8_t *raw=malloc(cap);int section=0;long got;
  while(raw&&(got=ov_read(&vf,(char *)raw+len,(int)(cap-len),&section))!=0){if(got<0)continue;len+=(size_t)got;if(cap-len<32768){size_t nc=cap*2;uint8_t *nr=realloc(raw,nc);if(!nr){free(raw);raw=NULL;break;}raw=nr;cap=nc;}}
  int channels=info->channels,rate=info->rate;ov_clear(&vf);if(!raw||!len){free(raw);return 0;}
  SDL_AudioCVT cvt;if(SDL_BuildAudioCVT(&cvt,AUDIO_S16SYS,(Uint8)channels,rate,AUDIO_S16SYS,2,48000)<0){free(raw);return 0;}
  if(cvt.needed){uint8_t *converted=malloc(len*(size_t)cvt.len_mult);if(!converted){free(raw);return 0;}memcpy(converted,raw,len);free(raw);cvt.buf=converted;cvt.len=(int)len;if(SDL_ConvertAudio(&cvt)<0){free(converted);return 0;}raw=converted;len=(size_t)cvt.len_cvt;}
  *pcm=(int16_t *)raw;*frames=(uint32_t)(len/4);return *frames>0;
}

static void load_sample(int id1,int id2,const char *path){
  if(!path||!*path)return;
  if(sample_for(id1,id2))return;
  Sample *s=sample_slot();if(!s)return;
  if(g_device)SDL_LockAudioDevice(g_device);
  s->used=1;s->id1=id1;s->id2=id2;s->pcm=NULL;s->frames=0;snprintf(s->path,sizeof s->path,"%s",path);
  if(g_device)SDL_UnlockAudioDevice(g_device);
}

static void play_sample(int id1,int id2,int loop,float volume){
  Sample *s=sample_for(id1,id2);
  if(!s)return;

  /* Android SoundPool.load() queues work asynchronously.  Decode only when a
   * registered sound is first played so nativeViewCreated is not held up by
   * the full CTR2 sound catalogue.  No voice can reference the sample until
   * after this completes, so the mixer never observes a partial sample. */
  if(!s->pcm){
    int16_t *pcm=NULL;
    uint32_t frames=0;
    if(!decode_ogg(s->path,&pcm,&frames))return;
    s->pcm=pcm;
    s->frames=frames;
  }

  if(volume<0)volume=0;
  if(volume>2)volume=2;
  if(g_device)SDL_LockAudioDevice(g_device);
  if(sample_is_music(s)){
    for(int i=0;i<MAX_VOICES;i++)if(g_voices[i].used&&sample_is_music(g_voices[i].sample)){
      if(g_device)SDL_UnlockAudioDevice(g_device);
      return;
    }
  }
  for(int i=0;i<MAX_VOICES;i++)if(!g_voices[i].used){
    const int should_loop=sample_is_music(s)||(loop>0||loop==-1);
    g_voices[i]=(Voice){1,s,0,should_loop,volume};
    break;
  }
  if(g_device)SDL_UnlockAudioDevice(g_device);
}
static void stop_sample(int a,int b){(void)b;if(g_device)SDL_LockAudioDevice(g_device);for(int i=0;i<MAX_VOICES;i++)if(g_voices[i].used&&g_voices[i].sample->id1==a)g_voices[i].used=0;if(g_device)SDL_UnlockAudioDevice(g_device);}
static void stop_all(void){if(g_device)SDL_LockAudioDevice(g_device);memset(g_voices,0,sizeof g_voices);if(g_device)SDL_UnlockAudioDevice(g_device);}
static void stop_type(int music){if(g_device)SDL_LockAudioDevice(g_device);for(int i=0;i<MAX_VOICES;i++)if(g_voices[i].used&&(sample_is_music(g_voices[i].sample)==music))g_voices[i].used=0;if(g_device)SDL_UnlockAudioDevice(g_device);}
static void set_volume(int id,float vol){if(vol<0)vol=0;if(vol>2)vol=2;Sample *sample=sample_for(id,0);if(sample_is_music(sample)){g_music_volume=vol;return;}if(g_device)SDL_LockAudioDevice(g_device);for(int i=0;i<MAX_VOICES;i++)if(g_voices[i].used&&g_voices[i].sample->id1==id)g_voices[i].volume=vol;if(g_device)SDL_UnlockAudioDevice(g_device);}
static void suspend_game_audio(int suspended){
  __atomic_store_n(&g_game_suspended,suspended!=0,__ATOMIC_RELEASE);
}

int zf_audio_handles(const char *cls,const char *name){
  if(!name)return 0;
  if(cls&&(strstr(cls,"ZSoundPlayer")||strstr(cls,"SoundPlayer")))return 1;
  return !strcmp(name,"loadState")||!strcmp(name,"stopAllEffects")||!strcmp(name,"stopAllSounds")||!strcmp(name,"stopMusic")||!strcmp(name,"getMusicVolume")||!strcmp(name,"setMusicVolume");
}
void zf_audio_void_v(const char *name,const char *sig,va_list va){(void)sig;
  if(!strcmp(name,"load")){int a=va_arg(va,int),b=va_arg(va,int);const char *p=jni_string_utf(va_arg(va,void*));load_sample(a,b,p);}
  else if(!strcmp(name,"play")){int a=va_arg(va,int),b=va_arg(va,int);float v=(float)va_arg(va,double);play_sample(a,b,b,v);}
  else if(!strcmp(name,"stop")){int a=va_arg(va,int),b=va_arg(va,int);stop_sample(a,b);}
  else if(!strcmp(name,"setVolume")){int a=va_arg(va,int);float v=(float)va_arg(va,double);set_volume(a,v);}
  else if(!strcmp(name,"setMusicVolume")){g_music_volume=(float)va_arg(va,double);if(g_music_volume<0)g_music_volume=0;if(g_music_volume>2)g_music_volume=2;}
  else if(!strcmp(name,"suspend"))suspend_game_audio(1);
  else if(!strcmp(name,"resume"))suspend_game_audio(0);
  else if(!strcmp(name,"stopAllEffects"))stop_type(0);
  else if(!strcmp(name,"stopMusic"))stop_type(1);
  else if(!strcmp(name,"stopAllSounds"))stop_all();
}
void zf_audio_void_a(const char *name,const char *sig,const void *args){(void)sig;const JValue *a=args;if(!a)return;
  if(!strcmp(name,"load"))load_sample(a[0].i,a[1].i,jni_string_utf(a[2].l));
  else if(!strcmp(name,"play"))play_sample(a[0].i,a[1].i,a[1].i,a[2].f);
  else if(!strcmp(name,"stop"))stop_sample(a[0].i,a[1].i);
  else if(!strcmp(name,"setVolume"))set_volume(a[0].i,a[1].f);
  else if(!strcmp(name,"setMusicVolume")){g_music_volume=a[0].f;if(g_music_volume<0)g_music_volume=0;if(g_music_volume>2)g_music_volume=2;}
  else if(!strcmp(name,"suspend"))suspend_game_audio(1);
  else if(!strcmp(name,"resume"))suspend_game_audio(0);
  else if(!strcmp(name,"stopAllEffects"))stop_type(0);
  else if(!strcmp(name,"stopMusic"))stop_type(1);
  else if(!strcmp(name,"stopAllSounds"))stop_all();
}
unsigned zf_audio_int_v(const char *name,const char *sig,va_list va){(void)sig;if(!strcmp(name,"loadState"))return (unsigned)va_arg(va,int);return 0;}
unsigned zf_audio_int_a(const char *name,const char *sig,const void *args){(void)sig;const JValue *a=args;if(a&&!strcmp(name,"loadState"))return (unsigned)a[0].i;return 0;}
float zf_audio_float_v(const char *name,const char *sig,va_list va){(void)sig;(void)va;return !strcmp(name,"getMusicVolume")?g_music_volume:0.0f;}
float zf_audio_float_a(const char *name,const char *sig,const void *args){(void)sig;(void)args;return !strcmp(name,"getMusicVolume")?g_music_volume:0.0f;}
void zf_audio_shutdown(void){stop_all();zf_audio_movie_end();__atomic_store_n(&g_game_suspended,0,__ATOMIC_RELEASE);if(g_device){SDL_CloseAudioDevice(g_device);g_device=0;}free(g_movie_pcm);g_movie_pcm=NULL;for(int i=0;i<MAX_SAMPLES;i++)free(g_samples[i].pcm);memset(g_samples,0,sizeof g_samples);}
