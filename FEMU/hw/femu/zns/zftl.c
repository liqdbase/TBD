#define POLLER_BUDGET 64

#include "zns.h"
#include "femu_shm_lib.h"
#include "hw/block/block.h"  
#include "system/dma.h"
//#define FEMU_DEBUG_ZFTL

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

static NvmeRequest *pending_reqs[65536];

static void *ftl_thread(void *arg);

static inline struct ppa get_maptbl_ent(struct zns_ssd *zns, uint64_t lpn)
{
    return zns->maptbl[lpn];
}

static inline void set_maptbl_ent(struct zns_ssd *zns, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < zns->l2p_sz);
    zns->maptbl[lpn] = *ppa;
}

static void check_completions(struct zns_ssd *zns)
{
    FemuSharedMemory *shm = zns->shm;
    ShmCpl cpl;
    int i;
    
    // 모든 I/O 큐를 순회 (Round-Robin)하여 특정 큐의 독점 방지
    for (i = 0; i < MAX_IO_QUEUES; i++) {
        
        // 큐당 최대 처리 개수 제한 (Budget)
        int queue_budget = POLLER_BUDGET;


        while (queue_budget > 0 && cq_dequeue(&shm->cq[i], &cpl)) {
            
            uint32_t tag = cpl.cmd_id;
            NvmeRequest *req = pending_reqs[tag]; 

            if (!req) {
                ftl_err("[Poller] Unknown or duplicate completion tag %u\n", tag);
                continue; 
            }

            uint8_t *cache_slot = (uint8_t*)shm + cpl.phys_addr;

            if (req->cmd.opcode == NVME_CMD_WRITE || req->cmd.opcode == NVME_CMD_ZONE_APPEND) {
                if (req->qsg.nsg > 0) {
                    dma_buf_read(cache_slot, LOGICAL_PAGE_SIZE, NULL, &req->qsg, MEMTXATTRS_UNSPECIFIED);
                } else if (req->iov.niov > 0) {
                    qemu_iovec_to_buf(&req->iov, 0, cache_slot, LOGICAL_PAGE_SIZE);
                }
            } 
            else if (req->cmd.opcode == NVME_CMD_READ) {
                if (req->qsg.nsg > 0) {
                    dma_buf_write(cache_slot, LOGICAL_PAGE_SIZE, NULL, &req->qsg, MEMTXATTRS_UNSPECIFIED);
                } else if (req->iov.niov > 0) {
                    qemu_iovec_from_buf(&req->iov, 0, cache_slot, LOGICAL_PAGE_SIZE);
                }
            }

            // Read/Write
            if (req->cmd.opcode == NVME_CMD_READ) {
                req->reqlat = SRAM_READ_LATENCY_NS;
            } else {
                req->reqlat = SRAM_WRITE_LATENCY_NS;
            }

            // Interrupt Time 갱신

            req->expire_time += req->reqlat;


            // 대기 목록에서 요청 제거 (중복 처리 방지)
            pending_reqs[tag] = NULL;

            femu_ring_enqueue(zns->to_poller[1], (void *)&req, 1);

            queue_budget--;
        }
    }
}

static FemuSharedMemory* zns_shm_init(void)
{
    int fd;
    FemuSharedMemory *shm;
    
    shm_unlink(SHM_NAME); 

    // 2. 새로 생성 (O_CREAT | O_RDWR)
    fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        ftl_err("shm_open failed: %s\n", strerror(errno));
        return NULL;
    }

    // 3. 크기 설정
    if (ftruncate(fd, sizeof(FemuSharedMemory)) == -1) {
        ftl_err("ftruncate failed: %s\n", strerror(errno));
        close(fd);
        shm_unlink(SHM_NAME);
        return NULL;
    }
    
    // 4. 메모리 매핑
    shm = (FemuSharedMemory *)mmap(NULL, sizeof(FemuSharedMemory),
                                    PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, 0);
    close(fd); 
    
    if (shm == MAP_FAILED) {
        ftl_err("mmap failed: %s\n", strerror(errno));
        return NULL;
    }
    
    // 5. 초기화
        memset(shm, 0, sizeof(FemuSharedMemory));

    // Atomic 초기화
    int i;
    for (i = 0; i < MAX_IO_QUEUES; i++) {
        atomic_init(&shm->sq[i].head, 0);
        atomic_init(&shm->sq[i].tail, 0);
        atomic_init(&shm->cq[i].head, 0);
        atomic_init(&shm->cq[i].tail, 0);
    }
    
    // Mapping Table 초기화 (-1: Empty)
    for (i = 0; i < MAX_LBA_MAP; i++) {
        atomic_init(&shm->mapping_table[i], -1);
    }
    
    //Cache Metadata 초기화 (추가!)
    for (i = 0; i < CACHE_SIZE; i++) {
        atomic_init(&shm->cache_metadata[i].lba, 0);
        atomic_init(&shm->cache_metadata[i].valid, 0);  // Invalid
        atomic_init(&shm->cache_metadata[i].timestamp, 0);
        atomic_init(&shm->cache_metadata[i].dirty, 0);
        atomic_init(&shm->cache_metadata[i].ref_count, 0);
    }
    
    // Zone 초기화
    for (i = 0; i < MAX_ZONES; i++) {
        atomic_init(&shm->zones[i].write_pointer, 0);
        atomic_init(&shm->zones[i].state, 1);  // EMPTY
        atomic_flag_clear(&shm->zones[i].lock);
    }
    
    // Stats 초기화 (추가!)
    atomic_init(&shm->stats.total_hits, 0);
    atomic_init(&shm->stats.total_misses, 0);
    atomic_init(&shm->stats.evictions, 0);
    atomic_init(&shm->stats.dirty_pages, 0);
    atomic_init(&shm->stats.flush_in_progress, 0);
    
    atomic_store(&shm->ready_magic, 0xDEADBEEF);
    ftl_log("Shared memory initialized with %d cache slots (1GB)\n", CACHE_SIZE);
    
    return shm;
}



void zftl_init(FemuCtrl *n)
{
    struct zns_ssd *ssd = n->zns;
    
    ssd->shm = zns_shm_init();
    if (!ssd->shm) {
        ftl_err("Failed to initialize shared memory\n");
        exit(1);
    }

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

static inline struct zns_ch *get_ch(struct zns_ssd *zns, struct ppa *ppa)
{
    return &(zns->ch[ppa->g.ch]);
}

static inline struct zns_fc *get_fc(struct zns_ssd *zns, struct ppa *ppa)
{
    struct zns_ch *ch = get_ch(zns, ppa);
    return &(ch->fc[ppa->g.fc]);
}

static inline struct zns_plane *get_plane(struct zns_ssd *zns, struct ppa *ppa)
{
    struct zns_fc *fc = get_fc(zns, ppa);
    return &(fc->plane[ppa->g.pl]);
}

static inline struct zns_blk *get_blk(struct zns_ssd *zns, struct ppa *ppa)
{
    struct zns_plane *pl = get_plane(zns, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline void check_addr(int a, int max)
{
   assert(a >= 0 && a < max);
}

static void zns_advance_write_pointer(struct zns_ssd *zns)
{
    struct write_pointer *wpp = &zns->wp;

    check_addr(wpp->ch, zns->num_ch);
    wpp->ch++;
    if (wpp->ch == zns->num_ch) {
        wpp->ch = 0;
        check_addr(wpp->lun, zns->num_lun);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == zns->num_lun) {
            wpp->lun = 0;
        }
    }
}

static uint64_t zns_advance_status(struct zns_ssd *zns, struct ppa *ppa,struct nand_cmd *ncmd)
{
    int c = ncmd->cmd;

    uint64_t nand_stime;
    uint64_t req_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;

    //plane level parallism
    struct zns_plane *pl = get_plane(zns, ppa);

    uint64_t lat = 0;
    int nand_type = get_blk(zns,ppa)->nand_type;

    uint64_t read_delay = zns->timing.pg_rd_lat[nand_type];
    uint64_t write_delay = zns->timing.pg_wr_lat[nand_type];
    uint64_t erase_delay = zns->timing.blk_er_lat[nand_type];

    switch (c) {
    case NAND_READ:
        nand_stime = (pl->next_plane_avail_time < req_stime) ? req_stime : \
                     pl->next_plane_avail_time;
        pl->next_plane_avail_time = nand_stime + read_delay;
        lat = pl->next_plane_avail_time - req_stime;
	    break;

    case NAND_WRITE:
	    nand_stime = (pl->next_plane_avail_time < req_stime) ? req_stime : \
		            pl->next_plane_avail_time;
	    pl->next_plane_avail_time = nand_stime + write_delay;
	    lat = pl->next_plane_avail_time - req_stime;
	    break;

    case NAND_ERASE:
        nand_stime = (pl->next_plane_avail_time < req_stime) ? req_stime : \
                        pl->next_plane_avail_time;
        pl->next_plane_avail_time = nand_stime + erase_delay;
        lat = pl->next_plane_avail_time - req_stime;
        break;

    default:
        /* To silent warnings */
        ;
    }

    return lat;
}

static inline bool valid_ppa(struct zns_ssd *zns, struct ppa *ppa)
{
    int ch = ppa->g.ch;
    int lun = ppa->g.fc;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sub_pg = ppa->g.spg;

    if (ch >= 0 && ch < zns->num_ch && lun >= 0 && lun < zns->num_lun && pl >=
        0 && pl < zns->num_plane && blk >= 0 && blk < zns->num_blk && pg>=0 && pg < zns->num_page && sub_pg >= 0 && sub_pg < ZNS_PAGE_SIZE/LOGICAL_PAGE_SIZE)
        return true;

    return false;
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static struct ppa get_new_page(struct zns_ssd *zns)
{
    struct write_pointer *wpp = &zns->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.fc = wpp->lun;
    ppa.g.blk = zns->active_zone;
    ppa.g.V = 1; //not padding page
    if(!valid_ppa(zns,&ppa))
    {
        ftl_err("[Misao] invalid ppa: ch %u lun %u pl %u blk %u pg %u subpg  %u \n",ppa.g.ch,ppa.g.fc,ppa.g.pl,ppa.g.blk,ppa.g.pg,ppa.g.spg);
        ppa.ppa = UNMAPPED_PPA;
    }
    return ppa;
}

static int zns_get_wcidx(struct zns_ssd* zns)
{
    int i;
    for(i = 0;i < zns->cache.num_wc;i++)
    {
        if(zns->cache.write_cache[i].sblk==zns->active_zone)
        {
            return i;
        }
    }
    return -1;
}

static void zns_invalidate_zone_cache(struct zns_ssd *zns, uint32_t zone_idx)
{
    int i;

    for (i = 0; i < zns->cache.num_wc; i++) {
        if (zns->cache.write_cache[i].sblk == zone_idx) {
            #ifdef FEMU_DEBUG_ZFTL
            uint64_t discarded_entries = zns->cache.write_cache[i].used;
            #endif

            zns->cache.write_cache[i].used = 0;
            zns->cache.write_cache[i].sblk = INVALID_SBLK;

            ftl_debug("Invalidated write_cache[%d] for zone %u (%lu entries discarded)\n",
                     i, zone_idx, discarded_entries);
            return;
        }
    }
}

static void zns_invalidate_zone_mappings(struct zns_ssd *zns, uint32_t zone_idx,
                                         uint64_t zone_size_lbas, uint64_t lbasz)
{
    FemuSharedMemory *shm = zns->shm;

    uint64_t zone_start_lba = zone_idx * zone_size_lbas;
    uint64_t zone_end_lba = (zone_idx + 1) * zone_size_lbas;
    uint64_t secs_per_pg = LOGICAL_PAGE_SIZE / lbasz;
    uint64_t start_lpn = zone_start_lba / secs_per_pg;
    uint64_t end_lpn = zone_end_lba / secs_per_pg;
    uint64_t lpn;
    uint64_t invalidated_count = 0;

    for (lpn = start_lpn; lpn < end_lpn && lpn < zns->l2p_sz; lpn++) {
        if (zns->maptbl[lpn].ppa != UNMAPPED_PPA) {
            zns->maptbl[lpn].ppa = UNMAPPED_PPA;
            invalidated_count++;
        }
        if (shm && lpn < MAX_LBA_MAP) {
            atomic_store_explicit(&shm->mapping_table[lpn], -1, 
                                 memory_order_release);
        }
    }

    ftl_debug("Invalidated %lu LPN mappings for zone %u (LPN %lu-%lu)\n",
             invalidated_count, zone_idx, start_lpn, end_lpn - 1);
}

static void zns_reset_block_state(struct zns_ssd *zns, uint32_t zone_idx)
{
    int ch, lun, pl;
    struct ppa ppa;
    struct zns_blk *blk;

    for (ch = 0; ch < zns->num_ch; ch++) {
        for (lun = 0; lun < zns->num_lun; lun++) {
            for (pl = 0; pl < zns->num_plane; pl++) {
                ppa.g.ch = ch;
                ppa.g.fc = lun;
                ppa.g.pl = pl;
                ppa.g.blk = zone_idx;
                ppa.g.pg = 0;
                ppa.g.spg = 0;

                blk = get_blk(zns, &ppa);
                blk->page_wp = 0;
            }
        }
    }

    ftl_debug("Reset block state for zone %u (all page_wp = 0)\n", zone_idx);
}

uint64_t zns_zone_reset(struct zns_ssd *zns, uint32_t zone_idx,
                        uint64_t zone_size_lbas, uint64_t lbasz, uint64_t stime)
{
    int ch, lun, pl;
    struct ppa ppa;
    struct nand_cmd erase_cmd;
    uint64_t sublat, maxlat = 0;
    uint64_t total_blocks_erased = 0;

    ftl_debug("=== Zone Reset Started for Zone %u ===\n", zone_idx);

    /* Step 1: Invalidate write cache (instant) */
    zns_invalidate_zone_cache(zns, zone_idx);

    /* Step 2: Invalidate L2P mappings (instant) */
    zns_invalidate_zone_mappings(zns, zone_idx, zone_size_lbas, lbasz);

    /* Step 3: Reset block state (instant) */
    zns_reset_block_state(zns, zone_idx);

    /* Step 4: Simulate physical erase across all channels/LUNs/planes (parallel) */
    erase_cmd.type = USER_IO;
    erase_cmd.cmd = NAND_ERASE;
    erase_cmd.stime = stime;

    for (ch = 0; ch < zns->num_ch; ch++) {
        for (lun = 0; lun < zns->num_lun; lun++) {
            for (pl = 0; pl < zns->num_plane; pl++) {
                ppa.ppa = 0;
                ppa.g.ch = ch;
                ppa.g.fc = lun;
                ppa.g.pl = pl;
                ppa.g.blk = zone_idx;
                ppa.g.pg = 0;
                ppa.g.spg = 0;

                sublat = zns_advance_status(zns, &ppa, &erase_cmd);
                maxlat = (sublat > maxlat) ? sublat : maxlat;
                total_blocks_erased++;
            }
        }
    }
    // Step 5 syncronized SHM
    if (zns->shm) {
        FemuSharedMemory *shm = zns->shm;
        
        atomic_store_explicit(&shm->zones[zone_idx].write_pointer, 0, 
                              memory_order_release);
        atomic_store_explicit(&shm->zones[zone_idx].state, 0, 
                              memory_order_release);
        
        ftl_debug("Shared memory zone %u reset (WP=0, state=EMPTY)\n", zone_idx);
    }


    ftl_debug("Zone %u reset complete: erased %lu blocks across %d ch * %d lun * %d planes\n",
             zone_idx, total_blocks_erased, (int)zns->num_ch, (int)zns->num_lun, (int)zns->num_plane);
    ftl_debug("Maximum erase latency: %lu ns (%.2f ms)\n", maxlat, maxlat / 1000000.0);
    ftl_debug("=== Zone Reset Finished ===\n\n");

    return maxlat;
}

static uint64_t zns_read(struct zns_ssd *zns, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    uint32_t nlb = req->nlb; 
    
    FemuSharedMemory *shm = zns->shm;
    int queue_id = req->sq->sqid % MAX_IO_QUEUES;
    uint32_t tag = req->cqe.cid;

    pending_reqs[tag] = req;
    
    ShmCmd cmd = {
        .cmd_id = tag,  
        .type = CMD_READ,
        .lba = lba,     
        .length = nlb,  
        .zone_id = 0    
    };

    int sq_retry = 1000000;
    while(!sq_enqueue(&shm->sq[queue_id], &cmd)) {
        sched_yield(); 
        if (--sq_retry == 0) {
            ftl_err("[R] SQ Full Timeout for LBA %lu\n", lba);
            pending_reqs[tag] = NULL;
            return 0; 
        }
    }

    return 0; 
}

static uint64_t zns_write(struct zns_ssd *zns, NvmeRequest *req)
{
    FemuSharedMemory *shm = zns->shm; 
    int queue_id = req->sq->sqid % MAX_IO_QUEUES;
    uint32_t tag = req->cqe.cid;

    pending_reqs[tag] = req;

    ShmCmd cmd = {
        .cmd_id = tag, 
        .type = CMD_WRITE,
        .lba = req->slba,
        .length = req->nlb, 
        .zone_id = zns->active_zone
    };

    int sq_retry = 1000000;
    while(!sq_enqueue(&shm->sq[queue_id], &cmd)) {
        sched_yield();
        if (--sq_retry == 0) {
            ftl_err("[W] SQ %d full timeout\n", queue_id);
            pending_reqs[tag] = NULL;
            return 0; 
        }
    }  
     
    return 0; 
}


static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct zns_ssd *zns = n->zns;
    NvmeRequest *req = NULL;
    int rc;
    int i;

    // 초기화 대기
    while (!*(zns->dataplane_started_ptr)) {
        usleep(100000);
    }

    zns->to_ftl = n->to_ftl;
    zns->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
            // 큐가 비었으면 패스
            if (!zns->to_ftl[i] || !femu_ring_count(zns->to_ftl[i]))
                continue;

            // 요청 꺼내기
            rc = femu_ring_dequeue(zns->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FEMU: FTL to_ftl dequeue failed\n");
                continue;
            }

            ftl_assert(req);
            
            bool is_async_command = false; // 비동기 명령 여부 플래그

            switch (req->cmd.opcode) {
                case NVME_CMD_ZONE_APPEND:
                     if (zns->shm) {
                        
                        uint32_t zone_id = req->slba / zns->zone_size_lbas;
                        FemuSharedMemory *shm = zns->shm;
                        
                        
                        uint64_t append_lba = atomic_fetch_add_explicit(
                            &shm->zones[zone_id].write_pointer, 
                            req->nlb, 
                            memory_order_acq_rel
                        );
                        
                        
                        req->slba = zone_id * zns->zone_size_lbas + append_lba;
                        
                        
                        req->cqe.n.result = (uint32_t)req->slba;

                        ftl_debug("[ZA] Zone %u append at LBA %lu (WP %lu)\n", 
                                zone_id, req->slba, append_lba);
                    }
                    
                    zns_write(zns, req); 
                    is_async_command = true; 
                    break;

                case NVME_CMD_WRITE:
                    zns_write(zns, req); 
                    is_async_command = true; 
                    break;

                case NVME_CMD_READ:
                    zns_read(zns, req);  
                    is_async_command = true; 
                    break;
                case NVME_CMD_FLUSH: {
                    if (!zns->shm) {
                        req->reqlat = 0;
                        is_async_command = false;
                        break;
                    }

                    FemuSharedMemory *shm = zns->shm;
                    int queue_id = req->sq->sqid % MAX_IO_QUEUES;
                    uint32_t tag = req->cqe.cid;

                    pending_reqs[tag] = req;

                    ShmCmd cmd = {
                        .cmd_id = tag,
                        .type = CMD_FLUSH,
                        .lba = 0,
                        .length = 0,
                        .zone_id = 0xFFFFFFFF,
                        .flags = CMD_FLAG_BARRIER
                    };

                    ftl_debug("[Flush] Sending Flush command to RTOS (tag=%u, queue=%d)\n", tag, queue_id);

                    int sq_retry = 1000000;

                    while (!sq_enqueue(&shm->sq[queue_id], &cmd)) {
                        sched_yield();
                        if (--sq_retry == 0) {
                            break; 
                        }
                    }

                    if (sq_retry > 0) {
                        is_async_command = true;
                    } else {
                        ftl_err("[Flush] SQ Full Timeout for Flush command\n");
                        pending_reqs[tag] = NULL;
                        req->reqlat = 0;
                        is_async_command = false;
                        ftl_log("[Flush] Fallback to immediate completion\n");
                    }
                    
                    break;
                }

                case NVME_CMD_DSM:
                    req->reqlat = 0;
                    is_async_command = false; // 동기 처리
                    break;

                default:
                    // 알 수 없는 명령은 즉시 에러 처리 혹은 0 레이턴시 완료
                    req->reqlat = 0;
                    is_async_command = false;
                    break;
            }

            if (is_async_command) {
                continue; 
            }

            // 동기 명령
            req->expire_time += req->reqlat;
            rc = femu_ring_enqueue(zns->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }
        }

        
        if (zns->shm) {
            check_completions(zns);
        }

    }

    return NULL;
}
