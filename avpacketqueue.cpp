#include "avpacketqueue.h"

AvPacketQueue::AvPacketQueue()
{
    mutex   = SDL_CreateMutex();
    cond    = SDL_CreateCond();
}

// 原始帧入队
void AvPacketQueue::enqueue(AVPacket *packet)
{
    AVPacket pktInQueue;
    av_init_packet(&pktInQueue);
    // 在锁外面先接管所有权，减少锁持有的时间
    if (av_packet_ref(&pktInQueue, packet) < 0) {
        // 如果 ref 失败（通常是因为 packet->buf 为空，比如你的常量字符串包）
        // 那就手动拷贝一份数据
        pktInQueue = *packet;
    }

    SDL_LockMutex(mutex);

    queue.enqueue(pktInQueue);

    SDL_CondSignal(cond);
    SDL_UnlockMutex(mutex);
}


/**
 * @brief 从队列中提取一个 AVPacket
 * * @param packet  用于存储提取出的数据包指针
 * @param isBlock 是否采用阻塞模式。true: 队列为空则等待；false: 立即返回
 */
void AvPacketQueue::dequeue(AVPacket *packet, bool isBlock)
{
    // 1. 进入临界区：锁定互斥锁，确保多线程下对 queue 操作的原子性
    SDL_LockMutex(mutex);

    while (1) {
        // 2. 检查队列是否有数据
        if (!queue.isEmpty()) {
            // 成功获取：从 Qt 队列中弹出头部数据包并赋值
            *packet = queue.dequeue();
            break;
        }
        // 3. 队列为空时的处理逻辑
        else if (!isBlock) {
            // 非阻塞模式：如果不允许等待，直接跳出循环
            break;
        }
        else {
            /* * 4. 阻塞模式：队列为空，线程进入休眠等待
             * SDL_CondWait 会执行以下原子操作：
             * a) 释放互斥锁 mutex，允许“生产者”线程进入并添加数据。
             * b) 线程进入休眠，等待 cond 信号唤醒。
             * c) 被唤醒后，重新抢占锁 mutex，然后继续下一次 while 循环检查。
             */
            SDL_CondWait(cond, mutex);
        }
    }

    // 5. 退出临界区：释放互斥锁，允许其他线程访问
    SDL_UnlockMutex(mutex);
}


// 清空队列
void AvPacketQueue::empty()
{
    SDL_LockMutex(mutex);
    while (queue.size() > 0) {
        AVPacket packet = queue.dequeue();
        av_packet_unref(&packet);
    }

    SDL_UnlockMutex(mutex);
}

bool AvPacketQueue::isEmpty()
{
    return queue.isEmpty();
}

int AvPacketQueue::queueSize()
{
    return queue.size();
}
