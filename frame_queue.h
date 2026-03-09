#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include"app.h"

typedef struct PacketQueue PacketQueue;//压缩码流边界
typedef struct FrameQueue FrameQueue;//原始图像边界

PacketQueue *packet_queue_create(void);
void packet_queue_destroy(PacketQueue *packet_q);

FrameQueue *frame_queue_create(int capacity);
void frame_queue_destroy(FrameQueue *frame_q);

int packet_queue_put(PacketQueue *packet_q,const AVPacket *src_pkt);
int packet_queue_get(AppState *app,PacketQueue *packet_q,AVPacket *dst_pkt,int block);
int packet_queue_size(PacketQueue *packet_q);

void frame_queue_push(FrameQueue *frame_q);
void frame_queue_next(FrameQueue *frame_q);
int frame_queue_peek_writable(AppState *app,FrameQueue *frame_q,VideoFrame **vf);
int frame_queue_peek_readable(AppState *app,FrameQueue *frame_q,VideoFrame **vf);
int frame_queue_try_peek_readable(FrameQueue *frame_q,VideoFrame **vf);

#endif