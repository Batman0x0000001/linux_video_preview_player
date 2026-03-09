#ifndef DISPLAY_H
#define DISPLAY_H

#include"app.h"

int display_init(AppState *app);
void display_destroy(AppState *app);

int display_present_frame(AppState *app,const VideoFrame *vf);

#endif