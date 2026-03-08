#include<stdio.h>
#include"demux.h"

static void dump_input_info(AppState *app){
    av_dump_format(app->fmt_ctx,0,app->input_filename,0);
}

int demux_open_input(AppState *app)
{
    int ret;

    ret = avformat_open_input(&app->fmt_ctx,app->input_filename,NULL,NULL);
    if(ret < 0){
        return ret;
    }

    ret = avformat_find_stream_info(&app->fmt_ctx,NULL);
    if(ret < 0){
        return ret;
    }

    dump_input_info(&app);
    return 0;
}

int demux_start(AppState *app)
{
    (void)app;
    return 0;
}