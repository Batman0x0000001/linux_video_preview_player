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

    app->texture = NULL;
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