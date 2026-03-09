#include <stdio.h>
#include "decoder.h"
#include"frame_queue.h"

static void print_ffmpeg_error(const char *msg,int errnum){
    char errbuf[AV_ERROR_MAX_STRING_SIZE]={0};
    av_strerror(errnum,errbuf,sizeof(errbuf));
    fprintf(stderr,"%s:%s\n",msg,errbuf);
}

static int queue_decode_frame(AppState *app,const AVFrame *src_frame,double pts){
    VideoFrame *vf = NULL;
    int ret;

    ret = frame_queue_peek_writable(app,app->video_frm_queue,&vf);
    if(ret < 0){
        if(app->quit){
            return AVERROR_EXIT;
        }
        return ret;
    }
    if(!vf||!vf->frame){
        return AVERROR(EINVAL);
    }

    if(vf->frame->width != app->video_dec_ctx->width ||
        vf->frame->height != app->video_dec_ctx->height ||
        vf->frame->format != AV_PIX_FMT_YUV420P ||
        vf->frame->buf[0] == NULL){

        av_frame_unref(vf->frame);
        vf->frame->format = AV_PIX_FMT_YUV420P;
        vf->frame->width = app->video_dec_ctx->width;
        vf->frame->height = app->video_dec_ctx->height;

        ret = av_frame_get_buffer(vf->frame,0);
        if(ret < 0){
            return ret;
        }
    }

    ret = av_frame_make_writable(vf->frame);
    if(ret < 0){
        return ret;
    }

    sws_scale(app->sws_ctx,
        (const uint8_t * const *)src_frame->data,
        src_frame->linesize,
        0,
        app->video_dec_ctx->height,
        vf->frame->data,
        vf->frame->linesize);
    
    vf->pts_sec = pts;
    frame_queue_push(app->video_frm_queue);

    return 0;
}

static int decoder_thread(void *arg){
    AppState *app = (AppState *)arg;
    AVPacket *pkt = NULL;
    AVFrame *frame =NULL;
    int ret = 0;

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    while(1){
        if(app->quit){
            break;
        }

        ret = packet_queue_get(app,app->video_pkt_queue,pkt,1);
        if(ret < 0){
            break;
        }
        if(ret == 0){
            continue;
        }

        //把压缩的视频包喂给解码器
        ret = avcodec_send_packet(app->video_dec_ctx,pkt);
        av_packet_unref(pkt);
        if(ret < 0){
            print_ffmpeg_error("avcodec_send_packet failed",ret);
            goto cleanup;
        }

        while(1){
            double pts = 0.0;

            //从解码器取出解码后的原始帧
            ret = avcodec_receive_frame(app->video_dec_ctx,frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                break;
            }else if (ret < 0)
            {
                print_ffmpeg_error("avcodec_receive_frame",ret);
                goto cleanup;
            }

            // AV_NOPTS_VALUE = 0x8000000000000000 （一个特殊的无效标记值）
            if(frame->best_effort_timestamp != AV_NOPTS_VALUE){
                //FFmpeg 综合推算出的最佳时间戳,决定这一帧应该在第几秒显示
                pts = frame->best_effort_timestamp * av_q2d(app->video_stream->time_base);
            }

            ret = queue_decode_frame(app,frame,pts);
            if(ret == AVERROR_EXIT){
                break;
            }
            if(ret < 0){
                print_ffmpeg_error("queue_decode_frame failed",ret);
                goto cleanup;
            }
            
            av_frame_unref(frame);
        }
    }

cleanup:
    if(frame){
        av_frame_free(&frame);
    }
    if(pkt){
        av_packet_free(&pkt);
    }

    return ret;
}

int decoder_open_video(AppState *app)
{
    int ret;

    ret = av_find_best_stream(app->fmt_ctx,AVMEDIA_TYPE_VIDEO,-1,-1,&app->video_dec,0);
    if(ret < 0){
        print_ffmpeg_error("找不到视频流",ret);
        return ret;
    }

    app->video_stream_index = ret;
    app->video_stream = app->fmt_ctx->streams[app->video_stream_index];

    app->video_dec_ctx = avcodec_alloc_context3(app->video_dec);
    if(!app->video_dec_ctx){
        fprintf(stderr, "avcodec_alloc_context3 失败\n");
        return AVERROR(ENOMEM);
    }

    ret = avcodec_parameters_to_context(app->video_dec_ctx,app->video_stream->codecpar);
    if(ret < 0){
        print_ffmpeg_error("avcodec_parameters_to_context failed",ret);
        return ret;
    }

    ret = avcodec_open2(app->video_dec_ctx,app->video_dec,NULL);
    if(ret < 0){
        print_ffmpeg_error("avcodec_open2 failed",ret);
        return ret;
    }
    
    app->sws_ctx = sws_getContext(app->video_dec_ctx->width,
        app->video_dec_ctx->height,
        app->video_dec_ctx->pix_fmt,
        app->video_dec_ctx->width,
        app->video_dec_ctx->height,
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        NULL,NULL,NULL);
    if(!app->sws_ctx){
        fprintf(stderr, "sws_getContext failed:%s\n", SDL_GetError());
        return AVERROR(EINVAL);
    }

    return 0;
}

int decoder_start(AppState *app)
{
    app->decode_tid = SDL_CreateThread(decoder_thread,"decoder_thread",app);
    if(!app->decode_tid){
        fprintf(stderr, "SDL_CreateThread failed:%s\n", SDL_GetError());
        return -1;
    }
    
    return 0;
}