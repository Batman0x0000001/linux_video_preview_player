#include <stdio.h>
#include "display.h"

int display_init(AppState *app)
{
    app->window = SDL_CreateWindow(
        "linux_video_preview_player",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        app->window_width,
        app->window_height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!app->window) {
        return -1;
    }

    app->renderer = SDL_CreateRenderer(
        app->window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!app->renderer) {
        app->renderer = SDL_CreateRenderer(app->window, -1, SDL_RENDERER_SOFTWARE);
        if (!app->renderer) {
            return -1;
        }
    }

    app->texture = SDL_CreateTexture(app->renderer,SDL_PIXELFORMAT_IYUV,SDL_TEXTUREACCESS_STREAMING,app->video_dec_ctx->width,app->video_dec_ctx->height);
    if(!app->texture){
        return -1;
    }
    return 0;
}

void display_destroy(AppState *app)
{
    if (app->texture) {
        SDL_DestroyTexture(app->texture);
        app->texture = NULL;
    }

    if (app->renderer) {
        SDL_DestroyRenderer(app->renderer);
        app->renderer = NULL;
    }

    if (app->window) {
        SDL_DestroyWindow(app->window);
        app->window = NULL;
    }
}

static void calculate_display_rect(AppState *app,SDL_Rect *rect){
    int pic_w = app->video_dec_ctx->width;
    int pic_h = app->video_dec_ctx->height;
    double aspect_ratio;
    int w,h,x,y;

    
    /*
    SAR（Sample Aspect Ratio）是单个像素的宽高比
        int num;   numerator   = 分子
        int den;   denominator = 分母
    */
    if(app->video_stream->sample_aspect_ratio.num != 0 &&
         app->video_stream->sample_aspect_ratio.den != 0){
        aspect_ratio = av_q2d(app->video_stream->sample_aspect_ratio);
    }else{
        aspect_ratio = 1.0;
    }

    aspect_ratio *= (double)pic_w / (double)pic_h;

    //& ~3 是对齐到4的倍数（YUV420P格式要求宽高是偶数，4对齐更安全）
    /*
        逻辑是尽量放大，但不能超出窗口：
        优先方案：高度 = 窗口高，宽度按比例算
        后备方案：宽度 = 窗口宽，高度按比例算
    */
    h = app->window_height;
    w = (int)(h * aspect_ratio)& ~3;

    if(w>app->window_width){
        w = app->window_width;
        h = (int)(w * aspect_ratio)& ~3;
    }

    x = (app->window_width - w)/2;
    y = (app->window_height - h)/2;

    rect->x = x;// 视频区域左上角距窗口左边的距离
    rect->y = y;// 视频区域左上角距窗口顶部的距离
    rect->w = w;// 视频区域的宽度
    rect->h = h;// 视频区域的高度
}

int display_present_frame(AppState *app,const VideoFrame *vf){
    SDL_Rect rect;

    if(!app || !vf || !vf->frame || !app->texture){
        return -1;
    }

    if(SDL_UpdateYUVTexture(app->texture,NULL,
        vf->frame->data[0],vf->frame->linesize[0],
        vf->frame->data[1],vf->frame->linesize[1],
        vf->frame->data[2],vf->frame->linesize[2])!=0){
        fprintf(stderr, "SDL_UpdateYUVTexture failed:%s\n", SDL_GetError());
        return -1;
    }

    calculate_display_rect(app,&rect);

    SDL_RenderClear(app->renderer);
    SDL_RenderCopy(app->renderer,app->texture,NULL,&rect);
    SDL_RenderPresent(app->renderer);

    return 0;
}