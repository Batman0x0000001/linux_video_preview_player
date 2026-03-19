#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_output.h"
#include "decoder.h"
#include "clock.h"

typedef struct AudioBufferNode
{
    AudioBuffer buf;
    struct AudioBufferNode *next;
} AudioBufferNode;

struct AudioBufferQueue
{
    AudioBufferNode *first;
    AudioBufferNode *last;
    int nb_buffers;
    int size;

    int abort_request;
    SDL_mutex *mutex;
    SDL_cond *cond;
};

static void audio_buffer_reset(AudioBuffer *buf)
{
    if (!buf) {
        return;
    }

    free(buf->data);
    buf->data = NULL;
    buf->size = 0;
    buf->pos = 0;
    buf->pts_sec = 0.0;
}

void audio_buffer_unref(AudioBuffer *buf)
{
    audio_buffer_reset(buf);
}

AudioBufferQueue *audio_buffer_queue_create(void)
{
    AudioBufferQueue *queue = (AudioBufferQueue *)calloc(1, sizeof(AudioBufferQueue));
    if (!queue) {
        return NULL;
    }

    queue->mutex = SDL_CreateMutex();
    queue->cond = SDL_CreateCond();
    if (!queue->mutex || !queue->cond) {
        if (queue->cond) {
            SDL_DestroyCond(queue->cond);
        }
        if (queue->mutex) {
            SDL_DestroyMutex(queue->mutex);
        }
        free(queue);
        return NULL;
    }

    return queue;
}

void audio_buffer_queue_destroy(AudioBufferQueue *queue)
{
    AudioBufferNode *node = NULL;
    AudioBufferNode *next = NULL;

    if (!queue) {
        return;
    }

    SDL_LockMutex(queue->mutex);

    node = queue->first;
    while (node) {
        next = node->next;
        audio_buffer_reset(&node->buf);
        free(node);
        node = next;
    }

    queue->first = NULL;
    queue->last = NULL;
    queue->nb_buffers = 0;
    queue->size = 0;

    SDL_UnlockMutex(queue->mutex);

    SDL_DestroyMutex(queue->mutex);
    SDL_DestroyCond(queue->cond);
    free(queue);
}

int audio_buffer_queue_put(AudioBufferQueue *queue, const uint8_t *data, int size, double pts_sec)
{
    AudioBufferNode *node = NULL;

    if (!queue || !data || size <= 0) {
        return AVERROR(EINVAL);
    }

    node = (AudioBufferNode *)calloc(1, sizeof(AudioBufferNode));
    if (!node) {
        return AVERROR(ENOMEM);
    }

    node->buf.data = (uint8_t *)malloc((size_t)size);
    if (!node->buf.data) {
        free(node);
        return AVERROR(ENOMEM);
    }

    memcpy(node->buf.data, data, (size_t)size);
    node->buf.size = size;
    node->buf.pos = 0;
    node->buf.pts_sec = pts_sec;
    node->next = NULL;

    SDL_LockMutex(queue->mutex);

    if (queue->abort_request) {
        SDL_UnlockMutex(queue->mutex);
        audio_buffer_reset(&node->buf);
        free(node);
        return AVERROR_EXIT;
    }

    if (!queue->last) {
        queue->first = node;
    } else {
        queue->last->next = node;
    }
    queue->last = node;

    queue->nb_buffers++;
    queue->size += size;

    SDL_CondSignal(queue->cond);
    SDL_UnlockMutex(queue->mutex);

    return 0;
}

int audio_buffer_queue_get(AppState *app, AudioBufferQueue *queue, AudioBuffer *out, int block)
{
    AudioBufferNode *node = NULL;
    int ret = 0;

    if (!app || !queue || !out) {
        return AVERROR(EINVAL);
    }

    SDL_LockMutex(queue->mutex);

    while (1) {
        if (app->quit || queue->abort_request) {
            ret = -1;
            break;
        }

        node = queue->first;
        if (node) {
            queue->first = node->next;
            if (!queue->first) {
                queue->last = NULL;
            }

            queue->nb_buffers--;
            queue->size -= node->buf.size;

            *out = node->buf;
            memset(&node->buf, 0, sizeof(node->buf));
            free(node);

            ret = 1;
            break;
        }

        if (!block) {
            ret = 0;
            break;
        }

        SDL_CondWait(queue->cond, queue->mutex);
    }

    SDL_UnlockMutex(queue->mutex);
    return ret;
}

int audio_buffer_queue_size(AudioBufferQueue *queue)
{
    int size = 0;

    if (!queue) {
        return 0;
    }

    SDL_LockMutex(queue->mutex);
    size = queue->size;
    SDL_UnlockMutex(queue->mutex);

    return size;
}

void audio_buffer_queue_abort(AudioBufferQueue *queue)
{
    if (!queue) {
        return;
    }

    SDL_LockMutex(queue->mutex);
    queue->abort_request = 1;
    SDL_CondBroadcast(queue->cond);
    SDL_UnlockMutex(queue->mutex);
}

static int min_int(int a, int b)
{
    return (a < b) ? a : b;
}

static void sdl_audio_callback(void *userdata, Uint8 *stream, int len)
{
    AppState *app = (AppState *)userdata;
    int filled = 0;
    int copied_real_audio = 0;

    if (!app || !stream || len <= 0) {
        return;
    }

    SDL_memset(stream, 0, (size_t)len);

    if (app->paused || !app->audio_buf_queue) {
        return;
    }

    while (filled < len) {
        int remain;
        int copy_len;
        AudioBuffer next_buf = {0};
        int ret;

        if (app->audio_buf_cur.pos >= app->audio_buf_cur.size) {
            audio_buffer_unref(&app->audio_buf_cur);

            ret = audio_buffer_queue_get(app, app->audio_buf_queue, &next_buf, 0);
            if (ret <= 0) {
                break;
            }

            app->audio_buf_cur = next_buf;
        }

        remain = app->audio_buf_cur.size - app->audio_buf_cur.pos;
        if (remain <= 0) {
            continue;
        }

        copy_len = min_int(len - filled, remain);
        SDL_memcpy(stream + filled,
                   app->audio_buf_cur.data + app->audio_buf_cur.pos,
                   (size_t)copy_len);

        app->audio_buf_cur.pos += copy_len;
        filled += copy_len;

        copied_real_audio = 1;
        app->audio_output_idle = 0;
        
        if (app->audio_tgt.bytes_per_sec > 0) {
            double pts_sec = app->audio_buf_cur.pts_sec +
                             (double)app->audio_buf_cur.pos /
                             (double)app->audio_tgt.bytes_per_sec;
            clock_update_audio(app, pts_sec);
        }
    }
    if (!copied_real_audio) {
        if (app->audio_decode_finished &&
            (!app->audio_buf_queue || audio_buffer_queue_size(app->audio_buf_queue) == 0) &&
            app->audio_buf_cur.pos >= app->audio_buf_cur.size) {
            app->audio_output_idle = 1;
        }
    }
}

static int audio_validate_obtained_spec(const SDL_AudioSpec *spec)
{
    if (!spec) {
        return AVERROR(EINVAL);
    }

    if (spec->format != AUDIO_S16SYS) {
        fprintf(stderr, "当前 demo 只支持 SDL 输出格式 AUDIO_S16SYS，实际得到 format=0x%x\n",
                spec->format);
        return AVERROR(EINVAL);
    }

    if (spec->channels != 1 && spec->channels != 2) {
        fprintf(stderr, "当前 demo 只支持 1 或 2 声道输出，实际得到 channels=%u\n",
                spec->channels);
        return AVERROR(EINVAL);
    }

    if (spec->freq <= 0) {
        fprintf(stderr, "无效的 SDL 音频采样率：%d\n", spec->freq);
        return AVERROR(EINVAL);
    }

    return 0;
}

int audio_output_open(AppState *app)
{
    SDL_AudioSpec desired;
    SDL_AudioSpec obtained;
    int ret;

    if (!app) {
        return AVERROR(EINVAL);
    }

    if (app->audio_stream_index < 0) {
        return 0;
    }

    app->audio_buf_queue = audio_buffer_queue_create();
    if (!app->audio_buf_queue) {
        fprintf(stderr, "创建音频 PCM 队列失败\n");
        return AVERROR(ENOMEM);
    }

    SDL_zero(desired);
    SDL_zero(obtained);

    desired.freq = app->audio_tgt.sample_rate;
    desired.format = AUDIO_S16SYS;
    desired.channels = (Uint8)app->audio_tgt.channels;
    desired.samples = 1024;
    desired.callback = sdl_audio_callback;
    desired.userdata = app;

    app->audio_dev = SDL_OpenAudioDevice(NULL,
                                         0,
                                         &desired,
                                         &obtained,
                                         SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                         SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    if (!app->audio_dev) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    ret = audio_validate_obtained_spec(&obtained);
    if (ret < 0) {
        goto fail;
    }

    app->audio_spec = obtained;
    app->audio_hw_buf_size = obtained.size;

    ret = decoder_configure_audio_target(app, obtained.freq, obtained.channels);
    if (ret < 0) {
        fprintf(stderr, "根据 SDL 实际输出参数重配音频目标格式失败\n");
        goto fail;
    }

    /*
     * 第4部分只把设备和回调搭起来，暂时不开始播。
     * 等第5部分音频解码线程接好后，再 unpause。
     */
    SDL_PauseAudioDevice(app->audio_dev, 1);

    return 0;

fail:
    audio_output_close(app);
    return ret;
}

void audio_output_close(AppState *app)
{
    if (!app) {
        return;
    }

    if (app->audio_dev) {
        SDL_LockAudioDevice(app->audio_dev);
        audio_buffer_unref(&app->audio_buf_cur);
        SDL_UnlockAudioDevice(app->audio_dev);

        SDL_CloseAudioDevice(app->audio_dev);
        app->audio_dev = 0;
    } else {
        audio_buffer_unref(&app->audio_buf_cur);
    }

    if (app->audio_buf_queue) {
        audio_buffer_queue_abort(app->audio_buf_queue);
        audio_buffer_queue_destroy(app->audio_buf_queue);
        app->audio_buf_queue = NULL;
    }

    SDL_zero(app->audio_spec);
    app->audio_hw_buf_size = 0;
}