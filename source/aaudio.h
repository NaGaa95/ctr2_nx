/* Minimal Android AAudio output API backed by SDL2 for Wwise. */

#ifndef CTR2_AAUDIO_H
#define CTR2_AAUDIO_H

#include <stdint.h>

typedef struct AAudioStreamBuilder AAudioStreamBuilder;
typedef struct AAudioStream AAudioStream;

int32_t AAudio_createStreamBuilder(AAudioStreamBuilder **builder);
int32_t AAudioStreamBuilder_delete(AAudioStreamBuilder *builder);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *builder, int32_t direction);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *builder, int32_t mode);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *builder, int32_t rate);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *builder, int32_t channels);
void AAudioStreamBuilder_setSamplesPerFrame(AAudioStreamBuilder *builder, int32_t channels);
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *builder, int32_t mode);
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *builder, int32_t type);
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *builder, int32_t usage);
void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder *builder, int32_t frames);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *builder, void *callback,
                                         void *user_data);
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *builder, void *callback,
                                          void *user_data);
int32_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *builder, AAudioStream **stream);

int32_t AAudioStream_close(AAudioStream *stream);
int32_t AAudioStream_requestStart(AAudioStream *stream);
int32_t AAudioStream_requestPause(AAudioStream *stream);
int32_t AAudioStream_requestStop(AAudioStream *stream);
int32_t AAudioStream_waitForStateChange(AAudioStream *stream, int32_t input_state,
                                        int32_t *next_state, int64_t timeout_ns);
int32_t AAudioStream_getState(AAudioStream *stream);
int32_t AAudioStream_getFramesPerBurst(AAudioStream *stream);
int32_t AAudioStream_getFramesPerDataCallback(AAudioStream *stream);
int32_t AAudioStream_setBufferSizeInFrames(AAudioStream *stream, int32_t frames);
int32_t AAudioStream_getBufferSizeInFrames(AAudioStream *stream);
int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream *stream);
int32_t AAudioStream_getChannelCount(AAudioStream *stream);
int32_t AAudioStream_getSamplesPerFrame(AAudioStream *stream);
int32_t AAudioStream_getSampleRate(AAudioStream *stream);
int32_t AAudioStream_getFormat(AAudioStream *stream);
int32_t AAudioStream_getPerformanceMode(AAudioStream *stream);
int32_t AAudioStream_getXRunCount(AAudioStream *stream);
const char *AAudio_convertResultToText(int32_t result);

void aaudio_shutdown(void);

#endif
