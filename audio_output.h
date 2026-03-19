#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include "app.h"

AudioBufferQueue *audio_buffer_queue_create(void);
void audio_buffer_queue_destroy(AudioBufferQueue *queue);
int audio_buffer_queue_put(AudioBufferQueue *queue, const uint8_t *data, int size, double pts_sec);
int audio_buffer_queue_get(AppState *app, AudioBufferQueue *queue, AudioBuffer *out, int block);
int audio_buffer_queue_size(AudioBufferQueue *queue);
void audio_buffer_queue_abort(AudioBufferQueue *queue);

void audio_buffer_unref(AudioBuffer *buf);

int audio_output_open(AppState *app);
void audio_output_close(AppState *app);

#endif