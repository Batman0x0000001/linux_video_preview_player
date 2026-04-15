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

        /* ---------- seek 处理 ---------- */
        if (app->seek_req) {
            /*
             * av_seek_frame 参数说明：
             *   stream_index = -1  → 用 AV_TIME_BASE 时间基（微秒）定位
             *   timestamp          → seek_pos（微秒）
             *   flags              → seek_flags（向前跳 0，向后跳 AVSEEK_FLAG_BACKWARD）
             */
            int seek_ret = av_seek_frame(app->fmt_ctx, -1, app->seek_pos, app->seek_flags);
            if (seek_ret < 0) {
                fprintf(stderr, "av_seek_frame 失败: %s\n",
                        av_err2str(seek_ret));
            } else {
                /* 1. 清空旧包，避免旧数据被解码 */
                packet_queue_flush(app->video_pkt_queue);
                packet_queue_flush(app->audio_pkt_queue);

                /* 2. 插入哨兵包，通知解码线程刷新解码器 */
                packet_queue_put_flush_pkt(app->video_pkt_queue);
                if (app->audio_stream_index >= 0) {
                    packet_queue_put_flush_pkt(app->audio_pkt_queue);
                }

                /* 3. 重置结束标志，允许 decoder/control 继续工作 */
                app->demux_finished        = 0;
                app->video_decode_finished = 0;
                app->audio_decode_finished = 0;
            }

            app->seek_req = 0; //消费完 seek 请求
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