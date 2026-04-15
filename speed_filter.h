/*
 * speed_filter.h — atempo 时间拉伸滤镜图模块
 *
 * 职责：
 *   管理一个 abuffer → atempo → abuffersink 滤镜图，
 *   实现音频变速不变调（Pitch-preserving time stretch）。
 *
 * 流程：
 *   1. speed_filter_init()    — decoder_open_audio 之后调用，以 app->speed 构建滤镜图。
 *   2. speed_filter_send()    — 将每帧解码后的 PCM（fltp）送入 abuffersrc。
 *   3. speed_filter_receive() — 从 abuffersink 取出经拉伸的帧（仍为 fltp）。
 *                               返回 0=有输出帧，EAGAIN=需更多输入，AVERROR_EOF=耗尽。
 *   4. speed_filter_rebuild() — 速度变更 / seek 后重建（释放旧图 + 重新初始化）。
 *   5. speed_filter_free()    — 释放所有资源，随 app 生命周期结束调用。
 *
 * 注意：
 *   - atempo 支持范围 [0.5, 2.0]，超出需链式（本项目档位均在范围内）。
 *   - 输出帧 pts 位于"输出时域"（= 原始时域 / speed），由调用方负责转成秒。
 *   - 所有函数均在音频解码线程内调用，无需额外加锁。
 */
#ifndef SPEED_FILTER_H
#define SPEED_FILTER_H

#include "app.h"

/*
 * speed_filter_init — 根据 app->audio_src 和 app->speed 构建 atempo 滤镜图。
 * 在 decoder_open_audio 之后、音频解码线程启动之前调用。
 * 返回 0 成功，负数为 FFmpeg 错误码。
 */
int speed_filter_init(AppState *app);

/*
 * speed_filter_rebuild — 速度切换或 seek 后重建滤镜图（旧图先释放再重建）。
 * 内部缓冲数据会丢失，可能有短暂音频过渡，播放预览场景可接受。
 */
int speed_filter_rebuild(AppState *app);

/*
 * speed_filter_free — 释放滤镜图及相关资源，置 audio_fg/src/sink 为 NULL。
 */
void speed_filter_free(AppState *app);

/*
 * speed_filter_send — 将一帧原始 PCM 送入 abuffersrc。
 * frame=NULL 表示发送 EOF 信号（drain/flush）。
 * 返回 0 成功，负数为 FFmpeg 错误码。
 */
int speed_filter_send(AppState *app, AVFrame *frame);

/*
 * speed_filter_receive — 从 abuffersink 取一帧经时间拉伸的 PCM。
 * 返回 0=有帧，AVERROR(EAGAIN)=暂无输出，AVERROR_EOF=滤镜已耗尽。
 * 调用方负责在取到帧后调用 av_frame_unref。
 */
int speed_filter_receive(AppState *app, AVFrame *frame);

#endif
