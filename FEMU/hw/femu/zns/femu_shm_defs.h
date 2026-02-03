#ifndef FEMU_SHM_DEFS_H
#define FEMU_SHM_DEFS_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdalign.h>

#define SHM_NAME "/femu_zns_shm"
#define MAX_IO_QUEUES 4
#define RING_SIZE 1024
#define MAX_LBA_MAP (1024 * 1024 * 10)  // 10M entries
#define MAX_ZONES 1024
#define PAGE_SIZE 4096
#define CACHE_SIZE 262144  // 1GB / 4KB slots

typedef enum {
    CMD_READ,
    CMD_WRITE,
    CMD_ZONE_APPEND,
    CMD_FLUSH,           // NVMe Flush
    CMD_PREFIL_EVICT
} CmdType;

typedef struct {
    uint32_t cmd_id;
    uint32_t type;
    uint64_t lba;
    uint32_t length;
    uint32_t zone_id;
    uint32_t flags;      // CMD_FLAG_*
} ShmCmd;

#define CMD_FLAG_FLUSH_HINT   (1 << 0)  // Flush 제안 (Non-blocking)
#define CMD_FLAG_BARRIER      (1 << 1)  // 강제 Flush (Blocking)
#define CMD_FLAG_FUA          (1 << 2)  // Force Unit Access

typedef struct {
    uint32_t cmd_id;
    int32_t status;
    uint64_t phys_addr;  // Data Pool Offset
} ShmCpl;

typedef struct {
    _Alignas(64) atomic_uint head;
    _Alignas(64) atomic_uint tail;
    ShmCmd cmds[RING_SIZE];
} SqRing;

typedef struct {
    _Alignas(64) atomic_uint head;
    _Alignas(64) atomic_uint tail;
    ShmCpl cpls[RING_SIZE];
} CqRing;

typedef struct {
    atomic_int write_pointer;
    atomic_int state;
    atomic_flag lock;
} SharedZoneMeta;

typedef struct {
    _Atomic uint64_t lba;        // Slot에 저장된 실제 LBA
    _Atomic uint32_t valid;      // 1=Valid, 0=Evicted
    _Atomic uint64_t timestamp;  // LRU/ARC용 시각
    _Atomic uint32_t dirty;      // Write 여부
    _Atomic uint32_t ref_count;  // Eviction 방지용
} CacheMetadata;

typedef struct {
    _Atomic uint64_t total_hits;
    _Atomic uint64_t total_misses;
    _Atomic uint64_t evictions;
    _Atomic uint64_t dirty_pages;
    _Atomic uint32_t flush_in_progress;
} CacheStats;

typedef struct {
    SqRing sq[MAX_IO_QUEUES];
    CqRing cq[MAX_IO_QUEUES];

    // Hash Index -> Slot Index (-1: Empty)
    atomic_int mapping_table[MAX_LBA_MAP];

    // Slot별 LBA 검증 및 메타데이터
    CacheMetadata cache_metadata[CACHE_SIZE];

    SharedZoneMeta zones[MAX_ZONES];
    CacheStats stats;

    // 1GB DRAM Cache
    uint8_t data_pool[CACHE_SIZE][PAGE_SIZE];

    atomic_uint ready_magic;
} FemuSharedMemory;

#endif // FEMU_SHM_DEFS_H