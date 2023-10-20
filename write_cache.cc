#include "write_cache.h"
#include <memory>

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

class wcache_write_req : public request,
                         public std::enable_shared_from_this<wcache_write_req>
{
    char *hdr = NULL;
    smartiov data_iovs;
    std::unique_ptr<request> req_data;

    char *pad_hdr = NULL;
    smartiov pad_iov;
    std::unique_ptr<request> req_pad;

    std::atomic<int> reqs = 0; // 2 if req_pad else 1
    write_cache &wcache;
    sptr<request> parent;

  public:
    uint64_t seq;

    /* lba,iov: request from liblsvd
     * n_pages: number of 4KB data pages (not counting header)
     * page:    page number to begin writing
     * n_pad:   number of pages to skip (not counting header)
     * pad:     page number for pad entry (0 if none)
     */
    wcache_write_req(sector_t lba, smartiov *iovs, page_t n_pages, page_t page,
                     page_t n_pad, page_t pad, page_t prev,
                     write_cache &wcache_)
        : wcache(wcache_)
    {
        // if we pad, then the data record has prev=pad, and the pad
        // has prev=prev
        if (pad != 0) {
            pad_hdr = (char *)aligned_alloc(4096, 4096);
            wcache.mk_header(pad_hdr, LSVD_J_PAD, n_pad + 1, prev);
            prev = pad;

            // if we pad before journal wraparound, zero out the remaining
            // pages to make crash recovery easier.
            pad_iov.push_back((iovec){pad_hdr, 4096});
            pad_out(pad_iov, n_pad);
            reqs++;
            req_pad = wcache.nvme_w->make_write_request(&pad_iov, pad * 4096L);
        }

        std::vector<j_extent> extents;
        extents.push_back((j_extent){(uint64_t)lba, iovs->bytes() / 512});

        // TODO: don't assign seq# in mk_header
        hdr = (char *)aligned_alloc(4096, 4096);
        j_hdr *j = wcache.mk_header(hdr, LSVD_J_DATA, 1 + n_pages, prev);
        seq = j->seq;

        j->extent_offset = sizeof(*j);
        size_t e_bytes = extents.size() * sizeof(j_extent);
        j->extent_len = e_bytes;
        memcpy((void *)(hdr + sizeof(*j)), (void *)extents.data(), e_bytes);

        // plba = (page + 1) * 8;
        data_iovs.push_back((iovec){hdr, 4096});
        auto [iov, iovcnt] = iovs->c_iov();
        data_iovs.ingest(iov, iovcnt);

        reqs++;
        req_data = wcache.nvme_w->make_write_request(&data_iovs, page * 4096L);
    }

    ~wcache_write_req()
    {
        free(hdr);
        if (pad_hdr)
            free(pad_hdr);
    }

    void run(sptr<request> parent_)
    {
        parent = parent_;
        if (req_pad)
            req_pad->run(shared_from_this());
        req_data->run(shared_from_this());
    }

    // called in order from notify_complete
    // write cache lock must be held
    void notify()
    {
        if (--reqs > 0)
            return;

        parent->notify();
        parent.reset();

        std::unique_lock lk(wcache.m);
        wcache.outstanding_writes--;
        wcache.write_cv.notify_all();
    }

    void wait() { NOT_IMPLEMENTED(); }
    void release() { NOT_IMPLEMENTED(); }
};

/* ------------- batched write request ------------- */

/* --------------- Write Cache ------------- */

/* stall write requests using window of max_write_blocks, which should
 * be <= 0.5 * write cache size.
 */
void write_cache::get_room(sector_t sectors)
{
    int pages = sectors / 8;
    std::unique_lock lk(m2);
    while (total_write_pages + pages > max_write_pages)
        write_cv.wait(lk);
    total_write_pages += pages;
}

void write_cache::release_room(sector_t sectors)
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
void write_cache::flush(void)
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
uint32_t write_cache::allocate(page_t n, page_t &pad, page_t &n_pad,
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
j_hdr *write_cache::mk_header(char *buf, uint32_t type, page_t blks,
                              page_t prev)
{
    // assert(!m.try_lock());
    memset(buf, 0, 4096);
    j_hdr *h = (j_hdr *)buf;
    // OH NO - am I using wcache.sequence or wcache.super->seq???
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
void write_cache::write_checkpoint(void)
{
    /* shouldn't really need the copy, since it's only called on
     * shutdown, except that some unit tests call this and expect things
     * to work afterwards
     */
    j_write_super *super_copy = (j_write_super *)aligned_alloc(4096, 4096);

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

    if (nvme_w->write((char *)super_copy, 4096, 4096L * super_blkno) < 0)
        throw_fs_error("wckpt_s");

    free(super_copy);
}

/* needs to set the following variables:
 *  super->next
 *  next_acked_page
 *  sequence
 */
int write_cache::roll_log_forward()
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
        std::vector<j_extent> entries;
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

write_cache::write_cache(uint32_t blkno, int fd, translate *_be,
                         lsvd_config *cfg_)
{

    super_blkno = blkno;
    dev_max = getsize64(fd);
    be = _be;
    cfg = cfg_;

    _hdrbuf = (char *)aligned_alloc(4096, 4096);

    const char *name = "write_cache_cb";
    nvme_w = make_nvme_uring(fd, name);

    char *buf = (char *)aligned_alloc(4096, 4096);
    if (nvme_w->read(buf, 4096, 4096L * super_blkno) < 4096)
        throw_fs_error("wcache");
    super = (j_write_super *)buf;

    /* if it's clean we can read in the map and lengths, otherwise
     * do crash recovery. Then set the dirty flag
     */
    if (super->clean) {
        sequence = super->seq;
        next_alloc = super->base;
    } else if (roll_log_forward() < 0)
        throw("write log roll-forward failed");
    next_alloc = super->base;

    super->clean = false;
    if (nvme_w->write(buf, 4096, 4096L * super_blkno) < 4096)
        throw_fs_error("wcache");

    int n_pages = super->limit - super->base;
    max_write_pages = n_pages / 2 + n_pages / 4;
    write_batch = cfg->wcache_batch;

    misc_threads = new thread_pool<int>(&m);
}

std::unique_ptr<write_cache> write_cache::make_write_cache(uint32_t blkno,
                                                           int fd,
                                                           translate *be,
                                                           lsvd_config *cfg)
{
    return std::make_unique<write_cache>(blkno, fd, be, cfg);
}

write_cache::~write_cache()
{
    delete misc_threads;
    free(super);
    free(_hdrbuf);
}

sptr<request> write_cache::writev(sector_t lba, smartiov *iovs)
{

    size_t bytes = iovs->bytes();
    page_t pages = div_round_up(bytes, 4096);
    page_t pad, n_pad, prev = 0;

    std::unique_lock lk(m);
    page_t page = allocate(pages + 1, pad, n_pad, prev);
    auto req = std::make_shared<wcache_write_req>(lba, iovs, pages, page,
                                                  n_pad - 1, pad, prev, *this);
    auto [iov, iovcnt] = iovs->c_iov();
    outstanding_writes++;
    lk.unlock();

    be->writev(req->seq, lba * 512, iov, iovcnt);
    return req;
}

void write_cache::do_write_checkpoint(void) { write_checkpoint(); }
