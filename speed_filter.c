#include <stdio.h>
#include <string.h>
#include <libavutil/opt.h>
#include "speed_filter.h"

static void print_ffmpeg_error_sf(const char *msg, int errnum)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, errbuf, sizeof(errbuf));
    fprintf(stderr, "[speed_filter] %s: %s\n", msg, errbuf);
}

/*
 * build_filter_graph — 核心构建函数，内部使用。
 *
 * 构建路径：abuffersrc → atempo → abuffersink
 *
 * FFmpeg 6.x 的 abuffer filter 需要传入 channel_layout 字符串（新 API），
 * 通过 av_channel_layout_describe 获得。
 */
static int build_filter_graph(AppState *app)
{
    const AVFilter *abuffer_flt    = NULL; //abuffersrc 滤镜
    const AVFilter *atempo_flt     = NULL; //atempo 时间拉伸滤镜
    const AVFilter *abuffersink_flt = NULL; //abuffersink 滤镜

    AVFilterGraph   *graph    = NULL; //新滤镜图
    AVFilterContext *src_ctx  = NULL; //abuffersrc context
    AVFilterContext *atempo_ctx = NULL; //atempo context
    AVFilterContext *sink_ctx = NULL; //abuffersink context

    char args[256];     //abuffersrc 初始化参数字符串
    char ch_desc[64];   //声道布局描述字符串（FFmpeg 6.x 新 API）
    int ret;

    /* 1. 获取各滤镜对象 */
    abuffer_flt     = avfilter_get_by_name("abuffer");
    atempo_flt      = avfilter_get_by_name("atempo");
    abuffersink_flt = avfilter_get_by_name("abuffersink");
    if (!abuffer_flt || !atempo_flt || !abuffersink_flt) {
        fprintf(stderr, "[speed_filter] avfilter_get_by_name failed\n");
        return AVERROR_FILTER_NOT_FOUND;
    }

    /* 2. 创建滤镜图 */
    graph = avfilter_graph_alloc();
    if (!graph) {
        return AVERROR(ENOMEM);
    }

    /* 3. 构建 abuffersrc 参数字符串
     *    time_base = 1/sample_rate（每个 time_unit = 一个采样）
     *    channel_layout 使用 FFmpeg 6.x av_channel_layout_describe
     */
    ret = av_channel_layout_describe(&app->audio_src.ch_layout,
                                     ch_desc, sizeof(ch_desc));
    if (ret < 0) {
        print_ffmpeg_error_sf("av_channel_layout_describe failed", ret);
        goto fail;
    }

    snprintf(args, sizeof(args),
             "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             app->audio_src.sample_rate,
             app->audio_src.sample_rate,
             av_get_sample_fmt_name(app->audio_src.fmt),
             ch_desc);

    /* 4. 创建各 context */
    ret = avfilter_graph_create_filter(&src_ctx, abuffer_flt,
                                       "in", args, NULL, graph);
    if (ret < 0) {
        print_ffmpeg_error_sf("创建 abuffersrc 失败", ret);
        goto fail;
    }

    /* atempo 参数：%.6f 保证足够精度 */
    snprintf(args, sizeof(args), "%.6f", app->speed);
    ret = avfilter_graph_create_filter(&atempo_ctx, atempo_flt,
                                       "atempo", args, NULL, graph);
    if (ret < 0) {
        print_ffmpeg_error_sf("创建 atempo 失败", ret);
        goto fail;
    }

    ret = avfilter_graph_create_filter(&sink_ctx, abuffersink_flt,
                                       "out", NULL, NULL, graph);
    if (ret < 0) {
        print_ffmpeg_error_sf("创建 abuffersink 失败", ret);
        goto fail;
    }

    /* 约束 abuffersink 输出格式和原始格式一致，防止 atempo 将 FLTP 转为 FLT 导致下游 swr_convert 崩溃 */
    enum AVSampleFormat out_sample_fmts[] = { app->audio_src.fmt, AV_SAMPLE_FMT_NONE };
    ret = av_opt_set_int_list(sink_ctx, "sample_fmts", out_sample_fmts, -1, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        print_ffmpeg_error_sf("设置 sink sample_fmts 失败", ret);
        goto fail;
    }

    int out_sample_rates[] = { app->audio_src.sample_rate, -1 };
    ret = av_opt_set_int_list(sink_ctx, "sample_rates", out_sample_rates, -1, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        print_ffmpeg_error_sf("设置 sink sample_rates 失败", ret);
        goto fail;
    }

    /* FFmpeg 6.x 中 channel_layouts 设置的是 AVChannelLayout 的表示，但简单起见我们也可以用老版的设置方式，
     * 或者让 atempo 保持 layout 不变（atempo 通常不改变声道布局）。
     * 为了安全起见，设置声道列表。 */
#if LIBAVUTIL_VERSION_MAJOR < 58
    int64_t out_channel_layouts[] = { app->audio_src.channel_layout, -1 };
    av_opt_set_int_list(sink_ctx, "channel_layouts", out_channel_layouts, -1, AV_OPT_SEARCH_CHILDREN);
#else
    AVChannelLayout ch_layouts[] = { app->audio_src.ch_layout, (AVChannelLayout){0} };
    /* FFmpeg 6.1 的 abuffersink 提供了 ch_layouts 选项用于传入 AVChannelLayout 数组 */
    av_opt_set_bin(sink_ctx, "ch_layouts", (const uint8_t *)ch_layouts, sizeof(AVChannelLayout), AV_OPT_SEARCH_CHILDREN);
#endif

    /* 5. 链接：src → atempo → sink */
    ret = avfilter_link(src_ctx, 0, atempo_ctx, 0);
    if (ret < 0) {
        print_ffmpeg_error_sf("avfilter_link src→atempo 失败", ret);
        goto fail;
    }

    ret = avfilter_link(atempo_ctx, 0, sink_ctx, 0);
    if (ret < 0) {
        print_ffmpeg_error_sf("avfilter_link atempo→sink 失败", ret);
        goto fail;
    }

    /* 6. 校验并生效 */
    ret = avfilter_graph_config(graph, NULL);
    if (ret < 0) {
        print_ffmpeg_error_sf("avfilter_graph_config 失败", ret);
        goto fail;
    }

    /* 7. 写入 AppState（先 free 旧图）*/
    if (app->audio_fg) {
        avfilter_graph_free(&app->audio_fg);
    }
    app->audio_fg      = graph;
    app->audio_fg_src  = src_ctx;
    app->audio_fg_sink = sink_ctx;

    return 0;

fail:
    avfilter_graph_free(&graph);
    return ret;
}

int speed_filter_init(AppState *app)
{
    if (!app || app->audio_stream_index < 0) {
        return 0; //无音频流，跳过
    }

    if (app->audio_src.sample_rate <= 0 ||
        app->audio_src.ch_layout.nb_channels <= 0) {
        fprintf(stderr, "[speed_filter] audio_src 参数未初始化，跳过滤镜初始化\n");
        return AVERROR(EINVAL);
    }

    return build_filter_graph(app);
}

int speed_filter_rebuild(AppState *app)
{
    /*
     * 先释放旧图（build_filter_graph 内部也会 free，但显式调用更清晰）。
     * 旧图内部缓冲的 PCM 会丢失，由此产生短暂音频过渡，预览场景可接受。
     */
    if (app->audio_fg) {
        avfilter_graph_free(&app->audio_fg);
        app->audio_fg      = NULL;
        app->audio_fg_src  = NULL;
        app->audio_fg_sink = NULL;
    }

    if (app->audio_stream_index < 0) {
        return 0;
    }

    return build_filter_graph(app);
}

void speed_filter_free(AppState *app)
{
    if (!app) {
        return;
    }

    if (app->audio_fg) {
        avfilter_graph_free(&app->audio_fg);
        app->audio_fg      = NULL;
        app->audio_fg_src  = NULL;
        app->audio_fg_sink = NULL;
    }
}

int speed_filter_send(AppState *app, AVFrame *frame)
{
    int ret;

    if (!app || !app->audio_fg_src) {
        return AVERROR(EINVAL);
    }

    /*
     * frame == NULL 表示向滤镜发送 EOF（drain/flush 信号）。
     * AV_BUFFERSRC_FLAG_KEEP_REF: 不让滤镜偷走 frame 的 ref，调用方自行管理。
     */
    if (frame) {
        ret = av_buffersrc_add_frame_flags(app->audio_fg_src, frame,
                                           AV_BUFFERSRC_FLAG_KEEP_REF);
    } else {
        ret = av_buffersrc_add_frame_flags(app->audio_fg_src, NULL, 0);
    }

    if (ret < 0 && ret != AVERROR_EOF) {
        print_ffmpeg_error_sf("av_buffersrc_add_frame_flags 失败", ret);
    }

    return ret;
}

int speed_filter_receive(AppState *app, AVFrame *frame)
{
    if (!app || !app->audio_fg_sink || !frame) {
        return AVERROR(EINVAL);
    }

    /*
     * 返回值：
     *   0           — 成功取到一帧
     *   AVERROR(EAGAIN) — 暂无输出，需要继续送入更多输入帧
     *   AVERROR_EOF     — 滤镜已耗尽（drain 完成）
     */
    return av_buffersink_get_frame(app->audio_fg_sink, frame);
}
