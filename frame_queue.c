#include <stdlib.h>
#include "frame_queue.h"

struct PacketQueue {
    int dummy;
};

struct FrameQueue {
    int dummy;
};

PacketQueue *packet_queue_create(void)
{
    return (PacketQueue *)calloc(1, sizeof(PacketQueue));
}

void packet_queue_destroy(PacketQueue *q)
{
    free(q);
}

FrameQueue *frame_queue_create(int capacity)
{
    (void)capacity;
    return (FrameQueue *)calloc(1, sizeof(FrameQueue));
}

void frame_queue_destroy(FrameQueue *q)
{
    free(q);
}