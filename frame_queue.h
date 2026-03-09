#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include"app.h"

typedef struct PacketQueue PacketQueue;
typedef struct FrameQueue FrameQueue;

PacketQueue *packet_queue_create(void);
void packet_queue_destroy(PacketQueue *packet_q);

FrameQueue *Frame_queue_create(int capacity);
void frame_queue_destroy(FrameQueue *frame_q);

int packet_queue_put(PacketQueue *packet_q,const AVPacket *src_pkt);
int packet_queue_get(AppState *app,PacketQueue *packet_q,const AVPacket *dst_pkt,int block);
int packet_queue_size(PacketQueue *packet_q);

#endif