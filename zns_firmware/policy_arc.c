
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include "policy.h"

#define CACHE_SIZE 262144
#define HASH_SIZE  1048576  // 2^20 (비트 연산 최적화)
#define HASH_MASK  (HASH_SIZE - 1)

#define HASH_EMPTY   -1
#define HASH_DELETED -2 

typedef enum { LIST_NONE=0, LIST_T1, LIST_T2, LIST_B1, LIST_B2 } ArcListType;

typedef struct {
    uint64_t lba;
    int prev;
    int next;
    int phys_slot;
    ArcListType type;
} ArcNode;

// [전역 변수]
static pthread_spinlock_t arc_slock; 
static ArcNode node_pool[CACHE_SIZE * 2]; 
static int free_node_head = -1;

static int hash_table[HASH_SIZE];
static int heads[5], tails[5], list_sizes[5];

static int arc_p = 0;
static _Thread_local uint64_t g_current_lba = 0;


static inline int hash_func(uint64_t lba) {
    return (int)((lba * 2654435761u) & HASH_MASK);
}

static inline int lookup_node_unsafe(uint64_t lba) {
    int h = hash_func(lba);
    for (int i = 0; i < 128; i++) {
        int idx = hash_table[h];
        if (idx == HASH_EMPTY) return -1;
        if (idx != HASH_DELETED && node_pool[idx].lba == lba) return idx;
        h = (h + 1) & HASH_MASK;
    }
    return -1;
}

static inline void list_remove_locked(int idx) {
    ArcListType type = node_pool[idx].type;
    if (type == LIST_NONE) return;

    int p = node_pool[idx].prev;
    int n = node_pool[idx].next;

    if (p != -1) node_pool[p].next = n;
    else heads[type] = n;

    if (n != -1) node_pool[n].prev = p;
    else tails[type] = p;

    list_sizes[type]--;
    node_pool[idx].type = LIST_NONE;
}

static inline void list_push_front_locked(int idx, ArcListType type) {
    node_pool[idx].type = type;
    node_pool[idx].next = heads[type];
    node_pool[idx].prev = -1;

    if (heads[type] != -1) node_pool[heads[type]].prev = idx;
    heads[type] = idx;
    if (tails[type] == -1) tails[type] = idx;

    list_sizes[type]++;
}


//REPLACE

static int REPLACE_locked(bool x_in_b2) {
    int victim_idx = -1;
    ArcListType dest_ghost;

    if ((list_sizes[LIST_T1] >= 1) && ((x_in_b2 && list_sizes[LIST_T1] == arc_p) || (list_sizes[LIST_T1] > arc_p))) {
        victim_idx = tails[LIST_T1];
        dest_ghost = LIST_B1;
    } else {
        victim_idx = tails[LIST_T2];
        dest_ghost = LIST_B2;
    }

    if (victim_idx == -1) return -1;

    // 슬롯 반환 전 해시 테이블에서 삭제 처리
    int h = hash_func(node_pool[victim_idx].lba);
    for (int i = 0; i < 128; i++) {
        if (hash_table[h] == victim_idx) {
            hash_table[h] = HASH_DELETED;
            break;
        }
        h = (h + 1) & HASH_MASK;
    }

    int slot = node_pool[victim_idx].phys_slot;
    list_remove_locked(victim_idx);
    node_pool[victim_idx].phys_slot = -1;
    list_push_front_locked(victim_idx, dest_ghost);

    // Ghost 상태로 해시에 재등록
    int nh = hash_func(node_pool[victim_idx].lba);
    while (hash_table[nh] >= 0) nh = (nh + 1) & HASH_MASK;
    hash_table[nh] = victim_idx;

    return slot;
}


static void arc_init(void) {
    pthread_spin_init(&arc_slock, PTHREAD_PROCESS_PRIVATE);
    for (int i = 0; i < CACHE_SIZE * 2; i++) {
        node_pool[i].next = (i < CACHE_SIZE * 2 - 1) ? i + 1 : -1;
        node_pool[i].type = LIST_NONE;
        node_pool[i].phys_slot = -1;
    }
    free_node_head = 0;
    memset(hash_table, -1, sizeof(int) * HASH_SIZE);
    memset(list_sizes, 0, sizeof(list_sizes));
    arc_p = 0;
    printf("[Policy] ARC Spin-Optimized (Fixed) Initialized.\n");
}

void arc_set_current_lba(uint64_t lba) {
    g_current_lba = lba;
}

static void arc_on_hit(int slot, uint64_t lba) {
    pthread_spin_lock(&arc_slock);
    int idx = lookup_node_unsafe(lba);
    if (idx != -1) {
        list_remove_locked(idx);
        list_push_front_locked(idx, LIST_T2);
    }
    pthread_spin_unlock(&arc_slock);
}

static int arc_select_victim(void) {
    pthread_spin_lock(&arc_slock);
    
    int idx = lookup_node_unsafe(g_current_lba);
    bool x_in_b2 = (idx != -1 && node_pool[idx].type == LIST_B2);
    
    static int init_ptr = 0;
    int chosen_slot = (init_ptr < CACHE_SIZE) ? init_ptr++ : REPLACE_locked(x_in_b2);
    
    pthread_spin_unlock(&arc_slock);
    return chosen_slot;
}

static void arc_on_miss(uint64_t lba, int phys_slot) {
    pthread_spin_lock(&arc_slock);
    int idx = lookup_node_unsafe(lba);

    if (idx != -1 && (node_pool[idx].type == LIST_B1 || node_pool[idx].type == LIST_B2)) {
        // Ghost Hits
        int b1 = list_sizes[LIST_B1], b2 = list_sizes[LIST_B2];
        if (node_pool[idx].type == LIST_B1) {
            int delta = (b1 >= b2) ? 1 : (b2 / (b1 > 0 ? b1 : 1));
            arc_p = (arc_p + delta > CACHE_SIZE) ? CACHE_SIZE : arc_p + delta;
        } else {
            int delta = (b2 >= b1) ? 1 : (b1 / (b2 > 0 ? b2 : 1));
            arc_p = (arc_p - delta < 0) ? 0 : arc_p - delta;
        }
        list_remove_locked(idx);
        node_pool[idx].phys_slot = phys_slot;
        list_push_front_locked(idx, LIST_T2);
    } 
    else if (idx == -1) {
        // Total Miss
        if (list_sizes[LIST_T1] + list_sizes[LIST_B1] == CACHE_SIZE) {
            int v = (list_sizes[LIST_B1] > 0) ? tails[LIST_B1] : tails[LIST_T1];
            if (v != -1) {
                int h = hash_func(node_pool[v].lba);
                for(int i=0; i<128; i++) { if(hash_table[h]==v){hash_table[h]=HASH_DELETED; break;} h=(h+1)&HASH_MASK; }
                list_remove_locked(v); // 인자 오류 수정됨
                node_pool[v].next = free_node_head; free_node_head = v;
            }
        } else {
            int total = list_sizes[LIST_T1] + list_sizes[LIST_T2] + list_sizes[LIST_B1] + list_sizes[LIST_B2];
            if (total >= CACHE_SIZE && total == 2 * CACHE_SIZE) {
                int v = tails[LIST_B2];
                if (v != -1) {
                    int h = hash_func(node_pool[v].lba);
                    for(int i=0; i<128; i++) { if(hash_table[h]==v){hash_table[h]=HASH_DELETED; break;} h=(h+1)&HASH_MASK; }
                    list_remove_locked(v); // 인자 오류 수정됨
                    node_pool[v].next = free_node_head; free_node_head = v;
                }
            }
        }

        if (free_node_head != -1) {
            int n = free_node_head; 
            free_node_head = node_pool[n].next;
            node_pool[n].lba = lba; 
            node_pool[n].phys_slot = phys_slot;
            int h = hash_func(lba); 
            while (hash_table[h] >= 0) h = (h + 1) & HASH_MASK;
            hash_table[h] = n;
            list_push_front_locked(n, LIST_T1);
        }
    }
    pthread_spin_unlock(&arc_slock);
}

static void arc_print_stats(void) {
    printf("[ARC] p=%d | T1=%d T2=%d B1=%d B2=%d\n", 
           arc_p, list_sizes[LIST_T1], list_sizes[LIST_T2],
           list_sizes[LIST_B1], list_sizes[LIST_B2]);
}

CachePolicy policy_arc = {
    .name = "ARC (Spinlock/Hash-Stable)",
    .init = arc_init,
    .on_hit = arc_on_hit,
    .on_miss = arc_on_miss,
    .select_victim = arc_select_victim,
    .print_stats = arc_print_stats,
    .set_lba = arc_set_current_lba
};