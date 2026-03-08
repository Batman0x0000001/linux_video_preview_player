#ifndef APP_H
#define APP_H

#include<stdint.h>
#include<SDL2/SDL.h>

#include<libavformat/avformat.h>
#include<libavcodec/avcodec.h>
#include<libswscale/swscale.h>

typedef struct PacketQueue PacketQueue;
typedef struct FrameQueue FrameQueue;

//显示帧：统一定义为YUV420P
typedef struct VideoFrame
{
    AVFrame *frame;
    double pts_sec;
}VideoFrame;

//项目总状态
typedef struct AppState
{
    char input_filename[1024];

    int quit;
    int paused;

    //FFmpeg
    AVFormatContext *fmt_ctx;
    int video_stream_index;
    AVStream *video_stream;

    const AVCodec *video_dec;
    AVCodecContext *video_dec_ctx;

    struct SwsContext *sws_ctx;

    //SDL2
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;

    int window_width;
    int window_height;

    //Thread
    SDL_Thread *demux_tid;
    SDL_Thread *decode_tid;

    //Queue
    PacketQueue *video_pkt_queue;
    FrameQueue *video_frm_queue;
    
    //clock
    double frame_timer;
    double frame_last_pts;
    double frame_last_delay;
    double video_clock;

    double video_current_pts;
    int64_t video_current_pts_time;
}AppState;

#endif