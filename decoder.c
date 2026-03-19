#include <stdio.h>
#include<string.h>
#include "decoder.h"
#include"frame_queue.h"
#include "audio_output.h"

static void print_ffmpeg_error(const char *msg,int errnum){
    char errbuf[AV_ERROR_MAX_STRING_SIZE]={0};
    av_strerror(errnum,errbuf,sizeof(errbuf));
    fprintf(stderr,"%s:%s\n",msg,errbuf);
}

static double frame_pts_to_seconds(const AVFrame *frame, AVRational time_base)
{
    if (!frame) {
        return 0.0;
    }

    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
        return frame->best_effort_timestamp * av_q2d(time_base);
    }

    if (frame->pts != AV_NOPTS_VALUE) {
        return frame->pts * av_q2d(time_base);
    }

    return 0.0;
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

static int queue_decoded_audio(AppState *app, const AVFrame *frame)
{
    uint8_t *out_buf = NULL;
    int out_linesize = 0;
    int out_nb_samples;
    int out_samples;
    int out_buf_size;
    int ret;
    double pts_sec;

    if (!app || !frame || !app->swr_ctx || !app->audio_buf_queue) {
        return AVERROR(EINVAL);
    }

    out_nb_samples = av_rescale_rnd(
        swr_get_delay(app->swr_ctx, app->audio_src.sample_rate) + frame->nb_samples,
        app->audio_tgt.sample_rate,
        app->audio_src.sample_rate,
        AV_ROUND_UP
    );
    if (out_nb_samples <= 0) {
        return AVERROR(EINVAL);
    }

    ret = av_samples_alloc(&out_buf,
                           &out_linesize,
                           app->audio_tgt.channels,
                           out_nb_samples,
                           app->audio_tgt.fmt,
                           0);
    if (ret < 0) {
        return ret;
    }

    out_samples = swr_convert(app->swr_ctx,
                              &out_buf,
                              out_nb_samples,
                              (const uint8_t **)frame->extended_data,
                              frame->nb_samples);
    if (out_samples < 0) {
        ret = out_samples;
        goto cleanup;
    }

    out_buf_size = av_samples_get_buffer_size(NULL,
                                              app->audio_tgt.channels,
                                              out_samples,
                                              app->audio_tgt.fmt,
                                              1);
    if (out_buf_size < 0) {
        ret = out_buf_size;
        goto cleanup;
    }

    pts_sec = frame_pts_to_seconds(frame, app->audio_stream->time_base);

    ret = audio_buffer_queue_put(app->audio_buf_queue, out_buf, out_buf_size, pts_sec);

cleanup:
    if (out_buf) {
        av_freep(&out_buf);
    }

    return ret;
}

static int decode_video_packet(AppState *app, AVPacket *pkt, AVFrame *frame)
{
    int ret;

    ret = avcodec_send_packet(app->video_dec_ctx, pkt);
    if (ret < 0) {
        print_ffmpeg_error("video avcodec_send_packet failed", ret);
        return ret;
    }

    while (1) {
        double pts = 0.0;

        ret = avcodec_receive_frame(app->video_dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }
        if (ret < 0) {
            print_ffmpeg_error("video avcodec_receive_frame failed", ret);
            return ret;
        }

        pts = frame_pts_to_seconds(frame, app->video_stream->time_base);

        ret = queue_decode_frame(app, frame, pts);
        av_frame_unref(frame);
        if (ret == AVERROR_EXIT) {
            return ret;
        }
        if (ret < 0) {
            print_ffmpeg_error("queue_decode_frame failed", ret);
            return ret;
        }
    }
}

static int drain_video_decoder(AppState *app, AVFrame *frame)
{
    int ret;

    ret = avcodec_send_packet(app->video_dec_ctx, NULL);
    if (ret < 0 && ret != AVERROR_EOF) {
        print_ffmpeg_error("video drain send_packet(NULL) failed", ret);
        return ret;
    }

    while (1) {
        double pts = 0.0;

        ret = avcodec_receive_frame(app->video_dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }
        if (ret < 0) {
            print_ffmpeg_error("video drain receive_frame failed", ret);
            return ret;
        }

        pts = frame_pts_to_seconds(frame, app->video_stream->time_base);

        ret = queue_decode_frame(app, frame, pts);
        av_frame_unref(frame);
        if (ret == AVERROR_EXIT) {
            return ret;
        }
        if (ret < 0) {
            print_ffmpeg_error("queue_decode_frame failed", ret);
            return ret;
        }
    }
}

static int decode_audio_packet(AppState *app, AVPacket *pkt, AVFrame *frame)
{
    int ret;

    ret = avcodec_send_packet(app->audio_dec_ctx, pkt);
    if (ret < 0) {
        print_ffmpeg_error("audio avcodec_send_packet failed", ret);
        return ret;
    }

    while (1) {
        ret = avcodec_receive_frame(app->audio_dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }
        if (ret < 0) {
            print_ffmpeg_error("audio avcodec_receive_frame failed", ret);
            return ret;
        }

        ret = queue_decoded_audio(app, frame);
        av_frame_unref(frame);
        if (ret < 0) {
            print_ffmpeg_error("queue_decoded_audio failed", ret);
            return ret;
        }
    }
}

static int drain_audio_decoder(AppState *app, AVFrame *frame)
{
    int ret;

    ret = avcodec_send_packet(app->audio_dec_ctx, NULL);
    if (ret < 0 && ret != AVERROR_EOF) {
        print_ffmpeg_error("audio drain send_packet(NULL) failed", ret);
        return ret;
    }

    while (1) {
        ret = avcodec_receive_frame(app->audio_dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }
        if (ret < 0) {
            print_ffmpeg_error("audio drain receive_frame failed", ret);
            return ret;
        }

        ret = queue_decoded_audio(app, frame);
        av_frame_unref(frame);
        if (ret < 0) {
            print_ffmpeg_error("queue_decoded_audio failed", ret);
            return ret;
        }
    }
}

static int open_codec_context_from_stream(AVCodecContext **dec_ctx,const AVCodec *dec,AVStream *stream){
    int ret;
    AVCodecContext *ctx = NULL;

    if (!dec_ctx || !dec || !stream) {
        return AVERROR(EINVAL);
    }

    ctx = avcodec_alloc_context3(dec);
    if (!ctx) {
        return AVERROR(ENOMEM);
    }

    ret = avcodec_parameters_to_context(ctx, stream->codecpar);
    if (ret < 0) {
        avcodec_free_context(&ctx);
        return ret;
    }

    ret = avcodec_open2(ctx, dec, NULL);
    if (ret < 0) {
        avcodec_free_context(&ctx);
        return ret;
    }

    //局部变量 ctx 充当事务缓冲区，保证"要么完整成功并写出，要么完全不修改调用者状态"的原子性语义
    *dec_ctx = ctx;
    return 0;
}

static int audio_params_init(AudioParams *ap,const AVChannelLayout *ch_layout,enum AVSampleFormat fmt,int sample_rate){
    int ret;

    if (!ap || !ch_layout || ch_layout->nb_channels <= 0 || sample_rate <= 0) {
        return AVERROR(EINVAL);
    }

    av_channel_layout_uninit(&ap->ch_layout);

    ret = av_channel_layout_copy(&ap->ch_layout, ch_layout);
    if (ret < 0) {
        return ret;
    }

    ap->fmt = fmt;
    ap->sample_rate = sample_rate;
    ap->channels = ap->ch_layout.nb_channels;

    ret = av_samples_get_buffer_size(NULL,
                                     ap->channels,
                                     ap->sample_rate,
                                     ap->fmt,
                                     1);
    if (ret < 0) {
        av_channel_layout_uninit(&ap->ch_layout);
        memset(ap, 0, sizeof(*ap));
        return ret;
    }

    ap->bytes_per_sec = ret;
    return 0;
}

static int audio_resampler_init(AppState *app){
    int ret;

    if (!app) {
        return AVERROR(EINVAL);
    }

    swr_free(&app->swr_ctx);

    ret = swr_alloc_set_opts2(&app->swr_ctx,
                              &app->audio_tgt.ch_layout,
                              app->audio_tgt.fmt,
                              app->audio_tgt.sample_rate,
                              &app->audio_src.ch_layout,
                              app->audio_src.fmt,
                              app->audio_src.sample_rate,
                              0,
                              NULL);
    if (ret < 0) {
        return ret;
    }

    ret = swr_init(app->swr_ctx);
    if (ret < 0) {
        swr_free(&app->swr_ctx);
        return ret;
    }

    return 0;
}

int decoder_configure_audio_target(AppState *app, int sample_rate, int channels)
{
    AVChannelLayout target_layout = {0};
    int ret;

    if (!app || sample_rate <= 0 || (channels != 1 && channels != 2)) {
        return AVERROR(EINVAL);
    }

    av_channel_layout_uninit(&app->audio_tgt.ch_layout);
    memset(&app->audio_tgt, 0, sizeof(app->audio_tgt));

    av_channel_layout_default(&target_layout, channels);

    ret = audio_params_init(&app->audio_tgt,
                            &target_layout,
                            AV_SAMPLE_FMT_S16,
                            sample_rate);

    av_channel_layout_uninit(&target_layout);
    if (ret < 0) {
        return ret;
    }

    ret = audio_resampler_init(app);
    if (ret < 0) {
        av_channel_layout_uninit(&app->audio_tgt.ch_layout);
        memset(&app->audio_tgt, 0, sizeof(app->audio_tgt));
        return ret;
    }

    return 0;
}



static int audio_buf_queue_limit_bytes(const AppState *app)
{
    int limit;

    if (!app) {
        return 0;
    }

    limit = app->audio_tgt.bytes_per_sec * 2;
    if (limit < app->audio_hw_buf_size * 4) {
        limit = app->audio_hw_buf_size * 4;
    }

    return limit;
}

static int video_decoder_thread(void *arg){
    AppState *app = (AppState *)arg;
    AVPacket *pkt = NULL;
    AVFrame *frame =NULL;
    int ret = 0;
    int drained = 0;

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

        ret = packet_queue_get(app,app->video_pkt_queue,pkt,0);
        // printf("video ret:%d\n",ret);
        if(ret < 0){
            break;
        }
        if(ret == 0){
            if (app->demux_finished && packet_queue_size(app->video_pkt_queue) == 0) {
                if (!drained) {
                    ret = drain_video_decoder(app, frame);
                    if (ret < 0) {
                        printf("drain_video_decoder(video) failed\n");
                        goto cleanup;
                    }
                    drained = 1;
                }
                app->video_decode_finished = 1;
                break;
            }

            SDL_Delay(5);
            continue;
        }

        //把压缩的视频包喂给解码器
        ret = decode_video_packet(app, pkt, frame);
        av_packet_unref(pkt);
        if (ret == AVERROR_EXIT) {
            break;
        }
        if (ret < 0) {
            goto cleanup;
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

static int audio_decoder_thread(void *arg)
{
    AppState *app = (AppState *)arg;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    int ret = 0;
    int drained = 0;

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    while (!app->quit) {
        if (app->audio_buf_queue &&
            audio_buffer_queue_size(app->audio_buf_queue) > audio_buf_queue_limit_bytes(app)) {
            SDL_Delay(5);
            continue;
        }

        ret = packet_queue_get(app, app->audio_pkt_queue, pkt, 0);
        // printf("audio ret:%d\n",ret);
        if (ret < 0) {
            break;
        }
        if (ret == 0) {
             if (app->demux_finished && packet_queue_size(app->audio_pkt_queue) == 0) {
                if (!drained) {
                    ret = drain_audio_decoder(app, frame);
                    if (ret < 0) {
                        goto cleanup;
                    }
                    drained = 1;
                }
                app->audio_decode_finished = 1;
                break;
            }

            SDL_Delay(5);
            continue;
        }

        ret = decode_audio_packet(app, pkt, frame);
        av_packet_unref(pkt);
        if (ret == AVERROR_EXIT) {
            break;
        }
        if (ret < 0) {
            goto cleanup;
        }
    }

cleanup:
    if (frame) {
        av_frame_free(&frame);
    }
    if (pkt) {
        av_packet_free(&pkt);
    }

    return ret;
}

int decoder_open_video(AppState *app)
{
    int ret;

    if (!app || app->video_stream_index < 0 || !app->video_stream || !app->video_dec) {
        return AVERROR_STREAM_NOT_FOUND;
    }

    ret = open_codec_context_from_stream(&app->video_dec_ctx,
                                         app->video_dec,
                                         app->video_stream);
    if (ret < 0) {
        print_ffmpeg_error("打开视频解码器失败", ret);
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

int decoder_open_audio(AppState *app){
    int ret;
    int target_channels = 0;

    if (!app) {
        return AVERROR(EINVAL);
    }

    if (app->audio_stream_index < 0 || !app->audio_stream || !app->audio_dec) {
        return 0;
    }

    ret = open_codec_context_from_stream(&app->audio_dec_ctx,
                                         app->audio_dec,
                                         app->audio_stream);
    if (ret < 0) {
        print_ffmpeg_error("打开音频解码器失败", ret);
        goto fail;
    }

    if (app->audio_dec_ctx->sample_rate <= 0 ||
        app->audio_dec_ctx->ch_layout.nb_channels <= 0) {
        fprintf(stderr, "音频流参数无效：sample_rate=%d channels=%d\n",
                app->audio_dec_ctx->sample_rate,
                app->audio_dec_ctx->ch_layout.nb_channels);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    ret = audio_params_init(&app->audio_src,
                            &app->audio_dec_ctx->ch_layout,
                            app->audio_dec_ctx->sample_fmt,
                            app->audio_dec_ctx->sample_rate);
    if (ret < 0) {
        print_ffmpeg_error("初始化音频源参数失败", ret);
        goto fail;
    }

    /*
     * 本教学阶段先做一个稳定、简单的目标格式：
     * - 单声道源 -> 单声道输出
     * - 多声道源 -> 统一先压到双声道
     * - 采样率先沿用源采样率
     * - 采样格式统一为 S16
     */
    target_channels = (app->audio_src.channels >= 2) ? 2 : 1;

    ret = decoder_configure_audio_target(app,
                                        app->audio_src.sample_rate,
                                        target_channels);
    if (ret < 0) {
        print_ffmpeg_error("初始化音频目标参数失败", ret);
        goto fail;
    }
    
    return 0;

fail:

    swr_free(&app->swr_ctx);

    av_channel_layout_uninit(&app->audio_src.ch_layout);
    memset(&app->audio_src, 0, sizeof(app->audio_src));

    av_channel_layout_uninit(&app->audio_tgt.ch_layout);
    memset(&app->audio_tgt, 0, sizeof(app->audio_tgt));

    if (app->audio_dec_ctx) {
        avcodec_free_context(&app->audio_dec_ctx);
    }

    return ret;
}

int decoder_start(AppState *app)
{
    app->decode_tid = SDL_CreateThread(video_decoder_thread,"video_decoder_thread",app);
    if(!app->decode_tid){
        fprintf(stderr, "SDL_CreateThread failed:%s\n", SDL_GetError());
        return -1;
    }
    
    if (app->audio_stream_index >= 0) {
        app->audio_decode_tid = SDL_CreateThread(audio_decoder_thread, "audio_decoder_thread", app);
        if (!app->audio_decode_tid) {
            fprintf(stderr, "创建音频解码线程失败: %s\n", SDL_GetError());
            return -1;
        }
    }

    return 0;
}