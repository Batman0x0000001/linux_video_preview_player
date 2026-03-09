#ifndef CLOCK_H
#define CLOCK_H

#include"app.h"

double clock_get_video(AppState *app);
void clock_update_video(AppState *app,double pts_sec);

#endif