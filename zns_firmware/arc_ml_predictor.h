

#ifndef ARC_ML_PREDICTOR_H
#define ARC_ML_PREDICTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>


typedef struct {
    atomic_uint_fast32_t sequence;
} seqlock_t_local;


static inline void seqlock_init_local(seqlock_t_local* lock) {
    atomic_init(&lock->sequence, 0);
}


static inline uint32_t seqlock_read_begin_local(const seqlock_t_local* lock) {
    uint32_t seq;
    while (1) {
        seq = atomic_load_explicit(&lock->sequence, memory_order_acquire);
        if ((seq & 1) == 0) {  // 짝수인 경우 (쓰기 중이 아님)
            return seq;
        }
        // 쓰기 중이면 CPU yield
        #ifdef __x86_64__
        __asm__ __volatile__("pause" ::: "memory");
        #endif
    }
}


static inline bool seqlock_read_retry_local(const seqlock_t_local* lock, uint32_t seq) {
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(&lock->sequence, memory_order_relaxed) != seq;
}


typedef struct PageFeatures {
    seqlock_t_local lock;
    int access_count;
    long last_access_tick;
    int list_type;
    long last_reuse_distance;
    float avg_reuse_interval;
    float reuse_variance;
    int reuse_count;
    long* reuse_history;
    int history_size;
    float neighbor_avg_access;
    float neighbor_avg_idle;
} PageFeatures_local;


/**
 * 페이지의 재사용 확률 예측
 * @param features: 22개의 피처 배열
 * @param num_features: 피처 개수 (반드시 22여야 함)
 * @return: 예측 점수 (0.0 ~ 1.0, 높을수록 재사용 가능성 높음)
 */
float ml_predict_reuse_probability(const float* features, int num_features);

/**
 * 원자적으로 피처를 복사하여 배열에 저장
 * @param src: PageFeatures_local 구조체 포인터
 * @param dest: float 배열 (최소 22개 요소)
 * @return: 재시도 횟수 (음수면 실패)
 */
int copy_features_atomic(const PageFeatures_local* src, float* dest);

/**
 * Thread-safe 예측 래퍼 함수 (copy_features_atomic + ml_predict_reuse_probability)
 * @param page_feat: 페이지 피처 구조체
 * @return: 예측 점수
 */
float predict_reuse_probability_safe(PageFeatures_local* page_feat);

#endif // ARC_ML_PREDICTOR_H