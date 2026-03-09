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

static void app_state_init(AppState *app,const char *filename){
    memset(app,0,sizeof(AppState));
    
    snprintf(app->input_filename,sizeof(app->input_filename),"%s",filename);

    app->video_stream_index=-1;

    app->window_width=960;
    app->window_height=540;

    app->frame_timer=0.0;
    app->frame_last_pts=0.0;
    app->frame_last_delay=0.04;
    app->video_clock=0.0;
    app->video_current_pts=0.0;
    app->video_current_pts_time=0;
}

static void app_state_cleanup(AppState *app){

    if (!app) {
        return;
    }

    app->quit = 1;

    if (app->demux_tid) {
        SDL_WaitThread(app->demux_tid, NULL);
        app->demux_tid = NULL;
    }

    if (app->decode_tid) {
        SDL_WaitThread(app->decode_tid, NULL);
        app->decode_tid = NULL;
    }

    frame_queue_destroy(app->video_frm_queue);
    app->video_frm_queue = NULL;

    packet_queue_destroy(app->video_pkt_queue);
    app->video_pkt_queue = NULL;

    display_destroy(app);

    if (app->sws_ctx) {
        sws_freeContext(app->sws_ctx);
        app->sws_ctx = NULL;
    }

    if (app->video_dec_ctx) {
        avcodec_free_context(&app->video_dec_ctx);
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

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init 失败: %s\n", SDL_GetError());
        goto cleanup;
    }

    app.video_pkt_queue = packet_queue_create();
    if (!app.video_pkt_queue) {
        fprintf(stderr, "创建视频包队列失败\n");
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

    ret = control_event_loop(&app);

cleanup:

    app_state_cleanup(&app);
    return ret;
}
