/* AAudio compatibility for Wwise 2021.1.
 *
 * Wwise loads libaaudio.so dynamically and renders from an AAudio data callback.
 * The Switch host has no Android audio service, so this supplies the negotiated
 * stream properties and drives that callback from an SDL2 audio device.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <SDL2/SDL.h>

#include "aaudio.h"
#include "libc_shim.h"
#include "util.h"

enum {
  AAUDIO_OK = 0,
  AAUDIO_ERROR_NULL = -901,
  AAUDIO_ERROR_UNAVAILABLE = -899,
  AAUDIO_FORMAT_PCM_I16 = 1,
  AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12,
  AAUDIO_STREAM_STATE_OPEN = 2,
  AAUDIO_STREAM_STATE_STARTED = 4,
  AAUDIO_STREAM_STATE_PAUSED = 6,
  AAUDIO_STREAM_STATE_STOPPED = 10,
  AAUDIO_STREAM_STATE_CLOSED = 12,
  AAUDIO_CALLBACK_RESULT_CONTINUE = 0,
};

typedef int32_t (*AAudioDataCallback)(AAudioStream *, void *, void *, int32_t);
typedef void (*AAudioErrorCallback)(AAudioStream *, void *, int32_t);

struct AAudioStreamBuilder {
  int32_t direction;
  int32_t performance_mode;
  int32_t sample_rate;
  int32_t channels;
  int32_t sharing_mode;
  int32_t content_type;
  int32_t usage;
  int32_t capacity_frames;
  AAudioDataCallback data_callback;
  void *data_user;
  AAudioErrorCallback error_callback;
  void *error_user;
};

struct AAudioStream {
  SDL_AudioDeviceID device;
  int32_t state;
  int32_t sample_rate;
  int32_t channels;
  int32_t format;
  int32_t performance_mode;
  int32_t frames_per_burst;
  int32_t callback_frames;
  int32_t capacity_frames;
  int32_t buffer_frames;
  AAudioDataCallback data_callback;
  void *data_user;
  AAudioErrorCallback error_callback;
  void *error_user;
};

#define MAX_AAUDIO_STREAMS 4
static AAudioStream *g_streams[MAX_AAUDIO_STREAMS];

static void SDLCALL aaudio_sdl_callback(void *userdata, Uint8 *output, int bytes) {
  AAudioStream *stream = userdata;
  static uint8_t audio_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  static int tls_ready;
  if (!tls_ready) {
    install_bionic_tls(audio_tls);
    tls_ready = 1;
  }

  memset(output, 0, (size_t)bytes);
  if (!stream || stream->state != AAUDIO_STREAM_STATE_STARTED ||
      !stream->data_callback || stream->channels <= 0)
    return;

  const int32_t frames = bytes / (stream->channels * (int32_t)sizeof(int16_t));
  if (frames <= 0)
    return;
  const int32_t result = stream->data_callback(stream, stream->data_user,
                                                output, frames);
  if (result != AAUDIO_CALLBACK_RESULT_CONTINUE) {
    stream->state = AAUDIO_STREAM_STATE_STOPPED;
    memset(output, 0, (size_t)bytes);
  }
}

int32_t AAudio_createStreamBuilder(AAudioStreamBuilder **out) {
  if (!out) return AAUDIO_ERROR_NULL;
  AAudioStreamBuilder *builder = calloc(1, sizeof(*builder));
  if (!builder) return AAUDIO_ERROR_UNAVAILABLE;
  builder->performance_mode = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
  builder->sample_rate = 48000;
  builder->channels = 2;
  builder->capacity_frames = 4096;
  *out = builder;
  return AAUDIO_OK;
}

int32_t AAudioStreamBuilder_delete(AAudioStreamBuilder *builder) {
  free(builder);
  return AAUDIO_OK;
}

void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, int32_t v) { if (b) b->direction = v; }
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, int32_t v) { if (b) b->performance_mode = v; }
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t v) { if (b && v > 0) b->sample_rate = v; }
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t v) { if (b && v > 0) b->channels = v; }
void AAudioStreamBuilder_setSamplesPerFrame(AAudioStreamBuilder *b, int32_t v) { if (b && v > 0) b->channels = v; }
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, int32_t v) { if (b) b->sharing_mode = v; }
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *b, int32_t v) { if (b) b->content_type = v; }
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, int32_t v) { if (b) b->usage = v; }
void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder *b, int32_t v) {
  if (b && v > 0) b->capacity_frames = v;
}
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, void *cb, void *user) {
  if (b) { b->data_callback = (AAudioDataCallback)cb; b->data_user = user; }
}
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, void *cb, void *user) {
  if (b) { b->error_callback = (AAudioErrorCallback)cb; b->error_user = user; }
}

int32_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *builder,
                                       AAudioStream **out) {
  if (!builder || !out) return AAUDIO_ERROR_NULL;
  if (builder->direction != 0) return AAUDIO_ERROR_UNAVAILABLE;

  AAudioStream *stream = calloc(1, sizeof(*stream));
  if (!stream) return AAUDIO_ERROR_UNAVAILABLE;
  stream->sample_rate = builder->sample_rate > 0 ? builder->sample_rate : 48000;
  stream->channels = builder->channels > 0 ? builder->channels : 2;
  if (stream->channels > 2) stream->channels = 2;
  stream->format = AAUDIO_FORMAT_PCM_I16;
  stream->performance_mode = builder->performance_mode;
  stream->capacity_frames = builder->capacity_frames > 0 ? builder->capacity_frames : 4096;
  stream->data_callback = builder->data_callback;
  stream->data_user = builder->data_user;
  stream->error_callback = builder->error_callback;
  stream->error_user = builder->error_user;

  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = stream->sample_rate;
  want.format = AUDIO_S16SYS;
  want.channels = (Uint8)stream->channels;
  /* Wwise mixing at a 256-frame cadence is too aggressive while Unity streams
   * assets from the SD card. A 1024-frame device period keeps rendering stable
   * without adding noticeable game-audio latency. */
  want.samples = 1024;
  want.callback = aaudio_sdl_callback;
  want.userdata = stream;
  stream->device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!stream->device) {
    free(stream);
    return AAUDIO_ERROR_UNAVAILABLE;
  }

  stream->sample_rate = have.freq;
  stream->channels = have.channels;
  stream->frames_per_burst = have.samples;
  stream->callback_frames = have.samples;
  if (stream->capacity_frames < stream->frames_per_burst * 2)
    stream->capacity_frames = stream->frames_per_burst * 2;
  stream->buffer_frames = stream->capacity_frames;
  stream->state = AAUDIO_STREAM_STATE_OPEN;
  SDL_PauseAudioDevice(stream->device, 1);

  for (int i = 0; i < MAX_AAUDIO_STREAMS; ++i) {
    if (!g_streams[i]) { g_streams[i] = stream; break; }
  }
  *out = stream;
  return AAUDIO_OK;
}

int32_t AAudioStream_requestStart(AAudioStream *stream) {
  if (!stream || !stream->device) return AAUDIO_ERROR_NULL;
  stream->state = AAUDIO_STREAM_STATE_STARTED;
  SDL_PauseAudioDevice(stream->device, 0);
  return AAUDIO_OK;
}

int32_t AAudioStream_requestPause(AAudioStream *stream) {
  if (!stream || !stream->device) return AAUDIO_ERROR_NULL;
  SDL_PauseAudioDevice(stream->device, 1);
  stream->state = AAUDIO_STREAM_STATE_PAUSED;
  return AAUDIO_OK;
}

int32_t AAudioStream_requestStop(AAudioStream *stream) {
  if (!stream || !stream->device) return AAUDIO_ERROR_NULL;
  SDL_PauseAudioDevice(stream->device, 1);
  stream->state = AAUDIO_STREAM_STATE_STOPPED;
  return AAUDIO_OK;
}

int32_t AAudioStream_close(AAudioStream *stream) {
  if (!stream) return AAUDIO_ERROR_NULL;
  if (stream->device) {
    SDL_PauseAudioDevice(stream->device, 1);
    SDL_CloseAudioDevice(stream->device);
    stream->device = 0;
  }
  stream->state = AAUDIO_STREAM_STATE_CLOSED;
  return AAUDIO_OK;
}

int32_t AAudioStream_waitForStateChange(AAudioStream *stream, int32_t input,
                                        int32_t *next, int64_t timeout_ns) {
  (void)input; (void)timeout_ns;
  if (!stream) return AAUDIO_ERROR_NULL;
  if (next) *next = stream->state;
  return AAUDIO_OK;
}

int32_t AAudioStream_getState(AAudioStream *s) { return s ? s->state : 0; }
int32_t AAudioStream_getFramesPerBurst(AAudioStream *s) { return s ? s->frames_per_burst : 0; }
int32_t AAudioStream_getFramesPerDataCallback(AAudioStream *s) { return s ? s->callback_frames : 0; }
int32_t AAudioStream_getBufferSizeInFrames(AAudioStream *s) { return s ? s->buffer_frames : 0; }
int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream *s) { return s ? s->capacity_frames : 0; }
int32_t AAudioStream_getChannelCount(AAudioStream *s) { return s ? s->channels : 0; }
int32_t AAudioStream_getSamplesPerFrame(AAudioStream *s) { return s ? s->channels : 0; }
int32_t AAudioStream_getSampleRate(AAudioStream *s) { return s ? s->sample_rate : 0; }
int32_t AAudioStream_getFormat(AAudioStream *s) { return s ? s->format : 0; }
int32_t AAudioStream_getPerformanceMode(AAudioStream *s) { return s ? s->performance_mode : 0; }
int32_t AAudioStream_getXRunCount(AAudioStream *s) { (void)s; return 0; }

int32_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t frames) {
  if (!s) return AAUDIO_ERROR_NULL;
  if (frames < s->frames_per_burst) frames = s->frames_per_burst;
  if (frames > s->capacity_frames) frames = s->capacity_frames;
  s->buffer_frames = frames;
  return frames;
}

const char *AAudio_convertResultToText(int32_t result) {
  return result == AAUDIO_OK ? "AAUDIO_OK" : "AAUDIO_ERROR_UNAVAILABLE";
}

void aaudio_shutdown(void) {
  for (int i = 0; i < MAX_AAUDIO_STREAMS; ++i) {
    if (!g_streams[i]) continue;
    AAudioStream_close(g_streams[i]);
    free(g_streams[i]);
    g_streams[i] = NULL;
  }
}
