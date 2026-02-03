/* Standard Includes */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>

/* FreeRTOS Includes */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* FEMU Shared Headers */
#include "femu_shm_lib.h"

#include "policy.h"

// -------------------------------------------------------------
// 캐시 설정
// -------------------------------------------------------------
#ifndef CACHE_SIZE
#define CACHE_SIZE 262144  // 1GB / 4KB = 262,144 slots
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#ifndef MAX_LBA_MAP
#define MAX_LBA_MAP 10485760  // 10M entries
#endif

#ifndef MAX_IO_QUEUES
#define MAX_IO_QUEUES 4
#endif

#ifndef SHM_NAME
#define SHM_NAME "/femu_zns_shm"
#endif

// Flush 설정
#define FLUSH_THRESHOLD_HIGH  85
#define FLUSH_THRESHOLD_LOW   50
#define FLUSH_INTERVAL_MS     10
#define MAX_PROBE_DEPTH       8

// Flush Retry 설정
#define FLUSH_ACQUIRE_RETRY   100
#define FLUSH_ACQUIRE_DELAY_US 1000

// 에러 코드
#define NVME_SC_SUCCESS       0x00
#define NVME_SC_INTERNAL      0x06

// -------------------------------------------------------------
// 전역 변수
// -------------------------------------------------------------
FemuSharedMemory *g_shm = NULL;
QueueHandle_t xLogQueue;

/* ★★★ [NEW] 현재 활성화된 정책 포인터 ★★★ */
CachePolicy *current_policy = NULL;

// -------------------------------------------------------------
// 로그 구조체
// -------------------------------------------------------------
typedef struct {
    long qid;
    int is_write;
    uint64_t lba;
    int cache_status;  // 0=Miss, 1=Hit, 2=Write, 3=Flush, 4=FlushFail
    int slot;
} LogMessage;

// -------------------------------------------------------------
// [Cache Management] LBA 검증 기반 Cache Lookup
// -------------------------------------------------------------

/**
 * 캐시에서 LBA 검색 (Linear Probing)
 * 
 * @return: 캐시 슬롯 번호 (Hit) 또는 -1 (Miss)
 * 
 * 변경 사항:
 * - ★ on_hit 호출 제거 (vWorkerTask에서 처리)
 * - Hit/Miss 통계는 여기서 업데이트 (정책 무관)
 */
static int cache_lookup_with_probing(uint64_t lba) {
    if (lba >= MAX_LBA_MAP) {
        return -1;
    }

    uint32_t hash = lba % MAX_LBA_MAP;

    for (uint32_t probe = 0; probe < MAX_PROBE_DEPTH; probe++) {
        uint32_t idx = (hash + probe) % MAX_LBA_MAP;
        int slot = atomic_load_explicit(&g_shm->mapping_table[idx], 
                                        memory_order_acquire);

        if (slot < 0) {
            atomic_fetch_add_explicit(&g_shm->stats.total_misses, 1, 
                                     memory_order_relaxed);
            return -1;  // Miss
        }

        uint64_t stored_lba = atomic_load_explicit(
            &g_shm->cache_metadata[slot].lba, memory_order_acquire);
        uint32_t valid = atomic_load_explicit(
            &g_shm->cache_metadata[slot].valid, memory_order_acquire);

        if (valid && stored_lba == lba) {
            // Hit 발견: timestamp 업데이트 (정책 무관 공통 작업)
            atomic_store_explicit(&g_shm->cache_metadata[slot].timestamp, 
                                 xTaskGetTickCount(), memory_order_release);

            atomic_fetch_add_explicit(&g_shm->stats.total_hits, 1, 
                                     memory_order_relaxed);
            return slot;  // Hit
        }
    }

    atomic_fetch_add_explicit(&g_shm->stats.total_misses, 1, 
                             memory_order_relaxed);
    return -1;  // Miss
}

/**
 * ★★★ [DELETED] cache_allocate_slot() 함수 삭제됨 ★★★
 * 
 * 이유: 하드코딩된 Round-Robin 로직을 policy_fifo.c로 이동
 * 대체: current_policy->select_victim() 호출
 */

/**
 * 캐시에 새 엔트리 삽입 (Linear Probing)
 * 
 * 동작:
 * 1. Mapping Table에서 LBA에 해당하는 슬롯 찾기
 * 2. 충돌 시 Linear Probing으로 빈 슬롯 탐색
 * 3. Metadata 업데이트 (LBA, valid, timestamp, dirty)
 * 
 * 정책 독립적: 어떤 정책이든 동일하게 동작
 */
static void cache_insert_with_probing(uint64_t lba, int new_slot) {
    uint32_t hash = lba % MAX_LBA_MAP;

    for (uint32_t probe = 0; probe < MAX_PROBE_DEPTH; probe++) {
        uint32_t idx = (hash + probe) % MAX_LBA_MAP;
        int old_slot = atomic_load_explicit(&g_shm->mapping_table[idx], 
                                            memory_order_acquire);

        if (old_slot < 0) {
            // 빈 슬롯 발견
            atomic_store_explicit(&g_shm->mapping_table[idx], new_slot, 
                                 memory_order_release);
            goto set_metadata;
        }

        uint64_t stored_lba = atomic_load_explicit(
            &g_shm->cache_metadata[old_slot].lba, memory_order_acquire);

        if (stored_lba == lba) {
            // 동일 LBA 존재: 기존 슬롯 무효화 후 새 슬롯 할당
            atomic_store_explicit(&g_shm->cache_metadata[old_slot].valid, 0, 
                                 memory_order_release);

            atomic_store_explicit(&g_shm->mapping_table[idx], new_slot, 
                                 memory_order_release);
            goto set_metadata;
        }
    }

    // Probing 실패: 강제 Eviction
    uint32_t evict_idx = hash;
    int evict_slot = atomic_load(&g_shm->mapping_table[evict_idx]);
    if (evict_slot >= 0) {
        atomic_store_explicit(&g_shm->cache_metadata[evict_slot].valid, 0, 
                             memory_order_release);
    }
    atomic_store_explicit(&g_shm->mapping_table[evict_idx], new_slot, 
                         memory_order_release);

set_metadata:
    // 새 슬롯 메타데이터 설정
    atomic_store_explicit(&g_shm->cache_metadata[new_slot].lba, lba, 
                         memory_order_release);
    atomic_store_explicit(&g_shm->cache_metadata[new_slot].valid, 1, 
                         memory_order_release);
    atomic_store_explicit(&g_shm->cache_metadata[new_slot].timestamp, 
                         xTaskGetTickCount(), memory_order_release);
    atomic_store_explicit(&g_shm->cache_metadata[new_slot].dirty, 0, 
                         memory_order_release);
}

// -------------------------------------------------------------
// [Flush 공통 로직]
// -------------------------------------------------------------

static int perform_flush_internal(const char *caller, unsigned long *out_flushed) {
    unsigned long flushed = 0;
    TickType_t start_tick = xTaskGetTickCount();

    for (int i = 0; i < CACHE_SIZE; i++) {
        uint32_t is_dirty = atomic_load_explicit(
            &g_shm->cache_metadata[i].dirty, memory_order_acquire);
        uint32_t is_valid = atomic_load_explicit(
            &g_shm->cache_metadata[i].valid, memory_order_acquire);

        if (is_dirty && is_valid) {
            atomic_store_explicit(&g_shm->cache_metadata[i].dirty, 0, 
                                 memory_order_release);
            flushed++;
        }
    }

    atomic_fetch_sub_explicit(&g_shm->stats.dirty_pages, flushed, 
                              memory_order_release);

    TickType_t elapsed = xTaskGetTickCount() - start_tick;

    printf("[%s] Flushed %lu pages in %lu ms\n", caller, flushed, elapsed);

    if (out_flushed) {
        *out_flushed = flushed;
    }

    return 0;
}

// -------------------------------------------------------------
// [Background Flush Task]
// -------------------------------------------------------------

void vFlushTask(void *pvParameters) {
    (void)pvParameters;

    printf("[FW] Background Flush Task Started (Check Interval: %dms)\n", 
           FLUSH_INTERVAL_MS);

    TickType_t xLastWakeTime = xTaskGetTickCount();
    unsigned long flush_count = 0;

    for (;;) {
        uint64_t dirty = atomic_load_explicit(&g_shm->stats.dirty_pages, 
                                              memory_order_acquire);
        uint64_t total_slots = CACHE_SIZE;

        uint32_t dirty_percent = (dirty * 100) / total_slots;

        bool should_flush = false;
        bool is_emergency = false;

        if (dirty_percent >= FLUSH_THRESHOLD_HIGH) {
            should_flush = true;
            is_emergency = true;
        } else if (dirty_percent >= FLUSH_THRESHOLD_LOW) {
            should_flush = true;
        }

        if (should_flush) {
            uint32_t expected = 0;
            if (!atomic_compare_exchange_strong_explicit(
                    &g_shm->stats.flush_in_progress, &expected, 1,
                    memory_order_acq_rel, memory_order_acquire)) {
                vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(FLUSH_INTERVAL_MS));
                continue;
            }

            printf("[BG Flush] Started (Dirty: %lu/%lu = %u%%)%s\n", 
                   dirty, total_slots, dirty_percent, 
                   is_emergency ? " [EMERGENCY]" : "");

            unsigned long flushed = 0;
            int result = perform_flush_internal("BG Flush", &flushed);

            atomic_store_explicit(&g_shm->stats.flush_in_progress, 0, 
                                 memory_order_release);

            if (result == 0) {
                printf("[BG Flush] Completed (Total BG Flush: %lu)\n", ++flush_count);
            } else {
                printf("[BG Flush] Failed!\n");
            }
        }

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(FLUSH_INTERVAL_MS));
    }
}

// -------------------------------------------------------------
// [Logger Task]
// -------------------------------------------------------------
void vLoggerTask(void *pvParameters) {
    (void)pvParameters;
    LogMessage logMsg;
    FILE *fp;
    unsigned long log_count = 0;

    printf("[FW] Logger Task Started... Writing to 'firmware.log'\n");

    fp = fopen("firmware.log", "w");
    if (fp == NULL) {
        printf("[FW] Failed to open log file: %s\n", strerror(errno));
        vTaskDelete(NULL);
        return;
    }

    fprintf(fp, "WorkerID, Type, LBA, CacheStatus, Slot\n");
    fflush(fp);

    for (;;) {
        if (xQueueReceive(xLogQueue, &logMsg, pdMS_TO_TICKS(1000)) == pdPASS) {

            const char *cache_str = "";
            if (logMsg.cache_status == 1) {
                cache_str = " [Cache HIT]";
            } else if (logMsg.cache_status == 0) {
                cache_str = " [Cache MISS]";
            } else if (logMsg.cache_status == 2) {
                cache_str = " [Cache WRITE]";
            } else if (logMsg.cache_status == 3) {
                cache_str = " [FLUSH OK]";
            } else if (logMsg.cache_status == 4) {
                cache_str = " [FLUSH FAIL]";
            }

            fprintf(fp, "[FW] Worker%ld: %s LBA %lu%s Slot=%d\n", 
                    logMsg.qid, 
                    (logMsg.is_write) ? "WRITE" : "READ", 
                    logMsg.lba,
                    cache_str,
                    logMsg.slot);

            log_count++;

            if (log_count % 1000000 == 0) {
                fflush(fp);
            }
        } else {
            fflush(fp);
        }
    }
}

// -------------------------------------------------------------
// [Statistics Task]
// -------------------------------------------------------------
void vStatsTask(void *pvParameters) {
    (void)pvParameters;

    printf("[FW] Statistics Task Started (10s interval)\n");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        unsigned long hits = atomic_load(&g_shm->stats.total_hits);
        unsigned long misses = atomic_load(&g_shm->stats.total_misses);
        unsigned long evictions = atomic_load(&g_shm->stats.evictions);
        unsigned long dirty = atomic_load(&g_shm->stats.dirty_pages);
        unsigned long total = hits + misses;

        if (total > 0) {
            double hit_rate = (hits * 100.0) / total;
            double dirty_percent = (dirty * 100.0) / CACHE_SIZE;

            printf("\n[STATS] Requests: %lu | Hits: %lu (%.2f%%) | Misses: %lu | Evictions: %lu | Dirty: %lu (%.1f%%)\n",
                   total, hits, hit_rate, misses, evictions, dirty, dirty_percent);
            
            /* ★★★ [NEW] 정책별 통계 출력 ★★★ */
            if (current_policy && current_policy->print_stats) {
                current_policy->print_stats();
            }
        }
    }
}

// [main.c] I/O 요청을 처리하는 핵심 Worker Task
void vWorkerTask(void *pvParameters) {
    long qid = (long)pvParameters;
    ShmCmd cmd;
    ShmCpl cpl;
    LogMessage logMsg;

    unsigned long worker_requests = 0;

    printf("[FW] Worker Task %ld started (Polling Queue %ld)\n", qid, qid);

    for (;;) {
        // 1. 전면 큐(Submission Queue)에서 명령 데큐
        if (sq_dequeue(&g_shm->sq[qid], &cmd)) {

            worker_requests++;

            // 로그 메시지 기본 설정
            logMsg.qid = qid;
            logMsg.lba = cmd.lba;
            logMsg.is_write = (cmd.type == CMD_WRITE || cmd.type == CMD_ZONE_APPEND);
            logMsg.cache_status = -1;
            logMsg.slot = -1;

            // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
            // [Common] 정책에 현재 처리 중인 LBA 정보 업데이트
            // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
            // ARC/ML-ARC의 경우 내부적으로 Ghost 캐시 체크 및 적응형 변수(p) 조절에 사용됨
            if (current_policy && current_policy->set_lba) {
                current_policy->set_lba(cmd.lba);
            }

            // =========================================================
            // [READ] 처리 로직
            // =========================================================
            if (cmd.type == CMD_READ) {
                int cache_slot = cache_lookup_with_probing(cmd.lba);

                if (cache_slot >= 0) {
                    // -------------------------------------------------
                    // [HIT] 캐시에 데이터가 있음
                    // -------------------------------------------------
                    if (current_policy && current_policy->on_hit) {
                        current_policy->on_hit(cache_slot, cmd.lba);
                    }

                    logMsg.cache_status = 1; // Hit
                    logMsg.slot = cache_slot;

                    cpl.cmd_id = cmd.cmd_id;
                    cpl.status = NVME_SC_SUCCESS;
                    cpl.phys_addr = (uint64_t)cache_slot * PAGE_SIZE;

                } else {
                    // 캐시에 데이터가 없음 (Eviction 발생 가능)
                    logMsg.cache_status = 0; // Miss

                    // 1. 희생자 선정 (물리 슬롯 확보)
                    int new_slot = -1;
                    if (current_policy && current_policy->select_victim) {
                        new_slot = current_policy->select_victim();
                    } else {
                        new_slot = 0; // Fallback
                    }

                    uint64_t old_lba = atomic_load(&g_shm->cache_metadata[new_slot].lba);
                    uint32_t victim_valid = atomic_load(&g_shm->cache_metadata[new_slot].valid);

                    if (victim_valid) {
                        uint32_t old_hash = old_lba % MAX_LBA_MAP;
                        for (uint32_t p = 0; p < MAX_PROBE_DEPTH; p++) {
                            uint32_t idx = (old_hash + p) % MAX_LBA_MAP;
                            if (atomic_load(&g_shm->mapping_table[idx]) == new_slot) {
                                atomic_store(&g_shm->mapping_table[idx], -1);
                                break;
                            }
                        }
                    }

                    if (current_policy && current_policy->on_miss) {
                        current_policy->on_miss(cmd.lba, new_slot);
                    }

                    uint32_t was_valid = atomic_exchange_explicit(
                        &g_shm->cache_metadata[new_slot].valid, 0, memory_order_acq_rel);

                    if (was_valid) {
                        atomic_fetch_add_explicit(&g_shm->stats.evictions, 1, memory_order_relaxed);
                        uint32_t was_dirty = atomic_exchange_explicit(
                            &g_shm->cache_metadata[new_slot].dirty, 0, memory_order_acq_rel);
                        if (was_dirty) {
                            atomic_fetch_sub_explicit(&g_shm->stats.dirty_pages, 1, memory_order_relaxed);
                        }
                    }

                    cache_insert_with_probing(cmd.lba, new_slot);
                    
                    logMsg.slot = new_slot;
                    cpl.cmd_id = cmd.cmd_id;
                    cpl.status = NVME_SC_SUCCESS;
                    cpl.phys_addr = (uint64_t)new_slot * PAGE_SIZE;
                }

            } else if (cmd.type == CMD_WRITE || cmd.type == CMD_ZONE_APPEND) {
                int cache_slot = cache_lookup_with_probing(cmd.lba);

                if (cache_slot < 0) {
                    // [Write Miss] 쓰기 시에도 Victim 선정 필요
                    if (current_policy && current_policy->select_victim) {
                        cache_slot = current_policy->select_victim();
                    } else {
                        cache_slot = 0;
                    }

                    uint64_t old_lba = atomic_load(&g_shm->cache_metadata[cache_slot].lba);
                    if (atomic_load(&g_shm->cache_metadata[cache_slot].valid)) {
                        uint32_t old_hash = old_lba % MAX_LBA_MAP;
                        for (uint32_t p = 0; p < MAX_PROBE_DEPTH; p++) {
                            uint32_t idx = (old_hash + p) % MAX_LBA_MAP;
                            if (atomic_load(&g_shm->mapping_table[idx]) == cache_slot) {
                                atomic_store(&g_shm->mapping_table[idx], -1);
                                break;
                            }
                        }
                        atomic_fetch_add_explicit(&g_shm->stats.evictions, 1, memory_order_relaxed);
                    }

                    if (current_policy && current_policy->on_miss) {
                        current_policy->on_miss(cmd.lba, cache_slot);
                    }
                    
                    cache_insert_with_probing(cmd.lba, cache_slot);
                } else {

                    if (current_policy && current_policy->on_hit) {
                        current_policy->on_hit(cache_slot, cmd.lba);
                    }
                }


                if (!atomic_exchange_explicit(&g_shm->cache_metadata[cache_slot].dirty, 1, memory_order_acq_rel)) {
                    atomic_fetch_add_explicit(&g_shm->stats.dirty_pages, 1, memory_order_relaxed);
                }

                logMsg.cache_status = 2; 
                logMsg.slot = cache_slot;
                cpl.cmd_id = cmd.cmd_id;
                cpl.status = NVME_SC_SUCCESS;
                cpl.phys_addr = (uint64_t)cache_slot * PAGE_SIZE;


            } else if (cmd.type == CMD_FLUSH) {
                cpl.cmd_id = cmd.cmd_id;
                cpl.status = NVME_SC_SUCCESS;
                cpl.phys_addr = 0;
            }

            // 완료 통지 및 로그 전송
            xQueueSend(xLogQueue, &logMsg, 0);

            // 완료 큐(Completion Queue)에 응답 삽입
            while (!cq_enqueue(&g_shm->cq[qid], &cpl)) {
                taskYIELD();
            }

            // 주기적 통계 출력 (50만회당 1회)
            if (worker_requests % 500000 == 0) {
                unsigned long hits = atomic_load(&g_shm->stats.total_hits);
                unsigned long misses = atomic_load(&g_shm->stats.total_misses);
                if (hits + misses > 0) {
                    printf("[Worker%ld] Processed: %lu | Hit Rate: %.2f%%\n",
                           qid, worker_requests, (hits * 100.0) / (hits + misses));
                }
            }

        } else {
            // 처리할 명령이 없으면 제어권 양보
            taskYIELD();
        }
    }
}


void init_shm_connection(void) {
    int fd;

    printf("[FW] Waiting for FEMU Shared Memory (%s)...\n", SHM_NAME);

    while (1) {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd != -1) {
            struct stat sb;
            if (fstat(fd, &sb) == 0 && sb.st_size > 0) {
                break; 
            }
            close(fd); 
        }
        usleep(100000);
    }

    g_shm = (FemuSharedMemory *)mmap(NULL, sizeof(FemuSharedMemory),
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd, 0);
    close(fd); 

    if (g_shm == MAP_FAILED) {
        printf("[FW] MMAP Failed: %s\n", strerror(errno));
        exit(1);
    }

    printf("[FW] Waiting for FEMU initialization...\n");
    while (atomic_load(&g_shm->ready_magic) != 0xDEADBEEF) {
        usleep(10000);
    }

    atomic_store(&g_shm->ready_magic, 0xCAFEBABE);
    printf("[FW] Shared Memory Connected & Ready!\n");
    printf("[FW]   Size: %lu MB\n", sizeof(FemuSharedMemory) / (1024*1024));
    printf("[FW]   Cache Slots: %d (1GB)\n", CACHE_SIZE);
    printf("[FW]   Mapping Table: %d entries (%.1f MB)\n", 
           MAX_LBA_MAP, (MAX_LBA_MAP * 4) / (1024.0 * 1024.0));
}

void vApplicationIdleHook(void) {
    usleep(1000);
}


int main(void) {
    printf("========================================\n");
    printf("  FEMU-RTOS Firmware (Modular)\n");
    printf("  - Strategy Pattern Cache Policy\n");
    printf("  - Pluggable FIFO/ML-ARC\n");
    printf("========================================\n");


    char *policy_name = getenv("CACHE_POLICY");
    if (policy_name == NULL || strcmp(policy_name, "FIFO") == 0) {
        current_policy = &policy_fifo;
    } 
    else if (strcmp(policy_name, "ML_ARC") == 0) {
         current_policy = &policy_ml_arc;
    }
    else if (strcmp(policy_name, "ARC") == 0) {
         current_policy = &policy_arc;
    }
    else {
        printf("[FW] Unknown policy '%s', defaulting to FIFO\n", 
               policy_name ? policy_name : "NULL");
        current_policy = &policy_fifo;
    }

    printf("[FW] Selected Cache Policy: %s\n", current_policy->name);


    if (current_policy && current_policy->init) {
        current_policy->init();
    } else {
        printf("[FW] WARNING: Policy has no init function\n");
    }
    
    init_shm_connection();

    xLogQueue = xQueueCreate(4096, sizeof(LogMessage));
    if (xLogQueue == NULL) {
        printf("[FW] Failed to create Log Queue!\n");
        return 1;
    }

    for (long i = 0; i < MAX_IO_QUEUES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "Worker%ld", i);

        BaseType_t ret = xTaskCreate(
            vWorkerTask,
            name,
            configMINIMAL_STACK_SIZE * 4,
            (void *)i,
            3,
            NULL
        );

        if (ret != pdPASS) {
            printf("[FW] Failed to create Worker Task %ld\n", i);
            return 1;
        }
    }

    if (xTaskCreate(vFlushTask, "FlushBG", configMINIMAL_STACK_SIZE * 2, 
                    NULL, 2, NULL) != pdPASS) {
        printf("[FW] Failed to create Background Flush Task\n");
        return 1;
    }

    if (xTaskCreate(vLoggerTask, "Logger", configMINIMAL_STACK_SIZE * 4, 
                    NULL, 2, NULL) != pdPASS) {
        printf("[FW] Failed to create Logger Task\n");
        return 1;
    }

    if (xTaskCreate(vStatsTask, "Stats", configMINIMAL_STACK_SIZE * 2, 
                    NULL, 1, NULL) != pdPASS) {
        printf("[FW] Failed to create Stats Task\n");
        return 1;
    }

    printf("[FW] All tasks created successfully!\n");
    printf("[FW]   - %d Worker Tasks (I/O Processing)\n", MAX_IO_QUEUES);
    printf("[FW]   - 1 Background Flush Task\n");
    printf("[FW]   - 1 Logger Task\n");
    printf("[FW]   - 1 Statistics Task\n");
    printf("[FW] Starting FreeRTOS Scheduler...\n");
    printf("========================================\n\n");

    vTaskStartScheduler();

    printf("[FW] ERROR: Scheduler returned!\n");
    return 0;
}
