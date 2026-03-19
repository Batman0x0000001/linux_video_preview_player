#include<stdio.h>
#include<stdlib.h>
#include<string.h>

#include"app.h"
#include"demux.h"
#include"decoder.h"
#include"frame_queue.h"
#include"display.h"
#include"clock.h"
#include"control.h"
#include<libavutil/time.h>
#include "audio_output.h"

static void app_state_init(AppState *app,const char *filename){
    memset(app,0,sizeof(AppState));
    
    snprintf(app->input_filename,sizeof(app->input_filename),"%s",filename);

    app->video_stream_index=-1;

    app->window_width=960;
    app->window_height=540;

    app->frame_timer=av_gettime_relative() / 1000000.0;
    app->frame_last_pts=0.0;
    app->frame_last_delay=0.04;
    app->video_clock=0.0;
    app->video_current_pts=0.0;
    app->video_current_pts_time=0;

    app->audio_clock = 0;
    app->audio_hw_buf_size = 0;
    app->audio_callback_time = 0;

    app->demux_finished = 0;
    app->video_decode_finished = 0;
    app->audio_decode_finished = 0;
    app->audio_output_idle = 1;
}

static void app_state_cleanup(AppState *app){

    if (!app) {
        return;
    }

    app->quit = 1;

    packet_queue_abort(app->video_pkt_queue);
    packet_queue_abort(app->audio_pkt_queue);
    frame_queue_abort(app->video_frm_queue);
    audio_buffer_queue_abort(app->audio_buf_queue);

    if (app->demux_tid) {
        SDL_WaitThread(app->demux_tid, NULL);
        app->demux_tid = NULL;
    }

    if (app->decode_tid) {
        SDL_WaitThread(app->decode_tid, NULL);
        app->decode_tid = NULL;
    }

    if (app->audio_decode_tid) {
        SDL_WaitThread(app->audio_decode_tid, NULL);
        app->audio_decode_tid = NULL;
    }

    audio_output_close(app);
    display_destroy(app);

    frame_queue_destroy(app->video_frm_queue);
    app->video_frm_queue = NULL;

    packet_queue_destroy(app->video_pkt_queue);
    app->video_pkt_queue = NULL;

    packet_queue_destroy(app->audio_pkt_queue);
    app->audio_pkt_queue = NULL;

    if (app->sws_ctx) {
        sws_freeContext(app->sws_ctx);
        app->sws_ctx = NULL;
    }

    if (app->swr_ctx) {
        swr_free(&app->swr_ctx);
    }

    av_channel_layout_uninit(&app->audio_src.ch_layout);
    memset(&app->audio_src, 0, sizeof(app->audio_src));

    av_channel_layout_uninit(&app->audio_tgt.ch_layout);
    memset(&app->audio_tgt, 0, sizeof(app->audio_tgt));

    if (app->video_dec_ctx) {
        avcodec_free_context(&app->video_dec_ctx);
    }

    if (app->audio_dec_ctx) {
        avcodec_free_context(&app->audio_dec_ctx);
    }

    if (app->fmt_ctx) {
        avformat_close_input(&app->fmt_ctx);
    }

    SDL_Quit();
}

int main(int argc, char const *argv[])
{
    AppState app;
    int ret=-1;

    if(argc<2){
        fprintf(stderr, "用法: %s <input_video>\n", argv[0]);
        exit(1);
    }

    app_state_init(&app,argv[1]);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init 失败: %s\n", SDL_GetError());
        goto cleanup;
    }

    app.video_pkt_queue = packet_queue_create();
    if (!app.video_pkt_queue) {
        fprintf(stderr, "创建视频包队列失败\n");
        goto cleanup;
    }

    app.audio_pkt_queue = packet_queue_create();
    if (!app.audio_pkt_queue) {
        fprintf(stderr, "创建音频包队列失败\n");
        goto cleanup;
    }

    app.video_frm_queue = frame_queue_create(32);
    if (!app.video_frm_queue) {
        fprintf(stderr, "创建视频帧队列失败\n");
        goto cleanup;
    }

    if (demux_open_input(&app) < 0) {
        fprintf(stderr, "打开输入失败\n");
        goto cleanup;
    }

    if (decoder_open_video(&app) < 0) {
        fprintf(stderr, "打开视频解码器失败\n");
        goto cleanup;
    }

    if (app.audio_stream_index >= 0){
        if (decoder_open_audio(&app) < 0) {
            fprintf(stderr, "打开音频解码器失败\n");
            goto cleanup;
        }
        if (audio_output_open(&app) < 0) {
            fprintf(stderr, "初始化音频输出失败\n");
            goto cleanup;
        }
    }

    if (display_init(&app) < 0) {
        fprintf(stderr, "初始化显示失败\n");
        goto cleanup;
    }

    if (decoder_start(&app) < 0) {
        fprintf(stderr, "启动解码线程失败\n");
        goto cleanup;
    }

    if (demux_start(&app) < 0) {
        fprintf(stderr, "启动读包线程失败\n");
        goto cleanup;
    }

    if (app.audio_dev) {
        SDL_PauseAudioDevice(app.audio_dev, 0);
    }

    ret = control_event_loop(&app);

cleanup:

    app_state_cleanup(&app);
    return ret;
}
