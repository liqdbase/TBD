#ifndef FEMU_SHM_LIB_H
#define FEMU_SHM_LIB_H

#include "femu_shm_defs.h"
#include <stdio.h>

// =========================================================
// Submission Queue (SQ) 조작 함수
// =========================================================

// [Producer] FEMU가 사용: 명령 넣기
static inline int sq_enqueue(SqRing *q, ShmCmd *cmd) {
    unsigned int tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    unsigned int next_tail = (tail + 1) % RING_SIZE;
    unsigned int head = atomic_load_explicit(&q->head, memory_order_acquire);

    if (next_tail == head) {
        return 0; // Queue Full (Backpressure 발생 지점)
    }

    q->cmds[tail] = *cmd;
    atomic_store_explicit(&q->tail, next_tail, memory_order_release);
    return 1; // Success
}

// [Consumer] RTOS가 사용: 명령 꺼내기
static inline int sq_dequeue(SqRing *q, ShmCmd *out_cmd) {
    unsigned int head = atomic_load_explicit(&q->head, memory_order_relaxed);
    unsigned int tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if (head == tail) {
        return 0; // Empty
    }

    *out_cmd = q->cmds[head];
    unsigned int next_head = (head + 1) % RING_SIZE;
    atomic_store_explicit(&q->head, next_head, memory_order_release);
    return 1; // Success
}

// =========================================================
// Completion Queue (CQ) 조작 함수
// =========================================================

// [Producer] RTOS가 사용: 완료 응답 넣기
static inline int cq_enqueue(CqRing *q, ShmCpl *cpl) {
    unsigned int tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    unsigned int next_tail = (tail + 1) % RING_SIZE;
    unsigned int head = atomic_load_explicit(&q->head, memory_order_acquire);

    if (next_tail == head) return 0; // Should not happen if sized correctly

    q->cpls[tail] = *cpl;
    atomic_store_explicit(&q->tail, next_tail, memory_order_release);
    return 1;
}

// [Consumer] FEMU가 사용: 완료 응답 확인
static inline int cq_dequeue(CqRing *q, ShmCpl *out_cpl) {
    unsigned int head = atomic_load_explicit(&q->head, memory_order_relaxed);
    unsigned int tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if (head == tail) return 0;

    *out_cpl = q->cpls[head];
    unsigned int next_head = (head + 1) % RING_SIZE;
    atomic_store_explicit(&q->head, next_head, memory_order_release);
    return 1;
}

#endif // FEMU_SHM_LIB_H