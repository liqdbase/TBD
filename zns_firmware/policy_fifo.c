/* policy_fifo.c */
#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>
#include "policy.h"

#ifndef CACHE_SIZE
#define CACHE_SIZE 262144
#endif

static atomic_int next_slot = 0;
// set_lba
static _Thread_local uint64_t g_fifo_current_lba = 0;

static void fifo_init(void) {
    atomic_store(&next_slot, 0);
    printf("[Policy] FIFO Initialized.\n");
}

static void fifo_on_hit(int slot, uint64_t lba) {
    (void)slot; (void)lba;
}

static void fifo_on_miss(uint64_t lba, int phys_slot) {
    (void)lba;
    (void)phys_slot; 
}

static int fifo_select_victim(void) {
    // 원자적 연산을 통한 슬롯 순환 할당
    int slot = atomic_fetch_add(&next_slot, 1) % CACHE_SIZE;
    return slot;
}

static void fifo_set_lba(uint64_t lba) {
    g_fifo_current_lba = lba;
}

static void fifo_print_stats(void) {
    printf("[FIFO] Next Victim Slot Suggestion: %d\n", atomic_load(&next_slot) % CACHE_SIZE);
}


CachePolicy policy_fifo = {
    .name = "FIFO",
    .init = fifo_init,
    .on_hit = fifo_on_hit,
    .on_miss = fifo_on_miss,
    .select_victim = fifo_select_victim,
    .print_stats = fifo_print_stats,
    .set_lba = fifo_set_lba 
};