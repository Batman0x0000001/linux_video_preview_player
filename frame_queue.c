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

/*
 * g_flush_pkt  — 全局哨兵 AVPacket，生命周期同进程。
 * g_flush_sentinel — 1 字节标记，其地址赋给 g_flush_pkt->data 作为唯一身份。
 *
 * 识别方式：pkt->data == &g_flush_sentinel（地址比较），
 * 而非 pkt == g_flush_pkt（指针比较）。
 * 这是因为 packet_queue_get 通过 av_packet_move_ref 把 data 字段
 * 复制到 dst_pkt，AVPacket 实例地址会变，但 data 地址保持不变。
 */
static AVPacket  *g_flush_pkt      = NULL;
static uint8_t    g_flush_sentinel = 0;   /* 仅用地址，值无意义 */

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

        if(node->pkt && node->pkt != g_flush_pkt){
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

int frame_queue_size(FrameQueue *frame_q)
{
    int size = 0;

    if (!frame_q) {
        return 0;
    }

    SDL_LockMutex(frame_q->mutex);
    size = frame_q->size;
    SDL_UnlockMutex(frame_q->mutex);

    return size;
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
            packet_q->size -= node->pkt->size; /* 哨兵 size==0，不影响计数 */

            if (node->pkt == g_flush_pkt) {
                /*
                 * 哨兵包不能走 av_packet_move_ref / av_packet_free：
                 *   - move_ref 会把 g_flush_pkt 内容搬走并清空它（破坏哨兵状态）
                 *   - av_packet_free 会释放全局哨兵堆内存 → crash
                 * 改为直接把哨兵地址写入 dst_pkt->data，供 is_flush_pkt 识别。
                 */
                av_packet_unref(dst_pkt);           /* 清掉上一轮遗留内容 */
                dst_pkt->data = &g_flush_sentinel;  /* 打上哨兵标记 */
                dst_pkt->size = 0;
            } else {
                av_packet_move_ref(dst_pkt, node->pkt);
                av_packet_free(&node->pkt);
            }
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

    while(frame_q->size >= frame_q->capacity && !app->quit){
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

void packet_queue_abort(PacketQueue *packet_q){
    if(!packet_q){
        return;
    }

    SDL_LockMutex(packet_q->mutex);
    SDL_CondBroadcast(packet_q->cond);
    SDL_UnlockMutex(packet_q->mutex);
}

void frame_queue_abort(FrameQueue *frame_q){
    if(!frame_q){
        return;
    }

    SDL_LockMutex(frame_q->mutex);
    SDL_CondBroadcast(frame_q->cond);
    SDL_UnlockMutex(frame_q->mutex);
}

/* ------------------------------------------------------------------ */
/* seek 辅助                                                            */
/* ------------------------------------------------------------------ */



/*
 * packet_queue_flush — 清空队列中所有待处理包，但保留同步原语。
 * seek 前调用，丢掉旧数据为新数据腾空间。
 */
void packet_queue_flush(PacketQueue *packet_q)
{
    PacketNode *node = NULL;
    PacketNode *next = NULL;

    if (!packet_q) {
        return;
    }

    SDL_LockMutex(packet_q->mutex);

    node = packet_q->first_pkt;
    while (node) {
        next = node->next;
        /* 哨兵包是全局共享对象，不能 free，只释放节点本身 */
        if (node->pkt && node->pkt != g_flush_pkt) {
            av_packet_free(&node->pkt);
        }
        free(node);
        node = next;
    }

    packet_q->first_pkt = NULL;
    packet_q->last_pkt  = NULL;
    packet_q->nb_packets = 0;
    packet_q->size       = 0;

    SDL_UnlockMutex(packet_q->mutex);
}

/*
 * packet_queue_put_flush_pkt — 向队列插入哨兵包。
 * 解码线程取到它后知道需要刷新解码器。
 * 若 g_flush_pkt 尚未分配则在此处创建（只创建一次）。
 */
void packet_queue_put_flush_pkt(PacketQueue *packet_q)
{
    PacketNode *node = NULL;

    if (!packet_q) {
        return;
    }

    /* 延迟初始化：整个进程生命周期内只分配一次 */
    if (!g_flush_pkt) {
        g_flush_pkt = av_packet_alloc();
        if (!g_flush_pkt) {
            fprintf(stderr, "packet_queue_put_flush_pkt: av_packet_alloc failed\n");
            return;
        }
    }
    /*
     * 每次插入前重置哨兵标记：
     * packet_queue_get 的哨兵分支会调用 av_packet_unref(g_flush_pkt 指向的包内容已被清空)，
     * 所以每次插入时必须重新设置 data，确保 is_flush_pkt 能正确识别。
     */
    g_flush_pkt->data = &g_flush_sentinel;
    g_flush_pkt->size = 0;

    node = (PacketNode *)calloc(1, sizeof(PacketNode));
    if (!node) {
        return;
    }

    /*
     * 哨兵包不走 av_packet_ref，直接存指针。
     * 取出时通过 packet_queue_is_flush_pkt 识别，不调用 av_packet_free。
     */
    node->pkt  = g_flush_pkt;
    node->next = NULL;

    SDL_LockMutex(packet_q->mutex);

    if (!packet_q->last_pkt) {
        packet_q->first_pkt = node;
    } else {
        packet_q->last_pkt->next = node;
    }
    packet_q->last_pkt = node;
    packet_q->nb_packets++;
    /* size 不计入哨兵包，避免影响背压判断 */

    SDL_CondSignal(packet_q->cond);
    SDL_UnlockMutex(packet_q->mutex);
}

/*
 * packet_queue_is_flush_pkt — 判断取出的包是否是哨兵包。
 * 对比 data 字段是否指向 g_flush_sentinel，而非比较 AVPacket 指针。
 * 原因：packet_queue_get 通过 dst_pkt->data = &g_flush_sentinel 打标记，
 * dst_pkt 本身的地址（栈变量）与 g_flush_pkt 不同，指针比较会失败。
 */
int packet_queue_is_flush_pkt(const AVPacket *pkt)
{
    return (pkt != NULL && pkt->data == &g_flush_sentinel);
}

/*
 * frame_queue_flush — 清空视频帧队列中所有待显示帧，重置读写索引。
 * seek 后由视频解码线程调用，确保旧帧不会被渲染。
 */
void frame_queue_flush(FrameQueue *frame_q)
{
    if (!frame_q) {
        return;
    }

    SDL_LockMutex(frame_q->mutex);

    /*
     * 不释放 AVFrame 本身（槽位复用），只 unref 内容，
     * 然后把读写索引和 size 归零。
     */
    for (int i = 0; i < frame_q->capacity; i++) {
        if (frame_q->frames[i].frame) {
            av_frame_unref(frame_q->frames[i].frame);
        }
        frame_q->frames[i].pts_sec = 0.0;
    }

    frame_q->rindex = 0;
    frame_q->windex = 0;
    frame_q->size   = 0;

    /* 唤醒可能阻塞在"等待可写槽"的生产者 */
    SDL_CondBroadcast(frame_q->cond);
    SDL_UnlockMutex(frame_q->mutex);
}