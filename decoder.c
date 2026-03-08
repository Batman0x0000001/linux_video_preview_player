#include <stdio.h>
#include "decoder.h"

static void print_ffmpeg_error(const char *msg,int errnum){
    char errbuf[AV_ERROR_MAX_STRING_SIZE]={0};
    av_strerror(errnum,errbuf,sizeof(errbuf));
    fprintf(stderr,"%s:%s\n",msg,errbuf);
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

    ret = avcodec_parameters_to_context(app->video_dec,app->video_stream->codecpar);
    if(ret < 0){
        print_ffmpeg_error("avcodec_parameters_to_context failed",ret);
        return ret;
    }

    ret = avcodec_open2(app->video_dec_ctx,app->video_dec,NULL);
    if(ret < 0){
        print_ffmpeg_error("avcodec_open2 failed",ret);
        return ret;
    }
    
    return 0;
}

int decoder_start(AppState *app)
{
    (void)app;
    return 0;
}