#include "control.h"

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
            }
        }

        SDL_Delay(10);
    }

    return 0;
}