#include<stdio.h>
#include"demux.h"
#include "frame_queue.h"
#define MAX_VIDEOQ_SIZE (2 * 1024 * 1024)
#define MAX_AUDIOQ_SIZE (512 * 1024)

static void dump_input_info(AppState *app){
    av_dump_format(app->fmt_ctx,0,app->input_filename,0);
}

static int demux_find_streams(AppState *app) {
    int ret;

    ret = av_find_best_stream(app->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &app->video_dec, 0);
    if (ret < 0) {
        fprintf(stderr, "找不到视频流\n");
        return ret;
    }

    app->video_stream_index = ret;
    app->video_stream = app->fmt_ctx->streams[app->video_stream_index];

    ret = av_find_best_stream(app->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &app->audio_dec, 0);
    if (ret >= 0) {
        app->audio_stream_index = ret;
        app->audio_stream = app->fmt_ctx->streams[app->audio_stream_index];
    } else {
        app->audio_stream_index = -1;
        app->audio_stream = NULL;
        app->audio_dec = NULL;
        fprintf(stderr, "提示：当前输入没有可用音频流，后续将按纯视频路径运行\n");
    }

    return 0;
}

static int demux_thread(void *arg){
    AppState *app = (AppState *)arg;
    AVPacket *pkt = NULL;
    int ret = 0;

    pkt = av_packet_alloc();
    if(!pkt){
        return AVERROR(ENOMEM);
    }

    while(1){
        if(app->quit){
            break;
        }

        //app.h里把 PacketQueue 设计成了不透明类型。这说明现在还需要给队列模块补一个查询接口，不然 demux 模块不应该知道队列内部结构。
        if(app->video_pkt_queue && packet_queue_size(app->video_pkt_queue) > MAX_VIDEOQ_SIZE){
            SDL_Delay(10);
            continue;
        }

        ret = av_read_frame(app->fmt_ctx,pkt);
        if(ret < 0){
            if(ret == AVERROR_EOF){
                app->demux_finished = 1;
                packet_queue_abort(app->video_pkt_queue);
                packet_queue_abort(app->audio_pkt_queue);
                break;
            }
            SDL_Delay(10);
            continue;
        }

        if(pkt->stream_index == app->video_stream_index){
            ret = packet_queue_put(app->video_pkt_queue,pkt);
            if(ret < 0){
                av_packet_unref(pkt);
                break;
            }
            continue;
        }

        if (pkt->stream_index == app->audio_stream_index) {
            ret = packet_queue_put(app->audio_pkt_queue, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                break;
            }
            continue;
        }

        av_packet_unref(pkt);
    }

    if (!app->demux_finished && !app->quit) {
        app->demux_finished = 1;
        packet_queue_abort(app->video_pkt_queue);
        packet_queue_abort(app->audio_pkt_queue);
    }
    
    if(pkt){
        av_packet_free(&pkt);
    }
    return 0;
}

int demux_open_input(AppState *app)
{
    int ret;

    ret = avformat_open_input(&app->fmt_ctx,app->input_filename,NULL,NULL);
    if(ret < 0){
        return ret;
    }

    ret = avformat_find_stream_info(app->fmt_ctx,NULL);
    if(ret < 0){
        return ret;
    }

    ret = demux_find_streams(app);
    if(ret < 0){
        return ret;
    }

    dump_input_info(app);
    return 0;
}

int demux_start(AppState *app)
{
    app->demux_tid = SDL_CreateThread(demux_thread,"demux_thread",app);
    if(!app->demux_tid){
        fprintf(stderr, "SDL_CreateThread failed:%s\n", SDL_GetError());
        return -1;
    }

    return 0;
}