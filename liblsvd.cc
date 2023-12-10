/*
 * file:        lsvd.cc
 * description: userspace block-on-object layer with librbd interface
 *
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <stack>
#include <string>
#include <thread>
#include <unistd.h>
#include <uuid/uuid.h>
#include <vector>

#include "backend.h"
#include "config.h"
#include "extent.h"
#include "fake_rbd.h"
#include "image.h"
#include "img_reader.h"
#include "journal.h"
#include "lsvd_types.h"
#include "misc_cache.h"
#include "nvme.h"
#include "request.h"
#include "shared_read_cache.h"
#include "smartiov.h"
#include "translate.h"
#include "utils.h"
#include "write_cache.h"

extern void do_log(const char *, ...);
extern void fp_log(const char *, ...);

extern void check_crc(sector_t sector, iovec *iov, int niovs, const char *msg);
extern void add_crc(sector_t sector, iovec *iov, int niovs);

/* RBD "image" and completions are only used in this file, so we
 * don't break them out into a .h
 */

extern int init_wcache(int fd, uuid_t &uuid, int n_pages);

int rbd_image::image_open(rados_ioctx_t io, const char *name)
{
    if (cfg.read() < 0)
        throw std::runtime_error("Failed to read config");

    objstore = get_backend(&cfg, io, name);
    auto shared_cache_path = cfg.shared_read_cache_path;
    shared_cache = shared_read_cache::get_instance(
        shared_cache_path, cfg.cache_size / CACHE_CHUNK_SIZE, objstore);

    /* read superblock and initialize translation layer
     */
    xlate =
        make_translate(objstore, &cfg, &map, &bufmap, &map_lock, &bufmap_lock);
    size = xlate->init(name, true);
    check_cond(size < 0, "Failed to initialize translation layer err={}", size);

    /* figure out cache file name, create it if necessary
     */

    /*
     * TODO: Open 2 files. One for wcache and one for reader
     */
    std::string wcache_name =
        cfg.cache_filename(xlate->uuid, name, LSVD_CFG_WRITE);

    if (access(wcache_name.c_str(), R_OK | W_OK) < 0) {
        log_info("Creating write cache file {}", wcache_name);
        int cache_pages = cfg.wlog_size / 4096;

        int fd = open(wcache_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
        check_ret_errno(fd, "Can't open wcache file");

        if (init_wcache(fd, xlate->uuid, cache_pages) < 0)
            return -1;
        close(fd);
    }

    write_fd = open(wcache_name.c_str(), O_RDWR);
    check_ret_errno(write_fd, "Can't open wcache file");

    j_write_super *jws = (j_write_super *)aligned_alloc(512, 4096);

    check_ret_errno(pread(write_fd, (char *)jws, 4096, 0),
                    "Can't read wcache superblock");
    if (jws->magic != LSVD_MAGIC || jws->type != LSVD_J_W_SUPER)
        throw std::runtime_error("bad magic/type in write cache superblock\n");
    if (memcmp(jws->vol_uuid, xlate->uuid, sizeof(uuid_t)) != 0)
        throw std::runtime_error("object and cache UUIDs don't match");

    wcache = make_write_cache(0, write_fd, xlate, &cfg);
    reader = make_reader(0, xlate, &cfg, &map, &bufmap, &map_lock, &bufmap_lock,
                         objstore, shared_cache);
    free(jws);

    if (!cfg.no_gc)
        xlate->start_gc();
    return 0;
}

void rbd_image::notify(rbd_completion_t c)
{
    std::unique_lock lk(m);
    if (ev.is_valid()) {
        completions.push(c);
        ev.notify();
    }
}

/* for debug use
 */
rbd_image *make_rbd_image(sptr<backend> b, translate *t, write_cache *w,
                          img_reader *r)
{
    auto img = new rbd_image;
    img->objstore = b;
    img->xlate = t;
    img->wcache = w;
    img->reader = r;
    return img;
}

translate *image_2_xlate(rbd_image_t image)
{
    auto img = (rbd_image *)image;
    return img->xlate;
}

rbd_image_t __lsvd_dbg_img;

extern "C" int rbd_open(rados_ioctx_t io, const char *name, rbd_image_t *image,
                        const char *snap_name)
{
    auto img = new rbd_image;
    if (img->image_open(io, name) < 0) {
        log_error("Failed to open image {}", name);
        delete img;
        return -1;
    }
    *image = __lsvd_dbg_img = (void *)img;
    log_info("opened image: {}, size {}", name, img->size);
    return 0;
}

int rbd_image::image_close(void)
{
    delete reader;
    wcache->flush();
    wcache->do_write_checkpoint();
    delete wcache;
    if (!cfg.no_gc)
        xlate->stop_gc();
    xlate->checkpoint();
    delete xlate;
    close(write_fd);
    return 0;
}

extern "C" int rbd_close(rbd_image_t image)
{
    rbd_image *img = (rbd_image *)image;
    log_info("closing image");
    img->image_close();
    delete img;

    return 0;
}

/* RBD-level completion structure
 */
struct lsvd_completion {
  public:
    int magic = LSVD_MAGIC;
    rbd_image *img;
    rbd_callback_t cb;
    void *arg;
    int retval;
    std::mutex m;
    std::condition_variable cv;
    /* done: += 1
     * released: += 10
     * caller waiting: += 20
     */
    std::atomic<int> done_released = 0;
    request *req;

    lsvd_completion(rbd_callback_t cb_, void *arg_) : cb(cb_), arg(arg_) {}

    /* see Ceph AioCompletion::complete
     */
    void complete(int val)
    {
        retval = val;
        if (cb)
            cb((rbd_completion_t)this, arg);
        img->notify((rbd_completion_t)this);

        std::unique_lock lk(m);
        int x = (done_released += 1);
        cv.notify_all();
        if (x == 11) {
            lk.unlock();
            delete this;
        }
    }

    void release()
    {
        int x = (done_released += 10);
        if (x == 11)
            delete this;
    }
};

int rbd_image::poll_io_events(rbd_completion_t *comps, int numcomp)
{
    std::unique_lock lk(m);
    int i;
    for (i = 0; i < numcomp && !completions.empty(); i++) {
        auto p = (lsvd_completion *)completions.front();
        assert(p->magic == LSVD_MAGIC);
        comps[i] = p;
        completions.pop();
    }
    return i;
}

extern "C" int rbd_poll_io_events(rbd_image_t image, rbd_completion_t *comps,
                                  int numcomp)
{
    rbd_image *img = (rbd_image *)image;
    int rv = img->poll_io_events(comps, numcomp);
    return rv;
}

extern "C" int rbd_set_image_notification(rbd_image_t image, int fd, int type)
{
    rbd_image *img = (rbd_image *)image;
    assert(type == EVENT_TYPE_EVENTFD);
    return img->ev.init(fd, type);
}

extern "C" int rbd_aio_create_completion(void *cb_arg,
                                         rbd_callback_t complete_cb,
                                         rbd_completion_t *c)
{
    lsvd_completion *p = new lsvd_completion(complete_cb, cb_arg);
    *c = (rbd_completion_t)p;
    return 0;
}

extern "C" void rbd_aio_release(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);
    p->release();
}

extern "C" int rbd_discard(rbd_image_t image, uint64_t ofs, uint64_t len)
{
    auto img = (rbd_image *)image;
    img->xlate->trim(ofs, len);
    return 0;
}

extern "C" int rbd_aio_discard(rbd_image_t image, uint64_t off, uint64_t len,
                               rbd_completion_t c)
{
    auto p = (lsvd_completion *)c;
    auto img = p->img = (rbd_image *)image;
    img->xlate->trim(off, len);
    p->complete(0);
    return 0;
}

extern "C" int rbd_aio_flush(rbd_image_t image, rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);
    auto img = p->img = (rbd_image *)image;

    if (img->cfg.hard_sync)
        img->xlate->flush();

    p->complete(0); // TODO - make asynchronous
    return 0;
}

extern "C" int rbd_flush(rbd_image_t image)
{
    auto img = (rbd_image *)image;
    if (img->cfg.hard_sync)
        img->xlate->flush();
    return 0;
}

extern "C" void *rbd_aio_get_arg(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);
    return p->arg;
}

extern "C" ssize_t rbd_aio_get_return_value(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);
    return p->retval;
}

static std::atomic<int> __reqs;

enum rbd_req_status {
    REQ_NOWAIT = 0,
    REQ_COMPLETE = 1,
    REQ_LAUNCHED = 2,
    REQ_WAIT = 4
};

/* rbd_aio_req - state machine for rbd_aio_read, rbd_aio_write
 *
 * TODO: fix this. I merged separate read & write classes in the
 * ugliest possible way, but it works...
 */
class rbd_aio_req : public request
{
    rbd_image *img;
    lsvd_completion *p;
    char *aligned_buf = NULL;
    uint64_t offset; // in bytes
    sector_t _sector;
    lsvd_op op;
    smartiov iovs;
    smartiov aligned_iovs;
    sector_t sectors = 0;
    std::vector<smartiov *> to_free;

    std::atomic<int> n_req = 0;
    std::atomic<int> status = 0;
    std::mutex m;
    std::condition_variable cv;

    // std::vector<request *> requests;

    void notify_parent(void)
    {
        // assert(!m.try_lock());
        if (p != NULL) {
            if (op == OP_READ) {
                p->complete(sectors * 512L);
            } else {
                p->complete(0);
            }
        }
        if (status & REQ_WAIT)
            cv.notify_all();
    }

    void notify_w(request *req)
    {
        std::unique_lock lk(m);
        auto z = --n_req;
        if (z > 0)
            return;

        img->wcache->release_room(sectors);

        auto x = (status |= REQ_COMPLETE);
        if (x & REQ_LAUNCHED) {
            lk.unlock();
            notify_parent();
            if (!(x & REQ_WAIT))
                delete this;
        }
    }

    void run_w()
    {
        if (aligned_buf) // copy to aligned *before* write
            iovs.copy_out(aligned_buf);

        img->wcache->get_room(sectors);
        img->xlate->wait_for_room();

        /* split large requests into 2MB (default) chunks
         */
        sector_t max_sectors = img->cfg.wcache_chunk / 512;
        n_req += div_round_up(sectors, max_sectors);
        // TODO: this is horribly ugly

        std::unique_lock lk(m);
        std::vector<request *> requests;

        for (sector_t s_offset = 0; s_offset < sectors;
             s_offset += max_sectors) {
            auto _sectors = std::min(sectors - s_offset, max_sectors);
            smartiov tmp = aligned_iovs.slice(
                s_offset * 512L, s_offset * 512L + _sectors * 512L);
            smartiov *_iov = new smartiov(tmp.data(), tmp.size());
            to_free.push_back(_iov);
            auto req = img->wcache->writev(offset / 512, _iov);
            requests.push_back(req);
            offset += _sectors * 512L;
        }

        for (auto r : requests)
            r->run(this);

        auto x = (status |= REQ_LAUNCHED);

        if (x == (REQ_LAUNCHED | REQ_COMPLETE)) {
            lk.unlock();
            notify_parent();
            delete this;
        }
    }

    void notify_r(request *child)
    {
        if (child)
            child->release();

        std::unique_lock lk(m);
        // do_log("rr done %d %p\n", n_req.load(), child);
        n_req.fetch_sub(1, std::memory_order_seq_cst);
        // debug("n_req {}", n_req.load());
        if (n_req > 0)
            return;

        if (aligned_buf) // copy from aligned *after* read
            iovs.copy_in(aligned_buf);

        auto x = (status |= REQ_COMPLETE);

        if (x & REQ_LAUNCHED) {
            lk.unlock();
            notify_parent();
            if (!(x & REQ_WAIT))
                delete this;
        }
    }

    void run_r()
    {
        __reqs++;
        std::vector<request *> requests;

        img->reader->handle_read(offset, &aligned_iovs, requests);

        n_req = requests.size();
        // do_log("rbd_read %ld:\n", offset/512);
        for (auto const &r : requests) {
            // do_log(" %p\n", r);
            r->run(this);
        }

        std::unique_lock lk(m);
        if (requests.size() == 0)
            status |= REQ_COMPLETE;
        auto x = (status |= REQ_LAUNCHED);

        if (x == (REQ_LAUNCHED | REQ_COMPLETE)) {
            lk.unlock();
            notify_parent();
            delete this;
        }
    }

    void setup(lsvd_op op_, rbd_image *img_, lsvd_completion *p_,
               uint64_t offset_, int status_)
    {
        op = op_; // OP_READ or OP_WRITE
        img = img_;
        p = p_;
        if (p != NULL)
            assert(p->magic == LSVD_MAGIC);
        offset = offset_; // byte offset into volume
        _sector = offset / 512;
        status = status_; // 0 or REQ_WAIT
        sectors = iovs.bytes() / 512L;

        if (iovs.aligned(512))
            aligned_iovs = iovs;
        else {
            aligned_buf = (char *)aligned_alloc(512, iovs.bytes());
            iovec iov = {aligned_buf, iovs.bytes()};
            aligned_iovs.ingest(&iov, 1);
        }
    }

  public:
    rbd_aio_req(lsvd_op op_, rbd_image *img_, lsvd_completion *p_,
                uint64_t offset_, int status_, const iovec *iov, size_t niov)
        : iovs(iov, niov)
    {
        setup(op_, img_, p_, offset_, status_);
    }
    rbd_aio_req(lsvd_op op_, rbd_image *img_, lsvd_completion *p_,
                uint64_t offset_, int status_, char *buf_, size_t len_)
        : iovs(buf_, len_)
    {
        setup(op_, img_, p_, offset_, status_);
    }
    ~rbd_aio_req()
    {
        if (aligned_buf)
            free(aligned_buf);
        for (auto iov : to_free)
            delete iov;
    }

    /* note that there's no child request until read cache is updated
     * to use request/notify model.
     */
    void notify(request *child)
    {
        if (op == OP_READ)
            notify_r(child);
        else
            notify_w(child);
    }

    /* TODO: this is really gross. To properly fix it I need to integrate this
     * with rbd_aio_completion and use its release() method
     */
    void wait()
    {
        assert(status & REQ_WAIT);
        std::unique_lock lk(m);
        while (!(status & REQ_COMPLETE))
            cv.wait(lk);
        lk.unlock();
        delete this;
    }

    void release() {}

    void run(request *parent /* unused */)
    {
        if (op == OP_READ)
            run_r();
        else
            run_w();
    }

    static void aio_read_cb(void *ptr)
    {
        auto req = (rbd_aio_req *)ptr;
        req->notify(NULL);
    }
};

extern "C" int rbd_aio_read(rbd_image_t image, uint64_t offset, size_t len,
                            char *buf, rbd_completion_t c)
{
    rbd_image *img = (rbd_image *)image;
    auto p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);
    p->img = img;

    p->req = new rbd_aio_req(OP_READ, img, p, offset, REQ_NOWAIT, buf, len);
    p->req->run(NULL);
    return 0;
}

extern "C" int rbd_aio_readv(rbd_image_t image, const iovec *iov, int iovcnt,
                             uint64_t offset, rbd_completion_t c)
{
    rbd_image *img = (rbd_image *)image;
    auto p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);
    p->img = img;

    p->req = new rbd_aio_req(OP_READ, img, p, offset, REQ_NOWAIT, iov, iovcnt);
    p->req->run(NULL);
    return 0;
}

extern "C" int rbd_aio_writev(rbd_image_t image, const struct iovec *iov,
                              int iovcnt, uint64_t offset, rbd_completion_t c)
{
    rbd_image *img = (rbd_image *)image;
    lsvd_completion *p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);
    p->img = img;

    p->req = new rbd_aio_req(OP_WRITE, img, p, offset, REQ_NOWAIT, iov, iovcnt);
    p->req->run(NULL);
    return 0;
}

extern "C" int rbd_aio_write(rbd_image_t image, uint64_t offset, size_t len,
                             const char *buf, rbd_completion_t c)
{
    rbd_image *img = (rbd_image *)image;
    lsvd_completion *p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);
    p->img = img;

    p->req =
        new rbd_aio_req(OP_WRITE, img, p, offset, REQ_NOWAIT, (char *)buf, len);
    p->req->run(NULL);

    return 0;
}

/* note that rbd_aio_read handles aligned bounce buffers for us
 */
extern "C" int rbd_read(rbd_image_t image, uint64_t off, size_t len, char *buf)
{
    rbd_image *img = (rbd_image *)image;
    auto req = new rbd_aio_req(OP_READ, img, NULL, off, REQ_WAIT, buf, len);

    req->run(NULL);
    req->wait();
    return 0;
}

extern "C" int rbd_write(rbd_image_t image, uint64_t off, size_t len,
                         const char *buf)
{
    rbd_image *img = (rbd_image *)image;
    auto req =
        new rbd_aio_req(OP_WRITE, img, NULL, off, REQ_WAIT, (char *)buf, len);
    req->run(NULL);
    req->wait();
    return 0;
}

extern "C" int rbd_aio_wait_for_complete(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);
    p->done_released += 20;
    std::unique_lock lk(p->m);
    while ((p->done_released.load() & 1) == 0)
        p->cv.wait(lk);
    int x = (p->done_released -= 20);
    if (x == 11) {
        lk.unlock();
        delete p;
    }
    return 0;
}

/* note that obj_size and order should match chunk size in
 * rbd_aio_req::run_w
 */
extern "C" int rbd_stat(rbd_image_t image, rbd_image_info_t *info,
                        size_t infosize)
{
    rbd_image *img = (rbd_image *)image;
    memset(info, 0, sizeof(*info));
    info->size = img->size;
    info->obj_size = 1 << 22; // 2^21 bytes
    info->order = 22;         // 2^21 bytes
    info->num_objs = img->size / info->obj_size;
    return 0;
}

extern "C" int rbd_get_size(rbd_image_t image, uint64_t *size)
{
    rbd_image *img = (rbd_image *)image;
    *size = img->size;
    return 0;
}

std::pair<std::string, std::string> split_string(std::string s,
                                                 std::string delim)
{
    auto i = s.find(delim);
    return std::pair(s.substr(0, i), s.substr(i + delim.length()));
}

extern "C" int rbd_create(rados_ioctx_t io, const char *name, uint64_t size,
                          int *order)
{
    lsvd_config cfg;
    if (cfg.read() < 0)
        return -1;
    auto objstore = get_backend(&cfg, io, NULL);
    auto rv = translate_create_image(objstore, name, size);
    return rv;
}

extern "C" int rbd_clone(rados_ioctx_t io, const char *source_img,
                         const char *dest_img)
{
    lsvd_config cfg;
    if (cfg.read() < 0) {
        throw std::runtime_error("Failed to read config");
        return -1;
    }

    auto objstore = get_backend(&cfg, io, NULL);
    auto rv = translate_clone_image(objstore, source_img, dest_img);

    return rv;
}

/* remove all objects and cache file.
 * this only removes objects pointed to by the last checkpoint, plus
 * a small range of following ones - there may be dangling objects after
 * removing a deeply corrupted image
 */
extern "C" int rbd_remove(rados_ioctx_t io, const char *name)
{
    lsvd_config cfg;
    auto rv = cfg.read();
    if (rv < 0)
        return rv;
    auto objstore = get_backend(&cfg, io, NULL);
    uuid_t uu;
    if ((rv = translate_get_uuid(objstore, name, uu)) < 0)
        return rv;
    auto rcache_file = cfg.cache_filename(uu, name, LSVD_CFG_READ);
    unlink(rcache_file.c_str());
    auto wcache_file = cfg.cache_filename(uu, name, LSVD_CFG_WRITE);
    unlink(wcache_file.c_str());
    rv = translate_remove_image(objstore, name);
    return rv;
}

extern "C" void rbd_uuid(rbd_image_t image, uuid_t *uuid)
{
    rbd_image *img = (rbd_image *)image;
    memcpy(uuid, img->xlate->uuid, sizeof(uuid_t));
}

extern "C" int rbd_aio_writesame(rbd_image_t image, uint64_t off, size_t len,
                                 const char *buf, size_t data_len,
                                 rbd_completion_t c, int op_flags)
{
    int n = div_round_up(len, data_len);
    iovec iov[n];
    int niovs = 0;
    while (len > 0) {
        size_t bytes = std::min(len, data_len);
        iov[niovs++] = (iovec){(void *)buf, bytes};
        len -= bytes;
    }
    return rbd_aio_writev(image, iov, niovs, off, c);
}

char zeropage[4096];
extern "C" int rbd_aio_write_zeroes(rbd_image_t image, uint64_t off, size_t len,
                                    rbd_completion_t c, int zero_flags,
                                    int op_flags)
{
    return rbd_aio_writesame(image, off, len, zeropage, 4096, c, op_flags);
}

/* any following functions are stubs only
 */
extern "C" int rbd_invalidate_cache(rbd_image_t image)
{
    fp_log("rbd_invalidate_cache: not implemented\n");
    return 0;
}

/* These RBD functions are unimplemented and return errors
 */
extern "C" int rbd_resize(rbd_image_t image, uint64_t size)
{
    fp_log("rbd_resize: not implemented\n");
    return -1;
}

extern "C" int rbd_snap_create(rbd_image_t image, const char *snapname)
{
    fp_log("rbd_snap_create: not implemented\n");
    return -1;
}
extern "C" int rbd_snap_list(rbd_image_t image, rbd_snap_info_t *snaps,
                             int *max_snaps)
{
    fp_log("rbd_snap_list: not implemented\n");
    return -1;
}
extern "C" void rbd_snap_list_end(rbd_snap_info_t *snaps)
{
    fp_log("rbd_snap_list_end: not implemented\n");
}
extern "C" int rbd_snap_remove(rbd_image_t image, const char *snapname)
{
    fp_log("rbd_snap_remove: not implemented\n");
    return -1;
}
extern "C" int rbd_snap_rollback(rbd_image_t image, const char *snapname)
{
    fp_log("rbd_snap_rollback: not implemented\n");
    return -1;
}

/* */
extern "C" int rbd_diff_iterate2(rbd_image_t image, const char *fromsnapname,
                                 uint64_t ofs, uint64_t len,
                                 uint8_t include_parent, uint8_t whole_object,
                                 int (*cb)(uint64_t, size_t, int, void *),
                                 void *arg)
{
    fp_log("rbd_diff_iterate2: not implemented\n");
    return *(int *)0;
}

extern "C" int rbd_encryption_format(rbd_image_t image,
                                     rbd_encryption_format_t format,
                                     rbd_encryption_options_t opts,
                                     size_t opts_size)
{
    fp_log("rbd_encryption_format: not implemented\n");
    return *(int *)0;
}

extern "C" int rbd_encryption_load(rbd_image_t image,
                                   rbd_encryption_format_t format,
                                   rbd_encryption_options_t opts,
                                   size_t opts_size)
{
    fp_log("rbd_encryption_load: not implemented\n");
    return *(int *)0;
}

extern "C" int rbd_get_features(rbd_image_t image, uint64_t *features)
{
    fp_log("rbd_get_features: not implemented\n");
    return *(int *)0;
}

extern "C" int rbd_get_flags(rbd_image_t image, uint64_t *flags)
{
    fp_log("rbd_get_flags: not implemented\n");
    return *(int *)0;
}

static int _tmp;
#define ASSERT_FAIL() _tmp += *(int *)0

extern "C" void rbd_image_spec_cleanup(rbd_image_spec_t *image)
{
    fp_log("rbd_image_spec_cleanup: not implemented\n");
    ASSERT_FAIL();
}

extern "C" void rbd_linked_image_spec_cleanup(rbd_linked_image_spec_t *image)
{
    fp_log("rbd_linked_image_spec_cleanup: not implemented\n");
    ASSERT_FAIL();
}

extern "C" int rbd_mirror_image_enable(rbd_image_t image)
{
    fp_log("rbd_mirror_image_enable: not implemented\n");
    return *(int *)0;
}

extern "C" int rbd_mirror_image_enable2(rbd_image_t image,
                                        rbd_mirror_image_mode_t mode)
{
    fp_log("rbd_mirror_image_enable2: not implemented\n");
    return *(int *)0;
}

extern "C" void
rbd_mirror_image_get_info_cleanup(rbd_mirror_image_info_t *mirror_image_info)
{
    fp_log("rbd_mirror_image_get_info_cleanup: not implemented\n");
    ASSERT_FAIL();
}

extern "C" void rbd_mirror_image_global_status_cleanup(
    rbd_mirror_image_global_status_t *mirror_image_global_status)
{
    fp_log("rbd_mirror_image_global_status_cleanup: not implemented\n");
    ASSERT_FAIL();
}

extern "C" int rbd_mirror_peer_site_add(rados_ioctx_t io_ctx, char *uuid,
                                        size_t uuid_max_length,
                                        rbd_mirror_peer_direction_t direction,
                                        const char *site_name,
                                        const char *client_name)
{
    fp_log("rbd_mirror_peer_site_add: not implemented\n");
    return *(int *)0;
}

extern "C" int rbd_mirror_peer_site_get_attributes(
    rados_ioctx_t p, const char *uuid, char *keys, size_t *max_key_len,
    char *values, size_t *max_value_len, size_t *key_value_count)
{
    fp_log("rbd_mirror_peer_site_get_attributes: not implemented\n");
    return *(int *)0;
}

extern "C" int rbd_mirror_peer_site_remove(rados_ioctx_t io_ctx,
                                           const char *uuid)
{
    fp_log("rbd_mirror_peer_site_remove: not implemented\n");
    return *(int *)0;
}

extern "C" int rbd_mirror_peer_site_set_attributes(rados_ioctx_t p,
                                                   const char *uuid,
                                                   const char *keys,
                                                   const char *values,
                                                   size_t key_value_count)
{
    fp_log("rbd_mirror_peer_site_set_attributes: not implemented\n");
    return *(int *)0;
}

extern "C" int rbd_mirror_peer_site_set_name(rados_ioctx_t io_ctx,
                                             const char *uuid,
                                             const char *site_name)
{
    fp_log("rbd_mirror_peer_site_set_name: not implemented\n");
    return *(int *)0;
}

extern "C" int rbd_mirror_peer_site_set_client_name(rados_ioctx_t io_ctx,
                                                    const char *uuid,
                                                    const char *client_name)
{
    fp_log("rbd_mirror_peer_site_set_client_name: not implemented\n");
    return *(int *)0;
}

extern "C" void rbd_pool_stats_create(rbd_pool_stats_t *stats)
{
    fp_log("rbd_pool_stats_create: not implemented\n");
    ASSERT_FAIL();
}

extern "C" void rbd_pool_stats_destroy(rbd_pool_stats_t stats)
{
    fp_log("rbd_pool_stats_destroy: not implemented\n");
    ASSERT_FAIL();
}

extern "C" int rbd_pool_stats_option_add_uint64(rbd_pool_stats_t stats,
                                                int stat_option,
                                                uint64_t *stat_val)
{
    fp_log("rbd_pool_stats_option_add_uint64: not implemented\n");
    return *(int *)0;
}

extern "C" void rbd_trash_get_cleanup(rbd_trash_image_info_t *info)
{
    fp_log("rbd_trash_get_cleanup: not implemented\n");
    ASSERT_FAIL();
}

extern "C" void rbd_version(int *major, int *minor, int *extra)
{
    *major = 0;
    *minor = 0;
    *extra = 1;
}
