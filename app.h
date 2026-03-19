#ifndef APP_H
#define APP_H

#include<stdint.h>
#include<SDL2/SDL.h>

#include<libavformat/avformat.h>
#include<libavcodec/avcodec.h>
#include<libswscale/swscale.h>
#include<libswresample/swresample.h>
#include<libavutil/channel_layout.h>

typedef struct PacketQueue PacketQueue;
typedef struct FrameQueue FrameQueue;
typedef struct AudioBufferQueue AudioBufferQueue;


//显示帧：统一定义为YUV420P
typedef struct VideoFrame
{
    AVFrame *frame;
    double pts_sec;
}VideoFrame;

typedef struct AudioBuffer
{
    uint8_t *data;
    int size;
    int pos;
    double pts_sec;
} AudioBuffer;

typedef struct AudioParams
{
    AVChannelLayout ch_layout;          // 现代 FFmpeg 6.x 的声道布局表示
    enum AVSampleFormat fmt;            // 采样格式，例如 s16 / fltp
    int sample_rate;                    // 采样率
    int channels;                       // 声道数（便于直接使用）
    int bytes_per_sec;                  // 每秒 PCM 字节数，后面算音频时钟会用到
} AudioParams;

//项目总状态
typedef struct AppState
{
    char input_filename[1024];

    int quit;
    int paused;
    int demux_finished;
    int video_decode_finished;
    int audio_decode_finished;
    int audio_output_idle;
    
    //FFmpeg
    AVFormatContext *fmt_ctx;

    int video_stream_index;
    AVStream *video_stream;
    const AVCodec *video_dec;
    AVCodecContext *video_dec_ctx;
    struct SwsContext *sws_ctx;

    int audio_stream_index;
    AVStream *audio_stream;
    const AVCodec *audio_dec;
    AVCodecContext *audio_dec_ctx;
    struct SwrContext *swr_ctx;
    AudioParams audio_src;
    AudioParams audio_tgt;

    //SDL2
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_AudioDeviceID audio_dev;
    SDL_AudioSpec audio_spec;

    int window_width;
    int window_height;

    //Thread
    SDL_Thread *demux_tid;
    SDL_Thread *decode_tid;
    SDL_Thread *audio_decode_tid;

    //Queue
    PacketQueue *video_pkt_queue;
    FrameQueue *video_frm_queue;

    PacketQueue *audio_pkt_queue;
    AudioBufferQueue *audio_buf_queue;
    AudioBuffer audio_buf_cur;
    

    /*
    clock:
        第一层：媒体时间层
        描述“视频本来应该怎么走”：
            pts_sec
            frame_last_pts
            frame_last_delay

        第二层：现实世界时钟层
        描述“电脑此刻真的走到哪里了”：
            frame_timer
            video_current_pts_time
    */
    double frame_timer;//记录上一帧实际显示的时刻（锚点）
    double frame_last_pts;//记录上一帧的pts，用于计算delay
    double frame_last_delay;//记录上一次正常的delay，用于异常兜底
    double video_clock;

    double video_current_pts;//当前时间基准
    int64_t video_current_pts_time;

    double audio_clock;//最近一次回调里，已经交给 SDL 的音频进度（以秒为单位）
    int audio_hw_buf_size;//音频设备一次硬件缓冲的大致字节数
    int64_t audio_callback_time;//上一次更新 audio_clock 的现实时间戳（微秒）
}AppState;

#endif