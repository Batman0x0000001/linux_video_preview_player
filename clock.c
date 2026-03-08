#include <libavutil/time.h>
#include "clock.h"

double clock_get_video(AppState *app)
{
    double delta;

    delta = (av_gettime_relative() - app->video_current_pts_time) / 1000000.0;
    return app->video_current_pts + delta;
}