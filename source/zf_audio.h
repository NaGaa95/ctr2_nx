#ifndef ZF_AUDIO_H
#define ZF_AUDIO_H
#include <stdarg.h>
#include <stdint.h>
int zf_audio_init(void);
void zf_audio_shutdown(void);
int zf_audio_handles(const char *cls, const char *name);
void zf_audio_void_v(const char *name, const char *sig, va_list va);
void zf_audio_void_a(const char *name, const char *sig, const void *args);
unsigned zf_audio_int_v(const char *name, const char *sig, va_list va);
unsigned zf_audio_int_a(const char *name, const char *sig, const void *args);
float zf_audio_float_v(const char *name, const char *sig, va_list va);
float zf_audio_float_a(const char *name, const char *sig, const void *args);

/* SPSC PCM stream mixed by the existing SDL device for FFmpeg movie audio. */
int zf_audio_movie_begin(void);
int zf_audio_movie_queue(const int16_t *stereo, int frames);
void zf_audio_movie_end(void);
uint64_t zf_audio_movie_frames_queued(void);
uint64_t zf_audio_movie_frames_played(void);
#endif
