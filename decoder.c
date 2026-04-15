#include <stdio.h>
#include<string.h>
#include "decoder.h"
#include"frame_queue.h"
#include "audio_output.h"
#include "speed_filter.h"

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
    
    /*
     * pts 存入输出时域：pts_output = pts_orig / speed
     * 使得视频帧的显示节奏和音频时钟（atempo 输出 pts）均处于同一时域，
     * 现有同步逻辑无需修改。
     */
    vf->pts_sec = (app->speed > 0.0) ? (pts / app->speed) : pts;
    frame_queue_push(app->video_frm_queue);

    return 0;
}

/*
 * queue_decoded_audio — 将一帧 PCM（已经过滤镜 / 原始）转为 s16 并入队。
 * pts_sec: 已由调用方计算好的输出时域 pts（秒），直接存入 AudioBuffer。
 */
static int queue_decoded_audio(AppState *app, const AVFrame *frame, double pts_sec)
{
    uint8_t *out_buf = NULL;
    int out_linesize = 0;
    int out_nb_samples;
    int out_samples;
    int out_buf_size;
    int ret;

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

/*
 * push_audio_frame — 将一帧解码帧经过 atempo 滤镜（若已初始化）存入 PCM 队列。
 * raw_pts_sec: 敲解码帧在原始流时域中的 pts（秒）。
 * 滤镜输出帧的 pts 已由 atempo 缩放到输出时域，用 abuffersink 时域基转成秒。
 */
static int push_audio_frame(AppState *app, AVFrame *frame, AVFrame *filt_frame, double raw_pts_sec)
{
    int ret;
    AVRational sink_tb;
    double filt_pts_sec;

    if (!app->audio_fg) {
        /* 无滤镜图：不走滤镜直接算 pts_sec */
        if (app->audio_filter_pts < 0.0) {
             app->audio_filter_pts = (app->speed > 0.0) ? (raw_pts_sec / app->speed) : raw_pts_sec;
        }
        double pts_sec = app->audio_filter_pts;
        app->audio_filter_pts += (double)frame->nb_samples / app->audio_src.sample_rate;
        return queue_decoded_audio(app, frame, pts_sec);
    }

    /* 1. 将解码帧送入 abuffersrc */
    ret = speed_filter_send(app, frame);
    if (ret < 0) {
        return ret;
    }

    /* 2. 循环取出所有已拉伸帧 */
    sink_tb = av_buffersink_get_time_base(app->audio_fg_sink);

    while (1) {
        ret = speed_filter_receive(app, filt_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            return ret;
        }

        /*
         * 主动维护输出 PTS：
         * FFmpeg 滤镜图在重建后会重新以初始输入为基准计算 PTS，这在多次 Seek 或
         * 变速重建中会导致极为严重的全局时间跳跃不连续。
         * 解决方案：第一次从滤镜收到原始流 PTS 时，算出对应的输出域初始时间。
         * 此后每压出一帧音频缓冲，就严格按照其拉伸后的大小（即时长）持续累加 PTS，
         * 保证音画输出主频的绝对平滑和单调递增，彻底根除 Seek 时残留旧数据导致的时间倒流。
         */
        if (app->audio_filter_pts < 0.0) {
            app->audio_filter_pts = (app->speed > 0.0) ? (raw_pts_sec / app->speed) : raw_pts_sec;
        }
        filt_pts_sec = app->audio_filter_pts;
        
        /* 累加上这一个输出帧占用的系统播放时长 */
        app->audio_filter_pts += (double)filt_frame->nb_samples / app->audio_src.sample_rate;

        ret = queue_decoded_audio(app, filt_frame, filt_pts_sec);
        av_frame_unref(filt_frame);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static int decode_audio_packet(AppState *app, AVPacket *pkt, AVFrame *frame, AVFrame *filt_frame)
{
    int ret;

    ret = avcodec_send_packet(app->audio_dec_ctx, pkt);
    if (ret < 0) {
        print_ffmpeg_error("audio avcodec_send_packet failed", ret);
        return ret;
    }

    while (1) {
        double raw_pts;

        ret = avcodec_receive_frame(app->audio_dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }
        if (ret < 0) {
            print_ffmpeg_error("audio avcodec_receive_frame failed", ret);
            return ret;
        }

        raw_pts = frame_pts_to_seconds(frame, app->audio_stream->time_base);

        ret = push_audio_frame(app, frame, filt_frame, raw_pts);
        av_frame_unref(frame);
        if (ret < 0) {
            print_ffmpeg_error("push_audio_frame failed", ret);
            return ret;
        }
    }
}

static int drain_audio_decoder(AppState *app, AVFrame *frame, AVFrame *filt_frame)
{
    int ret;

    ret = avcodec_send_packet(app->audio_dec_ctx, NULL);
    if (ret < 0 && ret != AVERROR_EOF) {
        print_ffmpeg_error("audio drain send_packet(NULL) failed", ret);
        return ret;
    }

    while (1) {
        double raw_pts;

        ret = avcodec_receive_frame(app->audio_dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            print_ffmpeg_error("audio drain receive_frame failed", ret);
            return ret;
        }

        raw_pts = frame_pts_to_seconds(frame, app->audio_stream->time_base);

        ret = push_audio_frame(app, frame, filt_frame, raw_pts);
        av_frame_unref(frame);
        if (ret < 0) {
            print_ffmpeg_error("push_audio_frame (drain) failed", ret);
            return ret;
        }
    }

    /* 将滤镜图中残留的 PCM 一并排尽 */
    if (app->audio_fg) {
        double filt_pts_sec;

        speed_filter_send(app, NULL); //发送 EOF 信号
        while (1) {
            ret = speed_filter_receive(app, filt_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                break;
            }

            if (app->audio_filter_pts < 0.0) {
                 app->audio_filter_pts = 0.0; 
            }
            filt_pts_sec = app->audio_filter_pts;
            app->audio_filter_pts += (double)filt_frame->nb_samples / app->audio_src.sample_rate;

            queue_decoded_audio(app, filt_frame, filt_pts_sec);
            av_frame_unref(filt_frame);
        }
    }

    return 0;
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
        if (packet_queue_is_flush_pkt(pkt)) {
            /*
             * seek 哨兵到达：
             * 1. 刷新解码器内部的 B/P 帧缓存
             * 2. 清空帧队列里残留的旧帧
             * 3. 重置 drained，下一个 EOF 才再次执行 drain
             */
            avcodec_flush_buffers(app->video_dec_ctx);
            frame_queue_flush(app->video_frm_queue);
            drained = 0;
            pkt->data = NULL; /* 清除哨兵标记，防止 cleanup av_packet_free 误 free */
            pkt->size = 0;
            continue;
        }

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
    AppState *app  = (AppState *)arg;
    AVPacket *pkt  = NULL;
    AVFrame  *frame      = NULL; //解码原始帧
    AVFrame  *filt_frame = NULL; //滤镜输出帧
    int ret    = 0;
    int drained = 0;

    pkt        = av_packet_alloc();
    frame      = av_frame_alloc();
    filt_frame = av_frame_alloc();
    if (!pkt || !frame || !filt_frame) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    while (!app->quit) {
        /* ---- 速度变更检测：重建 atempo 滤镜图 ---- */
        if (app->speed_change_req) {
            if (speed_filter_rebuild(app) < 0) {
                fprintf(stderr, "speed_filter_rebuild 失败，继续使用旧善镜图\n");
            }
            app->speed_change_req = 0;
        }

        if (app->audio_buf_queue &&
            audio_buffer_queue_size(app->audio_buf_queue) > audio_buf_queue_limit_bytes(app)) {
            SDL_Delay(5);
            continue;
        }

        ret = packet_queue_get(app, app->audio_pkt_queue, pkt, 0);
        if (ret < 0) {
            break;
        }
        if (ret == 0) {
             if (app->demux_finished && packet_queue_size(app->audio_pkt_queue) == 0) {
                if (!drained) {
                    ret = drain_audio_decoder(app, frame, filt_frame);
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

        if (packet_queue_is_flush_pkt(pkt)) {
            /*
             * seek 哨兵到达：
             * 1. 刷新音频解码器内部缓存
             * 2. 清空 PCM 缓冲队列里的旧数据
             * 3. 在 SDL 音频锁保护下清空正在播放的当前缓冲块
             * 4. 重建滤镜图，丢弃内部残留缓冲
             * 5. 重置滤镜 PTS 状态，下个包来时重新锚定新的时间起点
             */
            avcodec_flush_buffers(app->audio_dec_ctx);
            if (app->audio_buf_queue) {
                audio_buffer_queue_flush(app->audio_buf_queue);
            }
            if (app->audio_dev) {
                SDL_LockAudioDevice(app->audio_dev);
                audio_buffer_unref(&app->audio_buf_cur);
                SDL_UnlockAudioDevice(app->audio_dev);
            } else {
                audio_buffer_unref(&app->audio_buf_cur);
            }
            speed_filter_rebuild(app); //seek 后滤镜内部缓冲已无效，重建
            app->audio_filter_pts = -1.0; 
            drained = 0;
            pkt->data = NULL;
            pkt->size = 0;
            continue;
        }

        ret = decode_audio_packet(app, pkt, frame, filt_frame);
        av_packet_unref(pkt);
        if (ret == AVERROR_EXIT) {
            break;
        }
        if (ret < 0) {
            goto cleanup;
        }
    }

cleanup:
    if (filt_frame) {
        av_frame_free(&filt_frame);
    }
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

int decoder_audio_filter_init(AppState *app)
{
    return speed_filter_init(app);
}