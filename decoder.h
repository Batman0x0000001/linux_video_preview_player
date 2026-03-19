#ifndef DECODER_H
#define DECODER_H

#include"app.h"

int decoder_open_video(AppState *app);
int decoder_open_audio(AppState *app);
int decoder_configure_audio_target(AppState *app, int sample_rate, int channels);
int decoder_start(AppState *app);

#endif