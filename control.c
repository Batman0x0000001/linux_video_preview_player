#include "control.h"
#include"frame_queue.h"
#include"display.h"
#include"clock.h"
#include <libavutil/time.h>
#include"audio_output.h"

static double calc_frame_delay(AppState *app, const VideoFrame *vf)
{
    double delay = vf->pts_sec - app->frame_last_pts;

    if (delay <= 0.0 || delay >= 1.0) {
        delay = app->frame_last_delay;
    }
    if (delay <= 0.0) {
        delay = 0.04;
    }

    return delay;
}

static int frame_should_drop(AppState *app, const VideoFrame *vf)
{
    double audio_clock;
    double diff;

    if (!app || !vf) {
        return 0;
    }

    if (!(app->audio_stream_index >= 0 && app->audio_dev)) {
        return 0;
    }

    if (frame_queue_size(app->video_frm_queue) <= 1) {
        return 0;
    }

    audio_clock = clock_get_audio(app);
    diff = vf->pts_sec - audio_clock;

    /*
     * 如果当前视频帧已经比音频落后超过 100ms，
     * 且后面还有帧可追，就直接丢掉这一帧。
     */
    if (diff < -0.10) {
        return 1;
    }

    return 0;
}

static double get_sync_threshold(double delay)
{
    if (delay < 0.01) {
        return 0.01;
    }
    if (delay > 0.10) {
        return 0.10;
    }
    return delay;
}

static int frame_should_present(AppState *app,const VideoFrame *vf){
    double delay;
    double target_time;
    double now;
    double actual_delay;

    delay = calc_frame_delay(app,vf);

    actual_delay = delay;

    /*
     * 有音频时：音频做主时钟，视频根据与音频的差值做校正
     * 没音频时：退回原来的纯视频节奏
     */
    if (app->audio_stream_index >= 0 && app->audio_dev) {
        double audio_clock = clock_get_audio(app);
        double diff = vf->pts_sec - audio_clock;
        double sync_threshold = get_sync_threshold(delay);

        /*
         * diff > 0: 视频领先音频，应该适当多等一会儿
         * diff < 0: 视频落后音频，应该尽快显示
         */
        if (diff <= -sync_threshold) {
            actual_delay = 0.0;
        } else if (diff >= sync_threshold) {
            actual_delay = delay + diff;
            if (actual_delay > 0.5) {
                actual_delay = 0.5;
            }
        }
    }

    target_time = app->frame_timer + actual_delay;
    now = av_gettime_relative() / 1000000.0;//当前时间（微秒转换为秒）

    //当前时间已经到达目标时间:加了 1ms 的容忍误差，避免因系统调度延迟导致帧永远显示不出来
    if(now + 0.001 >=target_time){
        /*
         * frame_last_delay 记录的是“媒体本来的帧间隔”
         * 而不是“为了同步临时修正后的等待值”。
         * 这样下一帧的节奏基线才不会被同步校正污染。
         */
        app->frame_last_delay = delay;
        app->frame_last_pts = vf->pts_sec;

        if (actual_delay <= 0.0) {
            app->frame_timer = now;
        } else {
            app->frame_timer = target_time;
        }

        return 1;
    }

    return 0;
}

static void reset_playback_timing_after_resume(AppState *app){
    double now = av_gettime_relative() / 1000000.0;
    double master_pts = clock_get_master(app);

    app->frame_timer = now;
    app->frame_last_pts = master_pts;
    app->frame_last_delay = 0.04;
     /*
     * video_current_pts 仍然保留更新，方便纯视频路径和调试使用。
     * 下一次真正显示视频帧后，视频时钟会再次被校正到该帧 pts。
     */
    app->video_current_pts = master_pts;
    app->video_current_pts_time = av_gettime_relative();
}

static int playback_has_finished(AppState *app)
{
    if (!app || !app->demux_finished) {
        return 0;
    }

    if (!app->video_decode_finished) {
        return 0;
    }

    if (frame_queue_size(app->video_frm_queue) > 0) {
        return 0;
    }

    if (app->audio_stream_index >= 0) {
        if (!app->audio_decode_finished) {
            return 0;
        }

        if (!app->audio_output_idle) {
            return 0;
        }

        if (app->audio_buf_queue &&
            audio_buffer_queue_size(app->audio_buf_queue) > 0) {
            return 0;
        }

        if (app->audio_buf_cur.pos < app->audio_buf_cur.size) {
            return 0;
        }
    }

    return 1;
}

int control_event_loop(AppState *app)
{
    SDL_Event event;

    while (!app->quit) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                app->quit = 1;
            } else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                    app->quit = 1;
                    packet_queue_abort(app->video_pkt_queue);
                    frame_queue_abort(app->video_frm_queue);
                    break;
                case SDLK_SPACE:
                {
                    int was_paused = app->paused;
                    app->paused = !app->paused;
                    if (app->audio_dev) {
                        SDL_PauseAudioDevice(app->audio_dev, app->paused ? 1 : 0);
                    }
                    if(was_paused && !app->paused){
                        reset_playback_timing_after_resume(app);
                    }
                    break;
                } 
                default:
                    break;
                }
            }else if (event.type == SDL_WINDOWEVENT)
            {
                if(event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED){
                    app->window_width = event.window.data1;
                    app->window_height = event.window.data2;
                }
            }  
        }

        if(!app->paused){
            VideoFrame *vf = NULL;
            // int ret = frame_queue_peek_readable(app,app->video_frm_queue,&vf);
            // if(ret < 0){
            //     break;
            // }
            // if(ret >0 && vf){
            //     display_present_frame(app,vf);
            //     frame_queue_next(app->video_frm_queue);
            // }
            if (frame_queue_try_peek_readable(app->video_frm_queue, &vf) > 0 && vf) {
                if (frame_should_drop(app, vf)) {
                    double now = av_gettime_relative() / 1000000.0;
                    double delay = calc_frame_delay(app, vf);

                    app->frame_last_delay = delay;
                    app->frame_last_pts = vf->pts_sec;
                    app->frame_timer = now;
                    clock_update_video(app, vf->pts_sec);
                    frame_queue_next(app->video_frm_queue);
                } else if (frame_should_present(app, vf)) {
                    display_present_frame(app, vf);
                    clock_update_video(app, vf->pts_sec);
                    frame_queue_next(app->video_frm_queue);
                }
            }
            if (playback_has_finished(app)) {
                app->quit = 1;
                break;
            }
        }

        SDL_Delay(1);
    }

    return 0;
}