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
                case SDLK_LEFT:
                case SDLK_RIGHT:
                {
                    if (app->is_rtsp) {
                        fprintf(stderr, "RTSP流不支持seek功能\n");
                        break;
                    }
                    /*
                     * 1. clock_get_master(app) 返回的是“输出时域”（用户感受到的相对时间）。
                     * 2. FFmpeg av_seek_frame 需要的是“原始流时域”。
                     * 因此：原始时间 = 输出时间 * app->speed
                     */
                    double step = (event.key.keysym.sym == SDLK_RIGHT) ? 5.0 : -5.0; // 输出时域 5秒
                    double cur_out = clock_get_master(app);
                    double target_out = cur_out + step;
                    
                    double target_orig = target_out * app->speed;
                    double duration_orig = 0.0;

                    if (app->fmt_ctx) {
                        duration_orig = (double)app->fmt_ctx->duration / AV_TIME_BASE;
                    }

                    if (target_orig < 0.0) {
                        target_orig = 0.0;
                    }
                    if (duration_orig > 0 && target_orig > duration_orig) {
                        target_orig = duration_orig;
                    }

                    app->seek_pos   = (int64_t)(target_orig * AV_TIME_BASE); // 毫秒
                    app->seek_flags = (step < 0) ? AVSEEK_FLAG_BACKWARD : 0;
                    app->seek_req   = 1; // 触发全队列 Flush

                    /*
                     * 立即重置时钟基准：
                     * 新到达的帧其 PTS 也会被除以 speed，因此它将在 target_orig / speed。
                     */
                    app->frame_timer      = av_gettime_relative() / 1000000.0;
                    app->frame_last_pts   = target_orig / app->speed; // 输出时域
                    app->frame_last_delay = 0.04;
                    app->audio_output_idle = 0;
                    break;
                }
                case SDLK_COMMA:  /* < 键：减速 */
                case SDLK_PERIOD: /* > 键：加速 */
                {
                    if (app->is_rtsp) {
                        fprintf(stderr, "RTSP流不支持倍速功能\n");
                        break;
                    }
                    static const double speed_levels[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
                    static const int    n_levels = (int)(sizeof(speed_levels) / sizeof(speed_levels[0]));
                    int i;
                    int cur_idx = 2; /* 默认指向 1.0 */
                    double old_speed = app->speed;

                    for (i = 0; i < n_levels; i++) {
                        if (app->speed <= speed_levels[i] + 0.01) {
                            cur_idx = i;
                            break;
                        }
                    }

                    if (event.key.keysym.sym == SDLK_PERIOD) {
                        cur_idx = (cur_idx + 1 < n_levels) ? cur_idx + 1 : n_levels - 1;
                    } else {
                        cur_idx = (cur_idx - 1 >= 0) ? cur_idx - 1 : 0;
                    }

                    if (speed_levels[cur_idx] == old_speed) {
                        break;
                    }

                    app->speed = speed_levels[cur_idx];
                    
                    /*
                     * 发生变速时，如果不全盘清空，缓存队列里“以老速度计算的 PTS”的包
                     * 就会和“以新速度计算的 PTS”新包混杂，引发时光倒流式的严重卡顿。
                     * 最干净的做法：触发一次无缝的“原地 Seek”！
                     */
                    double cur_out  = clock_get_master(app);
                    double cur_orig = cur_out * old_speed; 

                    app->seek_pos   = (int64_t)(cur_orig * AV_TIME_BASE);
                    app->seek_flags = 0;
                    app->seek_req   = 1; // 触发解码、各种队列的 flush，外加 atempo 滤镜重建(flush 逻辑里写的)

                    /* 重置时序基准：新包即以 cur_orig / 新speed 入局 */
                    app->frame_timer      = av_gettime_relative() / 1000000.0;
                    app->frame_last_pts   = cur_orig / app->speed; 
                    app->frame_last_delay = 0.04;
                    app->audio_output_idle = 0;

                    fprintf(stderr, "[speed] 切换至 %.2fx\n", app->speed);
                    break;
                }
                case SDLK_UP:
                case SDLK_DOWN:
                {
                    int step = (event.key.keysym.sym == SDLK_UP) ? 8 : -8;
                    app->volume += step;

                    if (app->volume < 0) {
                        app->volume = 0;
                    } else if (app->volume > SDL_MIX_MAXVOLUME) {
                        app->volume = SDL_MIX_MAXVOLUME;
                    }

                    int volume_percent = (int)((app->volume * 100.0) / SDL_MIX_MAXVOLUME);
                    fprintf(stderr, "[volume] 显示音量: %d%%\n", volume_percent);
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