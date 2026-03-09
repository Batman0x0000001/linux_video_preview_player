#include <stdlib.h>
#include "frame_queue.h"

typedef struct PacketNode {
    AVPacket *pkt;
    struct PacketNode *next;
}PacketNode;

struct PacketQueue {
    PacketNode *first_pkt;
    PacketNode *last_pkt;
    int nb_packets;
    int size;

    SDL_mutex *mutex;
    SDL_cond *cond;
};

struct FrameQueue {
    int capacity;
    int dummy;
};

PacketQueue *packet_queue_create(void)
{
    PacketQueue *packet_q = (PacketQueue *)calloc(1, sizeof(PacketQueue));
    if(!packet_q){
        return NULL;
    }

    packet_q->mutex = SDL_CreateMutex();
    packet_q->cond = SDL_CreateCond();

    if (!packet_q->mutex || !packet_q->cond) {
        if (packet_q->cond) {
            SDL_DestroyCond(packet_q->cond);
        }
        if (packet_q->mutex) {
            SDL_DestroyMutex(packet_q->mutex);
        }
        free(packet_q);
        return NULL;
    }

    return packet_q;
}

void packet_queue_destroy(PacketQueue *packet_q)
{
    PacketNode *node = NULL;
    PacketNode *next = NULL;
    
    if(!packet_q){
        return;
    }

    SDL_LockMutex(packet_q->mutex);

    node = packet_q->first_pkt;
    while(node){
        next = node->next;

        if(node->pkt){
            av_packet_free(&node->pkt);
        }
        free(node);

        node = next;
    }

    packet_q->first_pkt = NULL;
    packet_q->last_pkt = NULL;
    packet_q->nb_packets = 0;
    packet_q->size = 0;
    
    SDL_UnlockMutex(packet_q->mutex);

    SDL_DestroyMutex(packet_q->mutex);
    SDL_DestroyCond(packet_q->cond);

    free(packet_q);
}

FrameQueue *frame_queue_create(int capacity)
{
    FrameQueue *frame_q = (FrameQueue *)calloc(1, sizeof(FrameQueue));
    if (!frame_q) {
        return NULL;
    }

    frame_q->capacity = capacity;
    return frame_q;
}

void frame_queue_destroy(FrameQueue *frame_q)
{
    free(frame_q);
}

int packet_queue_put(PacketQueue *packet_q,const AVPacket *src_pkt){
    PacketNode *node = NULL;
    AVPacket *pkt = NULL;

    pkt = av_packet_alloc();
    if (!pkt) {
        return AVERROR(ENOMEM);
    }

    if (av_packet_ref(pkt, src_pkt) < 0) {
        av_packet_free(&pkt);
        return AVERROR(ENOMEM);
    }

    node = (PacketNode *)calloc(1, sizeof(PacketNode));
    if (!node) {
        av_packet_free(&pkt);
        return AVERROR(ENOMEM);
    }
    
    node->pkt = pkt;
    node->next = NULL;

    SDL_LockMutex(packet_q->mutex);

    if(!packet_q->last_pkt){
        packet_q->first_pkt = node;
    }else{
        packet_q->last_pkt->next = node;
    }
    packet_q->last_pkt = node;

    packet_q->nb_packets++;
    packet_q->size += pkt->size;

    SDL_CondSignal(packet_q->cond);
    SDL_UnlockMutex(packet_q->mutex);

    return 0;
}

int packet_queue_get(AppState *app,PacketQueue *packet_q,const AVPacket *dst_pkt,int block){
    PacketNode *node = NULL;
    int ret = 0;

    SDL_LockMutex(packet_q->mutex);

    while(1){
        if(app->quit){
            ret = -1;
            break;
        }

        node = packet_q->first_pkt;
        if(node){
            packet_q->first_pkt = node->next;
            if(!packet_q->first_pkt){
                packet_q->last_pkt = NULL;
            }

            packet_q->nb_packets--;
            packet_q->size -= node->pkt->size;

            av_packet_move_ref(dst_pkt,node->pkt);
            av_packet_free(&node->pkt);
            free(node);

            ret = 1;
            break;
        }

        if(!block){
            ret = 0;
            break;
        }

        SDL_CondWait(packet_q->cond,packet_q->mutex);
    }

    SDL_UnlockMutex(packet_q->mutex);

    return ret;
    /*
    ret:
        1：成功拿到一个包
        0：当前没包，但调用者要求非阻塞
        -1：退出或异常
    */
}

int packet_queue_size(PacketQueue *packet_q){
    int size;

    SDL_LockMutex(packet_q->mutex);
    size = packet_q->size;
    SDL_UnlockMutex(packet_q->mutex);

    return size;
}