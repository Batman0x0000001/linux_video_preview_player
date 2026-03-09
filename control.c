#include "control.h"
#include"frame_queue.h"
#include"display.h"

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
                case SDLK_q:
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
                display_present_frame(app,vf);
                frame_queue_next(app->video_frm_queue);
            }
        }

        SDL_Delay(5);
    }

    return 0;
}