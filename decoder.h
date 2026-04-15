#ifndef DECODER_H
#define DECODER_H

#include"app.h"

int decoder_open_video(AppState *app);
int decoder_open_audio(AppState *app);
int decoder_configure_audio_target(AppState *app, int sample_rate, int channels);
int decoder_start(AppState *app);
int decoder_audio_filter_init(AppState *app); //音频变速滤镜初始化，需在 audio_output_open 之后调用

#endif