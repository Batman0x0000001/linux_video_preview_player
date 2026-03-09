#include "control.h"
#include"frame_queue.h"
#include"display.h"
#include"clock.h"
#include <libavutil/time.h>

static int frame_should_present(AppState *app,const VideoFrame *vf){
    double delay;
    double target_time;
    double now;

    delay = vf->pts_sec - app->frame_last_pts;
    //出现异常就复用上一帧的 delay，再不行就用 0.04 兜底。
    if(delay <= 0.0 || delay >= 1.0){
        delay = app->frame_last_delay;
    }

    if (delay <= 0.0) {
        delay = 0.04;
    }

    target_time = app->frame_timer + delay;
    now = av_gettime_relative() / 1000000.0;//当前时间（微秒转换为秒）

    //当前时间已经到达目标时间:加了 1ms 的容忍误差，避免因系统调度延迟导致帧永远显示不出来
    if(now + 0.001 >=target_time){
        app->frame_last_delay = delay;
        app->frame_last_pts = vf->pts_sec;
        app->frame_timer = target_time;
        
        return 1;
    }

    return 0;
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
                    break;
                case SDLK_SPACE:
                    app->paused = !app->paused;
                    break;
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
            if(frame_queue_try_peek_readable(app->video_frm_queue,&vf)>0 && vf){
                if(frame_should_present(app,vf)){
                    display_present_frame(app,vf);
                    clock_update_video(app,vf->pts_sec);
                    frame_queue_next(app->video_frm_queue);
                }
                
            }
        }

        SDL_Delay(1);
    }

    return 0;
}