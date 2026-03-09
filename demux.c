#include<stdio.h>
#include"demux.h"
#include "frame_queue.h"

static void dump_input_info(AppState *app){
    av_dump_format(app->fmt_ctx,0,app->input_filename,0);
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
        if(app->video_pkt_queue && packet_queue_size(app->video_pkt_queue) > (2*1024*1024)){
            SDL_Delay(10);
            continue;
        }

        ret = av_read_frame(app->fmt_ctx,pkt);
        if(ret < 0){
            if(ret == AVERROR_EOF){
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
        }
    }
    av_packet_unref(pkt);
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