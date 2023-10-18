#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <stack>
#include <thread>
#include <uuid/uuid.h>
#include <vector>

#include "backend.h"
#include "config.h"
#include "extent.h"
#include "io.h"
#include "journal.h"
#include "lsvd_types.h"
#include "misc_cache.h"
#include "nvme.h"
#include "request.h"
#include "smartiov.h"
#include "translate.h"
#include "utils.h"

// all addresses are in units of 4KB blocks
class write_cache
{
  private:
    size_t dev_max;
    uint32_t super_blkno;
    lsvd_config *cfg;

    std::atomic<uint64_t> sequence = 1; // write sequence #

    // bookkeeping for write throttle
    std::mutex m2; // for total_write_pages
    int total_write_pages = 0;
    int max_write_pages = 0;
    int outstanding_writes = 0;
    size_t write_batch = 0;
    std::condition_variable write_cv;

    // initialization stuff
    int roll_log_forward();
    char *_hdrbuf; // for reading at startup

    thread_pool<int> *misc_threads;

    void write_checkpoint(void);

    // allocate journal entry, create a header
    uint32_t allocate(page_t n, page_t &pad, page_t &n_pad, page_t &prev);
    j_write_super *super;
    page_t previous_hdr = 0;
    page_t next_alloc = 0;

    // these are used by wcache_write_req
    friend class wcache_write_req;
    std::mutex m;
    translate *be;
    j_hdr *mk_header(char *buf, uint32_t type, page_t blks, page_t prev);
    std::unique_ptr<nvme> nvme_w;

  public:
    write_cache(uint32_t blkno, int _fd, translate *_be, lsvd_config *cfg);
    ~write_cache();
    static std::unique_ptr<write_cache>
    make_write_cache(uint32_t blkno, int fd, translate *be, lsvd_config *cfg);

    // throttle writes with window of max_write_pages
    void get_room(sector_t sectors);
    void release_room(sector_t sectors);
    void flush(void);

    std::unique_ptr<request> writev(sector_t lba, smartiov *iov);
    void do_write_checkpoint(void);
};
