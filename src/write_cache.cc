#include <atomic>
#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <uuid/uuid.h>
#include <vector>

#include "config.h"
#include "journal.h"
#include "lsvd_types.h"
#include "misc_cache.h"
#include "nvme.h"
#include "request.h"
#include "smartiov.h"
#include "translate.h"
#include "utils.h"
#include "write_cache.h"

const usize SUPER_BLOCKNO = 1;

/* ------------- Write cache structure ------------- */

class wcache_write_req;
class write_cache_impl : public write_cache
{
    size_t dev_max;
    int fd = -1;

    std::atomic<uint64_t> sequence = 1; // write sequence #

    /* bookkeeping for write throttle
     */
    std::mutex m2; // for total_write_pages
    int total_write_pages = 0;
    int max_write_pages = 0;
    int outstanding_writes = 0;
    size_t write_batch = 0;
    std::condition_variable write_cv;

    // initialization stuff
    int roll_log_forward();
    void write_checkpoint(void);

    // allocate journal entry, create a header
    uint32_t allocate(page_t n, page_t &pad, page_t &n_pad, page_t &prev);
    j_write_super *super;
    page_t previous_hdr = 0;
    page_t next_alloc = 0;

    // these are used by wcache_write_req
    friend class wcache_write_req;
    std::mutex m;
    translate &be;
    j_hdr *mk_header(char *buf, uint32_t type, page_t blks, page_t prev);
    nvme *nvme_w = NULL;

  public:
    // throttle writes with window of max_write_pages
    void reserve_room(sector_t sectors);
    void release_room(sector_t sectors);
    void flush(void);

    write_cache_impl(int fd, translate &_be, lsvd_config &cfg);
    ~write_cache_impl();

    request *writev(sector_t lba, smartiov *iov);

    void do_write_checkpoint(void);
};

/* ------------- batched write request ------------- */

class wcache_write_req : public request
{
    sector_t plba;

    request *r_data = NULL;
    char *hdr = NULL;
    smartiov *data_iovs;

    request *r_pad = NULL;
    char *pad_hdr = NULL;
    smartiov pad_iov;

    std::atomic<int> reqs = 0; // 2 if r_pad else 1

    write_cache_impl *wcache = NULL;

    request *parent;

  public:
    uint64_t seq;

    wcache_write_req(sector_t lba, smartiov *iovs, page_t n_pages, page_t page,
                     page_t n_pad, page_t pad, page_t prev,
                     write_cache *wcache);
    ~wcache_write_req();
    void run(request *parent);
    void notify(request *child);

    void wait() {}
    void release() {} // TODO: free properly
};

static char *z_page;
void pad_out(smartiov &iov, int pages)
{
    if (z_page == NULL) {
        z_page = (char *)aligned_alloc(4096, 4096);
        memset(z_page, 0, 4096); // make valgrind happy
    }
    for (int i = 0; i < pages; i++)
        iov.push_back((iovec){z_page, 4096});
}

/* lba,iov: request from liblsvd
 * n_pages: number of 4KB data pages (not counting header)
 * page:    page number to begin writing
 * n_pad:   number of pages to skip (not counting header)
 * pad:     page number for pad entry (0 if none)
 */
wcache_write_req::wcache_write_req(sector_t lba, smartiov *iovs, page_t n_pages,
                                   page_t page, page_t n_pad, page_t pad,
                                   page_t prev, write_cache *wcache_)
{
    wcache = (write_cache_impl *)wcache_;

    /* if we pad, then the data record has prev=pad, and the pad
     * has prev=prev
     */
    if (pad != 0) {
        pad_hdr = (char *)aligned_alloc(4096, 4096);
        wcache->mk_header(pad_hdr, LSVD_J_PAD, n_pad + 1, prev);
        prev = pad;

        /* if we pad before journal wraparound, zero out the remaining
         * pages to make crash recovery easier.
         */
        pad_iov.push_back((iovec){pad_hdr, 4096});
        pad_out(pad_iov, n_pad);
        reqs++;
        r_pad = wcache->nvme_w->make_write_request(&pad_iov, pad * 4096L);
    }

    vec<j_extent> extents;
    extents.push_back((j_extent){(uint64_t)lba, iovs->bytes() / 512});

    /* TODO: don't assign seq# in mk_header
     */
    hdr = (char *)aligned_alloc(4096, 4096);
    j_hdr *j = wcache->mk_header(hdr, LSVD_J_DATA, 1 + n_pages, prev);
    seq = j->seq;

    j->extent_offset = sizeof(*j);
    size_t e_bytes = extents.size() * sizeof(j_extent);
    j->extent_len = e_bytes;
    memcpy((void *)(hdr + sizeof(*j)), (void *)extents.data(), e_bytes);

    plba = (page + 1) * 8;
    data_iovs = new smartiov();
    data_iovs->push_back((iovec){hdr, 4096});
    auto [iov, iovcnt] = iovs->c_iov();
    data_iovs->ingest(iov, iovcnt);

    reqs++;
    r_data = wcache->nvme_w->make_write_request(data_iovs, page * 4096L);
}

wcache_write_req::~wcache_write_req()
{
    free(hdr);
    if (pad_hdr)
        free(pad_hdr);
    delete data_iovs;
}

/* called in order from notify_complete
 * write cache lock must be held
 */
void wcache_write_req::notify(request *child)
{
    child->release();
    if (--reqs > 0)
        return;
    parent->notify(this);

    std::unique_lock lk(wcache->m);
    wcache->outstanding_writes--;
    wcache->write_cv.notify_all();

    /* we don't implement release or wait - just delete ourselves.
     */
    delete this;
}

void wcache_write_req::run(request *parent_)
{
    parent = parent_;
    if (r_pad)
        r_pad->run(this);
    r_data->run(this);
}

/* --------------- Write Cache ------------- */

/**
 * stall write requests using window of max_write_blocks, which should
 * be <= 0.5 * write cache size. Backpressure for the write journal, also
 * prevents wraparound
 *
 * TODO record how long this takes per request, unlikely to be bottleneck though
 */
void write_cache_impl::reserve_room(sector_t sectors)
{
    int pages = sectors / 8;
    std::unique_lock lk(m2);
    while (total_write_pages + pages > max_write_pages)
        write_cv.wait(lk);
    total_write_pages += pages;
}

void write_cache_impl::release_room(sector_t sectors)
{
    int pages = sectors / 8;
    std::unique_lock lk(m2);
    total_write_pages -= pages;
    if (total_write_pages < max_write_pages)
        write_cv.notify_all();
}

/* this is kind of messed up. total_write_pages only counts calls to
 * get/release_room from lsvd.cc, outstanding.size() should be zero whenever
 * total_write_pages is zero. Could just flush at higher level.
 */
void write_cache_impl::flush(void)
{
    std::unique_lock lk2(m2); // total_write_pages
    std::unique_lock lk(m);   // outstanding
    while (total_write_pages > 0 || outstanding_writes > 0)
        write_cv.wait(lk);
}

/* must be called with lock held.
 * n:        total length to allocate (including header)
 * <return>: page number for header
 * pad:      page number for pad header (or 0)
 * n_pad:    total pages for pad
 * prev_:    previous header page (reverse link)
 *
 * TODO: totally confusing that n_pad includes the header page here,
 * while it excluses the header page in wcache_write_req constructor
 */
uint32_t write_cache_impl::allocate(page_t n, page_t &pad, page_t &n_pad,
                                    page_t &prev)
{
    // assert(!m.try_lock());
    assert(n > 0);
    assert(next_alloc >= super->base && next_alloc < super->limit);

    prev = previous_hdr;
    pad = n_pad = 0;

    if (super->limit - next_alloc < n) {
        pad = next_alloc;
        n_pad = super->limit - next_alloc;
        next_alloc = super->base;
    }

    auto start = previous_hdr = next_alloc;
    next_alloc += n;
    if (next_alloc == super->limit)
        next_alloc = super->base;
    return start;
}

/* call with lock held
 */
j_hdr *write_cache_impl::mk_header(char *buf, uint32_t type, page_t blks,
                                   page_t prev)
{
    // assert(!m.try_lock());
    memset(buf, 0, 4096);
    j_hdr *h = (j_hdr *)buf;
    // OH NO - am I using wcache->sequence or wcache->super->seq???
    *h = (j_hdr){.magic = LSVD_MAGIC,
                 .type = type,
                 .version = 1,
                 .len = blks,
                 .seq = sequence++,
                 .crc32 = 0,
                 .extent_offset = 0,
                 .extent_len = 0,
                 .prev = prev};
    return h;
}

/* note that this is only called on shutdown, so we don't
 * worry about locking, and we set the 'clean' flag in the superblock
 */
void write_cache_impl::write_checkpoint(void)
{
    // shouldn't really need the copy, since it's only called on
    // shutdown, except that some unit tests call this and expect things
    // to work afterwards
    vec<byte> buf(4096);
    auto super_copy = (j_write_super *)buf.data();

    memcpy(super_copy, super, 4096);
    super_copy->seq = sequence;
    super_copy->next = 0; // FIXME - not used anymore

    super_copy->map_start = 0;
    super_copy->map_blocks = 0;
    super_copy->map_entries = 0;

    super_copy->len_start = 0;
    super_copy->len_blocks = 0;
    super_copy->len_entries = 0;

    super_copy->clean = true;

    auto res = nvme_w->write(buf.data(), buf.size(), 4096);
    THROW_ERRNO_ON(res < 0, errno, "Failed to write wlog header");
}

/* needs to set the following variables:
 *  super->next
 *  next_acked_page
 *  sequence
 */
int write_cache_impl::roll_log_forward()
{
    return 0;

#if 0
    page_t start = super->base, prev = 0;
    auto h = (j_hdr *)_hdrbuf;

    if (nvme_w->read(_hdrbuf, 4096, 4096L * start) < 0)
        throw_fs_error("cache log roll-forward");

    /* nothing in cache
     */
    if (h->magic != LSVD_MAGIC || h->type != LSVD_J_DATA) {
        sequence = 1;
        super->next = next_acked_page = super->base;
        // before = after = {}
        return 0;
    }
    sequence = h->seq;

    /* find the oldest journal entry
     */
    while (true) {
        prev = h->prev;
        if (prev == 0) // hasn't wrapped
            break;
        if (nvme_w->read(_hdrbuf, 4096, 4096L * prev) < 0)
            throw_fs_error("cache log roll-forward");
        if (h->magic != LSVD_MAGIC || h->seq != sequence - 1 ||
            (h->type != LSVD_J_DATA && h->type != LSVD_J_PAD))
            break;
        sequence = h->seq;
        start = prev;
    }

    /* Read all the records in, update lengths and map, and write
     * to backend if they're newer than the last write the translation
     * layer guarantees is persisted.
     */
    while (true) {
        /* handle wrap-around
         */
        if (start == super->limit) {
            after.insert(after.end(), before.begin(), before.end());
            before.clear();
            start = super->base;
            continue;
        }

        /* is this another journal record?
         */
        if (nvme_w->read(_hdrbuf, 4096, 4096L * start) < 0)
            throw_fs_error("cache log roll-forward");
        if (h->magic != LSVD_MAGIC || h->seq != sequence ||
            (h->type != LSVD_J_DATA && h->type != LSVD_J_PAD))
            break;

        if (h->type == LSVD_J_PAD) {
            start = super->limit;
            sequence++;
            continue;
        }

        before.push_back({start, h->len});

        /* read LBA info from header, read data, then
         * - put mappings into cache map
         * - write data to backend
         */
        vec<j_extent> entries;
        decode_offset_len<j_extent>(_hdrbuf, h->extent_offset, h->extent_len,
                                    entries);

        size_t data_len = 4096L * (h->len - 1);
        char *data = (char *)aligned_alloc(4096, data_len);
        if (nvme_w->read(data, data_len, 4096L * (start + 1)) < 0)
            throw_fs_error("wcache");

        sector_t plba = (start + 1) * 8;
        size_t offset = 0;

        /* all write batches with sequence < max_cache_seq are
         * guaranteed to be persisted in the backend already
         */
        for (auto e : entries) {
            map.update(e.lba, e.lba + e.len, plba);

            size_t bytes = e.len * 512;
            iovec iov = {data + offset, bytes};
            if (sequence >= be->max_cache_seq) {
                do_log("write %ld %d+%d\n", sequence.load(), (int)e.lba, e.len);
                be->writev(sequence, e.lba * 512, &iov, 1);
            } else
                do_log("skip %ld %d (max %d) %ld+%d\n", start, sequence.load(),
                       be->max_cache_seq, e.lba, e.len);
            offset += bytes;
            plba += e.len;
        }

        free(data);

        start += h->len;
        sequence++;
    }

    super->next = next_acked_page = cleared_limit = start;

    be->flush();
    usleep(10000);

    return 0;
#endif
}

write_cache_impl::write_cache_impl(int fd, translate &be, lsvd_config &cfg)
    : fd(fd), be(be)
{
    dev_max = getsize64(fd);

    nvme_w = make_nvme_uring(fd, "wlog_uring");

    char *buf = (char *)aligned_alloc(4096, 4096);
    auto res = nvme_w->read(buf, 4096, 4096L * SUPER_BLOCKNO);
    THROW_ERRNO_ON(res < 0, -res, "Failed to read wlog header");
    THROW_MSG_ON(res < 4096, "Short read {}/4096 on wlog header", res);

    super = (j_write_super *)buf;
    THROW_MSG_ON(super->magic != LSVD_MAGIC, "Invalid magic in wlog sub-hdr");

    // if it's clean we can read in the map and lengths, otherwise
    // do crash recovery. Then set the dirty flag
    if (super->clean) {
        sequence = super->seq;
        next_alloc = super->base;
    } else if (roll_log_forward() < 0)
        throw("write log roll-forward failed");
    next_alloc = super->base;

    super->clean = false;
    res = nvme_w->write(buf, 4096, 4096L * SUPER_BLOCKNO);
    THROW_ERRNO_ON(res < 0, -res, "Failed to write wlog subhdr");

    int n_pages = super->limit - super->base;
    max_write_pages = n_pages / 2 + n_pages / 4; // no idea why this is 3/4ths
    write_batch = cfg.wcache_batch;
}

write_cache_impl::~write_cache_impl()
{
    close(fd);
    free(super);
    delete nvme_w;
}

request *write_cache_impl::writev(sector_t lba, smartiov *iovs)
{
    size_t bytes = iovs->bytes();
    page_t pages = div_round_up(bytes, 4096);
    page_t pad, n_pad, prev = 0;

    // Ordering: we hold the lock to maintain the same ordering between the
    // write journal and the in-memory/backend data structures

    std::unique_lock lk(m);
    page_t page = allocate(pages + 1, pad, n_pad, prev);
    auto req = new wcache_write_req(lba, iovs, pages, page, n_pad - 1, pad,
                                    prev, this);
    auto [iov, iovcnt] = iovs->c_iov();
    outstanding_writes++;

    // this unlock may not be in the right place, move it to below writev?
    lk.unlock();

    // writing to in-memory buffer (translation layer)
    be.writev(req->seq, lba * 512, iov, iovcnt);

    return req;
}

void write_cache_impl::do_write_checkpoint(void) { write_checkpoint(); }

int init_wcache(int fd, uuid_t &uuid, usize cache_size)
{
    // write log file has 2 header blocks: the first 4k block is the j_hdr,
    // the second 4k block is the j_write_super
    // not entirely sure why they are separate, but I'm leaving it for now

    page_t total_pages = cache_size / 4096;
    page_t content_pages = total_pages - 2;
    page_t _map = div_round_up(content_pages, 256);
    page_t _len = div_round_up(content_pages, 512);
    page_t meta_pages = 2 * (_map + _len);
    page_t data_pages = content_pages - meta_pages;

    vec<byte> buf(4096 * 2);
    auto hdr = (j_hdr *)buf.data();
    *hdr = {
        .magic = LSVD_MAGIC,
        .type = LSVD_J_DATA,
        .version = 1,
        .len = total_pages,
        .seq = 0,
        .crc32 = 0,
        .extent_offset = 0,
        .extent_len = 0,
        .prev = 0,
    };

    auto sup = (j_write_super *)(buf.data() + 4096);
    *sup = {
        .magic = LSVD_MAGIC,
        .type = LSVD_J_W_SUPER,
        .version = 1,
        .clean = 1,
        .seq = 1,
        .meta_base = 1,
        .meta_limit = 1 + meta_pages,
        .base = 1 + meta_pages,
        .limit = 1 + meta_pages + data_pages,
        .next = 1 + meta_pages,
        .map_start = 0,
        .map_blocks = 0,
        .map_entries = 0,
        .len_start = 0,
        .len_blocks = 0,
        .len_entries = 0,
        .vol_uuid = {0},
    };
    uuid_copy(sup->vol_uuid, uuid);

    int ret = pwrite(fd, buf.data(), buf.size(), 0);
    PR_ERR_RET_IF(ret < 0, -errno, errno, "Failed to write wlog header");

    // just truncate to right length, don't bother writing zeroes
    ret = ftruncate(fd, 4096 * total_pages);
    PR_ERR_RET_IF(ret < 0, -errno, errno, "Failed to truncate wlog file");

    return 0;
}

uptr<write_cache> open_wlog(fspath path, translate &xlate, lsvd_config &cfg)
{
    log_info("Opening write log at '{}'", path.string());

    int fd = 0;
    if (!std::filesystem::exists(path)) {
        log_info("Creating write log file '{}'", path.string());
        fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
        PR_ERR_RET_IF(fd < 0, nullptr, errno, "Failed to create cache file");

        auto err = init_wcache(fd, xlate.uuid, cfg.wlog_size);
        PR_ERR_RET_IF(err < 0, nullptr, -err, "Failed to init wlog");
    } else {
        fd = open(path.c_str(), O_RDWR);
        PR_ERR_RET_IF(fd < 0, nullptr, errno, "Failed to open wlog file");
    }

    char buf[4096];
    int err = pread(fd, buf, 4096, 0);
    PR_ERR_RET_IF(err < 0, nullptr, errno, "Failed to read wlog header");

    auto super = (j_hdr *)buf;
    PR_RET_IF(super->magic != LSVD_MAGIC, nullptr,
              "Invalid write cache magic number: {}", super->magic);
    PR_RET_IF(super->type != LSVD_J_DATA, nullptr, "Invalid cache type: {}",
              super->type);

    try {
        return std::make_unique<write_cache_impl>(fd, xlate, cfg);
    } catch (std::exception &e) {
        log_error("Failed to open write cache: {}", e.what());
        close(fd);
        return nullptr;
    }
}
