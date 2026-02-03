/* policy.h */
#ifndef POLICY_H
#define POLICY_H

#include <stdint.h>
#include <stdbool.h>

/* 정책 인터페이스 정의 */
typedef struct {
    const char *name;
    void (*init)(void);
    void (*on_hit)(int slot_index, uint64_t lba);
    void (*on_miss)(uint64_t lba, int phys_slot);
    int (*select_victim)(void);
    void (*print_stats)(void);
    void (*set_lba)(uint64_t lba);
} CachePolicy;

/* 전역 변수 및 정책 인스턴스 선언 */
extern CachePolicy *current_policy;
extern CachePolicy policy_fifo;
extern CachePolicy policy_arc;
extern CachePolicy policy_ml_arc;
#endif /* POLICY_H */