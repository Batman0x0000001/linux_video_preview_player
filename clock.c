#include <libavutil/time.h>
#include "clock.h"

double clock_get_video(AppState *app)
{
    double delta;

    delta = (av_gettime_relative() - app->video_current_pts_time) / 1000000.0;
    return app->video_current_pts + delta;
}

void clock_update_video(AppState *app,double pts_sec){
    app->video_current_pts = pts_sec;
    app->video_current_pts_time = av_gettime_relative();
}

double clock_get_audio(AppState *app)
{
    double pts;
    double hw_delay;
    double elapsed;

    if (!app) {
        return 0.0;
    }

    pts = app->audio_clock;

    if (app->audio_callback_time <= 0 ||
        app->audio_tgt.bytes_per_sec <= 0 ||
        app->audio_hw_buf_size <= 0) {
        return pts;
    }

    /*
     * audio_clock 记录的是“最近一次回调里，已经交给 SDL 的音频进度”。
     * 但这些数据还会在硬件缓冲里滞留一小段时间，所以这里减去
     * 一个近似的硬件缓冲延迟，再加上从那次回调到现在已经过去的时间，
     * 得到一个更接近“此刻真正听到哪里”的估计值。
     */
    hw_delay = (double)app->audio_hw_buf_size / (double)app->audio_tgt.bytes_per_sec;
    elapsed = (av_gettime_relative() - app->audio_callback_time) / 1000000.0;

    if (elapsed < 0.0) {
        elapsed = 0.0;
    }
    if (elapsed > hw_delay) {
        elapsed = hw_delay;
    }

    return pts - hw_delay + elapsed;
}

void clock_update_audio(AppState *app, double pts_sec)
{
    if (!app) {
        return;
    }

    app->audio_clock = pts_sec;
    app->audio_callback_time = av_gettime_relative();
}

double clock_get_master(AppState *app)
{
    if (!app) {
        return 0.0;
    }

    if (app->audio_stream_index >= 0 && app->audio_dev) {
        return clock_get_audio(app);
    }

    return clock_get_video(app);
}