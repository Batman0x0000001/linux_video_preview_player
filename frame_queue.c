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

/*
帧队列不是链表，而是一个固定容量的环形队列
    视频显示通常不需要无限积压帧
    固定容量更容易控制内存
    生产者/消费者模型很清晰
    环形队列对视频帧这种“先进先出、容量受限”的场景非常合适
*/
struct FrameQueue {
    VideoFrame *frames;
    int capacity;
    int size;
    int rindex;
    int windex;

    SDL_mutex *mutex;
    SDL_cond *cond;
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

    frame_q->frames = (VideoFrame *)calloc((size_t)capacity,sizeof(VideoFrame));
    if(!frame_q->frames){
        free(frame_q);
        return NULL;
    }

    frame_q->capacity = capacity;
    frame_q->size = 0;
    frame_q->rindex = 0;
    frame_q->windex = 0;

    frame_q->mutex = SDL_CreateMutex();
    frame_q->cond = SDL_CreateCond();
    if (!frame_q->mutex || !frame_q->cond) {
        if (frame_q->cond) {
            SDL_DestroyCond(frame_q->cond);
        }
        if (frame_q->mutex) {
            SDL_DestroyMutex(frame_q->mutex);
        }
        free(frame_q->frames);
        free(frame_q);
        return NULL;
    }

    for (int i = 0; i < capacity; i++)
    {
        //[i]下标访问本身就包含了解引用操作（等价于 *(frames+i)），结果是结构体值而非指针，所以后面用 . 而不是 ->。
        frame_q->frames[i].frame = av_frame_alloc();
        frame_q->frames[i].pts_sec = 0.0;

        if(!frame_q->frames[i].frame){
            frame_queue_destroy(frame_q);
            return NULL;
        }
    }
    
    return frame_q;
}

void frame_queue_destroy(FrameQueue *frame_q)
{
    if(!frame_q){
        return;
    }

    SDL_LockMutex(frame_q->mutex);

    if(frame_q->frames){
        for (int i = 0; i < frame_q->capacity; i++)
        {
            if(frame_q->frames[i].frame){
                av_frame_free(&frame_q->frames[i].frame);
            }
        }
        free(frame_q->frames);
        frame_q->frames = NULL;
    }

    frame_q->capacity = 0;
    frame_q->size = 0;
    frame_q->rindex = 0;
    frame_q->windex = 0;
    
    SDL_UnlockMutex(frame_q->mutex);
    SDL_DestroyMutex(frame_q->mutex);
    SDL_DestroyCond(frame_q->cond);

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

int packet_queue_get(AppState *app,PacketQueue *packet_q,AVPacket *dst_pkt,int block){
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

int frame_queue_peek_writable(AppState *app,FrameQueue *frame_q,VideoFrame **vf){
    SDL_LockMutex(frame_q->mutex);

    while(frame_q->size > frame_q->capacity && !app->quit){
        SDL_CondWait(frame_q->cond,frame_q->mutex);
    }

    if(app->quit){
        SDL_UnlockMutex(frame_q->mutex);
        return -1;
    }

    *vf = &frame_q->frames[frame_q->windex];
    SDL_UnlockMutex(frame_q->mutex);

    return 0;
}

void frame_queue_push(FrameQueue *frame_q){
    SDL_LockMutex(frame_q->mutex);

    frame_q->windex++;
    if(frame_q->windex == frame_q->capacity){
        frame_q->windex = 0;
    }

    //size 字段来区分"满"和"空"的状态，size >= capacity 时生产者等待，size > 0 时消费者才能读取
    frame_q->size++;

    SDL_CondSignal(frame_q->cond);
    SDL_UnlockMutex(frame_q->mutex);
}

int frame_queue_peek_readable(AppState *app,FrameQueue *frame_q,VideoFrame **vf){
    SDL_LockMutex(frame_q->mutex);

    while(frame_q->size <= 0 && !app->quit){
        SDL_CondWait(frame_q->cond,frame_q->mutex);
    }

    if(app->quit){
        SDL_UnlockMutex(frame_q->mutex);
        return -1;
    }

    *vf = &frame_q->frames[frame_q->rindex];
    SDL_UnlockMutex(frame_q->mutex);

    return 0;
}

void frame_queue_next(FrameQueue *frame_q){
    SDL_LockMutex(frame_q->mutex);

    frame_q->rindex++;
    if(frame_q->rindex == frame_q->capacity){
        frame_q->rindex = 0;
    }

    frame_q->size--;

    SDL_CondSignal(frame_q->cond);
    SDL_UnlockMutex(frame_q->mutex);
}

int frame_queue_try_peek_readable(FrameQueue *frame_q,VideoFrame **vf){
    int ret = 0;

    SDL_LockMutex(frame_q->mutex);

    if(frame_q->size > 0){
        *vf = &frame_q->frames[frame_q->rindex];
        ret = 1;
    }else{
        *vf = NULL;
        ret = 0;
    }

    SDL_UnlockMutex(frame_q->mutex);
    
    return ret;
}