#include <libaio.h>
#include <uuid/uuid.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <atomic>
#include <map>
#include <condition_variable>
#include <thread>
#include <stack>
#include <queue>
#include <cassert>
#include <shared_mutex>
#include <sys/stat.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>

#include "base_functions.h"

#include "journal2.h"
#include "smartiov.h"
#include "objects.h"
#include "extent.h"
#include "misc_cache.h"
#include "backend.h"
#include "translate.h"
#include "io.h"
#include "request.h"
#include "nvme.h"
#include "nvme_request.h"
#include "send_write_request.h"
#include "write_cache.h"

extern uuid_t my_uuid;
    int write_cache::pages_free(uint32_t oldest) {
        auto size = super->limit - super->base;
        auto tail = (super->next >= oldest) ? oldest + size : oldest;
        return tail - super->next - 1;
    }
    
    uint32_t write_cache::allocate_locked(page_t n, page_t &pad,
                             std::unique_lock<std::mutex> &lk) {
        auto timeout = std::chrono::seconds(1);
        while (pages_free(super->oldest) < 2*n) {
            alloc_cv.wait_for(lk, timeout);
            if (pages_free(super->oldest) < 2*n)
                printf("\nwaiting(1): %d %d %d\n", pages_free(super->oldest), super->next, super->oldest);
        }
        
        pad = 0;
        if (super->limit - super->next < (uint32_t)n) {
            pad = super->next;
            super->next = super->base;
        }
        auto val = super->next;
        super->next += n;
        if (super->next == super->limit)
            super->next = super->base;
        return val;
    }

    j_hdr *write_cache::mk_header(char *buf, uint32_t type, uuid_t &uuid, page_t blks) {
        memset(buf, 0, 4096);
        j_hdr *h = (j_hdr*)buf;
        *h = (j_hdr){.magic = LSVD_MAGIC, .type = type, .version = 1, .vol_uuid = {0},
                     .seq = super->seq++, .len = (uint32_t)blks, .crc32 = 0,
                     .extent_offset = 0, .extent_len = 0};
        memcpy(h->vol_uuid, uuid, sizeof(uuid));
        return h;
    }

void write_cache::append_seq(void) {
  super->seq++;
}
    /* min free is min(5%, 100MB). Free space:
     *  N = limit - base
     *  oldest == newest : free = N-1
     *  else : free = ((oldest + N) - newest - 1) % N
     */

    void write_cache::evict(void) {
        assert(!m.try_lock());  // m must belocked
        auto oldest = super->oldest;
        int pgs_free = pages_free(oldest);
        assert(pgs_free <= evict_trigger);

        std::vector<j_extent> to_delete;
        while (pages_free(oldest) < evict_trigger*3) {
            /*
             * for each record we trim from the cache, collect the vLBA->pLBA
             * mappings that we need to remove. Note that we keep the revers map
             * up to date, so we don't need to check them against the forward map.
             */
            auto len = lengths[oldest];
            assert(len > 0);
            lengths.erase(oldest);
            sector_t base = (oldest+1)*8, limit = base + (len-1)*8;
            for (auto it = rmap.lookup(base); it != rmap.end() && it->base() < limit; it++) {
                auto [p_base, p_limit, vlba] = it->vals(base, limit);
                to_delete.push_back((j_extent){(uint64_t)vlba, (uint64_t)p_limit-p_base});
            }
            rmap.trim(base, limit);
            oldest += len;
            if (oldest >= super->limit)
                oldest = super->base;
        }

        /* TODO: unlock, read the data and add to read cache, re-lock
         */
        super->oldest = oldest;         
        for (auto e : to_delete)
            map.trim(e.lba, e.lba + e.len);
    }

    void write_cache::ckpt_thread(thread_pool<int> *p) {
	auto next0 = super->next, N = super->limit - super->base;
	auto period = std::chrono::milliseconds(100);
	auto t0 = std::chrono::system_clock::now();
	auto timeout = std::chrono::seconds(5);
	const int ckpt_interval = N / 4;

	while (p->running) {
	    std::unique_lock lk(m);
	    p->cv.wait_for(lk, period);
	    if (!p->running)
		return;
	    auto t = std::chrono::system_clock::now();
	    bool do_ckpt = (int)((super->next + N - next0) % N) > ckpt_interval ||
		((t - t0 > timeout) && map_dirty);
	    if (p->running && do_ckpt) {
		next0 = super->next;
		t0 = t;
		lk.unlock();
		write_checkpoint();
	    }
	}
    }

    void write_cache::write_checkpoint(void) {
        std::unique_lock<std::mutex> lk(m);
        if (ckpt_in_progress)
            return;
        ckpt_in_progress = true;

        size_t map_bytes = map.size() * sizeof(j_map_extent);
        size_t len_bytes = lengths.size() * sizeof(j_length);
        page_t map_pages = div_round_up(map_bytes, 4096),
            len_pages = div_round_up(len_bytes, 4096),
            ckpt_pages = map_pages + len_pages;

        /* TODO - switch between top/bottom of metadata region so we
         * don't leave in inconsistent state if we crash during checkpoint
         */
        page_t blockno = super->meta_base;

        char *e_buf = (char*)aligned_alloc(512, 4096L*map_pages);
        auto jme = (j_map_extent*)e_buf;
        int n_extents = 0;
        if (map.size() > 0)
            for (auto it = map.begin(); it != map.end(); it++)
                jme[n_extents++] = (j_map_extent){(uint64_t)it->s.base,
                                                  (uint64_t)it->s.len, (uint32_t)it->s.ptr};
        // valgrind:
        int pad1 = 4096L*map_pages - map_bytes;
        assert(map_bytes == n_extents * sizeof(j_map_extent));
        assert(pad1 + map_bytes == 4096UL*map_pages);
        if (pad1 > 0)
            memset(e_buf + map_bytes, 0, pad1);

        char *l_buf = (char*)aligned_alloc(512, 4096L * len_pages);
        auto jl = (j_length*)l_buf;
        int n_lens = 0;
        for (auto it = lengths.begin(); it != lengths.end(); it++)
            jl[n_lens++] = (j_length){it->first, it->second};

        // valgrind:
        int pad2 = 4096L * len_pages - len_bytes;
        assert(len_bytes + pad2 == len_pages*4096UL);
        if (pad2 > 0)
            memset(l_buf + len_bytes, 0, pad2);

        j_write_super *super_copy = (j_write_super*)aligned_alloc(512, 4096);
        memcpy(super_copy, super, 4096);

        super_copy->map_start = super->map_start = blockno;
        super_copy->map_blocks = super->map_blocks = map_pages;
        super_copy->map_entries = super->map_entries = n_extents;

        super_copy->len_start = super->len_start = blockno+map_pages;
        super_copy->len_blocks = super->len_blocks = len_pages;
        super_copy->len_entries = super->len_entries = n_lens;

        lk.unlock();

        assert(4096UL*blockno + 4096UL*ckpt_pages <= dev_max);
        iovec iov[] = {{e_buf, map_pages*4096UL},
                       {l_buf, len_pages*4096UL}};

        if (pwritev(fd, iov, 2, 4096L*blockno) < 0)
            throw_fs_error("wckpt_e");
        if (pwrite(fd, (char*)super_copy, 4096, 4096L*super_blkno) < 0)
            throw_fs_error("wckpt_s");

        free(super_copy);
        free(e_buf);
        free(l_buf);

        map_dirty = false;
        ckpt_in_progress = false;
    }

    write_cache::write_cache(uint32_t blkno, int _fd, translate *_be, int n_threads) {

        super_blkno = blkno;
        fd = _fd;
        dev_max = getsize64(fd);
        be = _be;
        char *buf = (char*)aligned_alloc(512, 4096);
        if (pread(fd, buf, 4096, 4096L*blkno) < 4096)
            throw_fs_error("wcache");
        super = (j_write_super*)buf;
        pad_page = (char*)aligned_alloc(512, 4096);
        memset(pad_page, 0, 4096);
        map_dirty = false;

        if (super->map_entries) {
            size_t map_bytes = super->map_entries * sizeof(j_map_extent),
                map_bytes_rounded = round_up(map_bytes, 4096);
            char *map_buf = (char*)aligned_alloc(512, map_bytes_rounded);
            std::vector<j_map_extent> extents;
            if (pread(fd, map_buf, map_bytes_rounded, 4096L * super->map_start) < 0)
                throw_fs_error("wcache_map");
            decode_offset_len<j_map_extent>(map_buf, 0, map_bytes, extents);
            for (auto e : extents) {
                map.update(e.lba, e.lba+e.len, e.plba);
                rmap.update(e.plba, e.plba + e.len, e.lba);
            }
            free(map_buf);

            size_t len_bytes = super->len_entries * sizeof(j_length),
                len_bytes_rounded = round_up(len_bytes, 4096);
            char *len_buf = (char*)aligned_alloc(512, len_bytes_rounded);
            std::vector<j_length> _lengths;
            if (pread(fd, len_buf, len_bytes_rounded, 4096L * super->len_start) < 0)
                throw_fs_error("wcache_len");
            decode_offset_len<j_length>(len_buf, 0, len_bytes, _lengths);
            for (auto l : _lengths) {
                lengths[l.page] = l.len;
                assert(lengths[l.page] > 0);
                //xprintf("init: lens[%d] = %d\n", l.page, l.len);
            }
            free(len_buf);
        }
        /* TODO TODO TODO - need to roll log forward
         */
        auto N = super->limit - super->base;
        if (super->oldest == super->next)
            nfree = N - 1;
        else
            nfree = (super->oldest + N - super->next) % N;

        int evict_min_pct = 5;
        int evict_max_mb = 100;
        evict_trigger = std::min((evict_min_pct * (int)(super->limit - super->base) / 100),
                                 evict_max_mb * (1024*1024/4096));

        // https://stackoverflow.com/questions/22657770/using-c-11-multithreading-on-non-static-member-function

        misc_threads = new thread_pool<int>(&m);
        misc_threads->pool.push(std::thread(&write_cache::ckpt_thread, this, misc_threads));

        //e_io_running = true;
        //io_queue_init(64, &ioctx);
        const char *name = "write_cache_cb";
	//   e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name);
	
      
      
      nvme_w = new nvme(fd, name);
    }

    write_cache::~write_cache() {
    #if 0
        printf("wc map: %d %d (%ld)\n", map.size(), map.capacity(), sizeof(extmap::lba2lba));
        printf("wc rmap: %d %d (%ld)\n", rmap.size(), rmap.capacity(), sizeof(extmap::lba2lba));
    #endif

        delete misc_threads;
	
        free(pad_page);
        free(super);

        delete nvme_w;
	
	//        e_io_running = false;
        //e_io_th.join();
        //io_queue_release(ioctx);
    }


    void write_cache::send_writes(void) {
        std::unique_lock lk(m);
        writes_outstanding++;
        auto w = new std::vector<cache_work*>(std::make_move_iterator(work.begin()),
                                              std::make_move_iterator(work.end()));
        work.erase(work.begin(), work.end());

        sector_t sectors = 0;
        for (auto _w : *w) {
            sectors += _w->sectors;
            assert(_w->iovs.aligned(512));
        }
	
        page_t blocks = div_round_up(sectors, 8);
        
        page_t pad;
        page_t blockno = allocate_locked(blocks+1, pad, lk);
	if (pad) {
	  lengths[pad] = super->limit - pad;
	  assert((pad+1)*4096UL <= dev_max);
	  append_seq();
	}
	
	lk.unlock();
	
        lengths[blockno] = blocks+1;
	send_write_request *X = new send_write_request(w, blocks, blockno, pad, this); 
      	X->run(NULL);
    }

    void write_cache::writev(size_t offset, const iovec *iov, int iovcnt, void (*cb)(void*), void *ptr) {
        auto w = new cache_work(offset/512L, iov, iovcnt, cb, ptr);
        std::unique_lock lk(m);

        while (pages_free(super->oldest) <= evict_trigger) {
            if (evicting)
                alloc_cv.wait(lk);
            else {
                evicting = true;
                evict();
                evicting = false;
            }
        }
        work.push_back(w);
        if (writes_outstanding < 4 || work.size() >= 8) {
            lk.unlock();
            send_writes();
        }
    }

    /* returns (number of bytes skipped), (number of bytes read_started)
     */
    std::pair<size_t,size_t> write_cache::async_read(size_t offset, char *buf, size_t bytes,
                                        void (*cb)(void*), void *ptr) {
        sector_t base = offset/512, limit = base + bytes/512;
        std::unique_lock<std::mutex> lk(m);
        off_t nvme_offset = 0;
        size_t skip_len = 0, read_len = 0;

        auto it = map.lookup(base);
        if (it == map.end() || it->base() >= limit)
            skip_len = bytes;
        else {
            auto [_base, _limit, plba] = it->vals(base, limit);
            if (_base > base) {
                skip_len = 512 * (_base - base);
                buf += skip_len;
            }
            read_len = 512 * (_limit - _base),
                nvme_offset = 512L * plba;
        }
        lk.unlock();
        if (read_len) {
	    /*auto one_iovs = new smartiov();
	    one_iovs->push_back((iovec){buf, read_len});
	      auto closure = wrap([one_iovs]{
                    delete one_iovs;
                    return true;
                });
		nvme_w->make_read_request(one_iovs, nvme_offset
*/
	    auto eio = new e_iocb;
            e_io_prep_pread(eio, fd, buf, read_len, nvme_offset, cb, ptr);
            e_io_submit(nvme_w->ioctx, eio);
        }
        return std::make_pair(skip_len, read_len);
    }

    // debugging
    void write_cache::getmap(int base, int limit, int (*cb)(void*, int, int, int), void *ptr) {
        for (auto it = map.lookup(base); it != map.end() && it->base() < limit; it++) {
            auto [_base, _limit, plba] = it->vals(base, limit);
            if (!cb(ptr, (int)_base, (int)_limit, (int)plba))
                break;
        }
    }
    void write_cache::reset(void) {
        map.reset();
    }
    void write_cache::get_super(j_write_super *s) {
        *s = *super;
    }
    /* debug:
     * cache eviction - get info from oldest entry in cache. [should be private]
     *  @blk - header to read
     *  @extents - data to move. empty if J_PAD or J_CKPT
     *  return value - first page of next record
     */

    page_t write_cache::get_oldest(page_t blk, std::vector<j_extent> &extents) {
        char *buf = (char*)aligned_alloc(512, 4096);
        j_hdr *h = (j_hdr*)buf;

        if (pread(fd, buf, 4096, blk*4096) < 0)
            throw_fs_error("wcache");
        if (h->magic != LSVD_MAGIC) {
            printf("bad block: %d\n", blk);
        }
        assert(h->magic == LSVD_MAGIC && h->version == 1);

        auto next_blk = blk + h->len;
        if (next_blk >= super->limit)
            next_blk = super->base;
        if (h->type == LSVD_J_DATA)
            decode_offset_len<j_extent>(buf, h->extent_offset, h->extent_len, extents);

	free(buf);
        return next_blk;
    }
    
    void write_cache::do_write_checkpoint(void) {
        if (map_dirty)
            write_checkpoint();
    }

bool aligned(const void *ptr, int a)
    {
         return 0 == ((long)ptr & (a-1));
    }

