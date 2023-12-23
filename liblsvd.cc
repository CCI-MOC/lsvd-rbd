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

extern "C" int rbd_open(rados_ioctx_t io, const char *name, rbd_image_t *image,
                        const char *snap_name)
{
    auto img = new rbd_image;
    if (img->image_open(io, name) < 0) {
        log_error("Failed to open image {}", name);
        delete img;
        return -1;
    }
    *image = (void *)img;
    log_info("Opened image: {}, size {}", name, img->size);
    return 0;
}

extern "C" int rbd_close(rbd_image_t image)
{
    rbd_image *img = (rbd_image *)image;
    log_info("Closing image {}", img->image_name);
    img->image_close();
    delete img;

    return 0;
}

/* RBD-level completion structure
 */
struct lsvd_completion {
  public:
    int magic = LSVD_MAGIC;

  private:
    std::atomic_int refcount = 2;
    std::atomic_flag done = ATOMIC_FLAG_INIT;

    rbd_callback_t cb;
    void *cb_arg;

    rbd_image *img = nullptr;
    int retval = -1;

    request *req = nullptr;

  public:
    lsvd_completion(rbd_callback_t cb, void *cb_arg) : cb(cb), cb_arg(cb_arg) {}
    ~lsvd_completion()
    {
        if (req)
            req->release();
    }

    void delayed_init(rbd_image *img, request *req)
    {
        this->img = img;
        this->req = req;
    }

    int get_retval() { return retval; }
    void *get_cb_arg() { return cb_arg; }

    void run()
    {
        assert(req != nullptr);
        // refcount++;
        this->req->run(nullptr);
    }

    /* see Ceph AioCompletion::complete
     */
    void complete(int val)
    {
        retval = val;
        if (cb)
            cb((rbd_completion_t)this, cb_arg);
        img->notify((rbd_completion_t)this);

        done.test_and_set();
        done.notify_all();
        dec_and_free();
    }

    void release() { dec_and_free(); }

    void wait()
    {
        refcount++;
        done.wait(false, std::memory_order_seq_cst);
        refcount--;
    }

  private:
    inline void dec_and_free()
    {
        auto old = refcount.fetch_sub(1, std::memory_order_seq_cst);
        if (old == 1)
            delete this;
    }
};

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
    auto img = (rbd_image *)image;
    p->delayed_init(img, nullptr);
    img->xlate->trim(off, len);
    p->complete(0);
    return 0;
}

extern "C" int rbd_aio_flush(rbd_image_t image, rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);
    auto img = (rbd_image *)image;
    p->delayed_init(img, nullptr);

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
    return p->get_cb_arg();
}

extern "C" ssize_t rbd_aio_get_return_value(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);
    return p->get_retval();
}

/**
 * This is the base for aio read and write requests. It's copied from
 * the old rbd_aio_req omniclass, with the read and write paths split out and
 * the common completion handling moved here.
 */
class aio_request : public self_refcount_request
{
  private:
    lsvd_completion *p;
    std::atomic_flag done = false;

  protected:
    rbd_image *img;
    smartiov iovs;

    size_t req_offset;
    size_t req_bytes;

    std::mutex mtx;
    bool has_ran = false;
    bool has_notified = false;

    aio_request(rbd_image *img, size_t offset, smartiov iovs,
                lsvd_completion *p)
        : p(p), img(img), iovs(iovs), req_offset(offset)
    {
        req_bytes = iovs.bytes();
    }

    void complete_request(int val)
    {
        if (p)
            p->complete(val);

        done.test_and_set(std::memory_order_seq_cst);
        done.notify_all();
        dec_and_free();
    }

    void try_complete(int val)
    {
        if (has_notified && has_ran)
            complete_request(val);
    }

  public:
    inline virtual void wait() override
    {
        inc_rc();
        done.wait(false, std::memory_order_seq_cst);
        dec_and_free();
    }
};

class read_request : public aio_request
{
  private:
    std::atomic_int num_subreqs = 0;
    // std::atomic_bool notified = false;

  public:
    read_request(rbd_image *img, size_t offset, smartiov iovs,
                 lsvd_completion *p)
        : aio_request(img, offset, iovs, p)
    {
    }

    void run(request *parent) override
    {
        assert(parent == nullptr);

        std::unique_lock<std::mutex> lock(mtx);
        std::vector<request *> requests;
        img->reader->handle_read(req_offset, iovs, requests);
        num_subreqs = requests.size();

        if (num_subreqs > 0) {
            for (auto const &r : requests)
                r->run(this);
            has_ran = true;
            // try_complete(req_bytes);
        } else {
            // We might sometimes instantly complete; in that case, there will
            // be no notifiers, we must notify ourselves. NOTE lookout for
            // self-deadlock
            has_ran = true;
            lock.unlock();
            notify(nullptr);
        }
    }

    void notify(request *child) override
    {
        free_child(child);

        auto old = num_subreqs.fetch_sub(1, std::memory_order_seq_cst);
        if (old > 1)
            return;

        std::lock_guard<std::mutex> lock(mtx);
        has_notified = true;
        try_complete(req_bytes);
    }
};

class write_request : public aio_request
{
  private:
    std::atomic_int n_req = 0;
    /**
     * Not quite sure we own these iovs; we should transfer ownership to writev
     * and be done with it. The old code had these as pointers, but changed
     * them to be in the vectwor.
     */
    std::vector<smartiov> sub_iovs;

  public:
    write_request(rbd_image *img, size_t offset, smartiov iovs,
                  lsvd_completion *p)
        : aio_request(img, offset, iovs, p)
    {
    }

    void run(request *parent) override
    {
        assert(parent == nullptr);

        img->wcache->get_room(req_bytes / 512);
        img->xlate->wait_for_room();

        sector_t size_sectors = req_bytes / 512;

        // split large requests into 2MB (default) chunks
        sector_t max_sectors = img->cfg.wcache_chunk / 512;
        n_req += div_round_up(req_bytes / 512, max_sectors);
        // TODO: this is horribly ugly

        std::lock_guard<std::mutex> lock(mtx);
        std::vector<request *> requests;
        auto cur_offset = req_offset;

        for (sector_t s_offset = 0; s_offset < size_sectors;
             s_offset += max_sectors) {
            auto _sectors = std::min(size_sectors - s_offset, max_sectors);
            smartiov tmp =
                iovs.slice(s_offset * 512L, s_offset * 512L + _sectors * 512L);
            smartiov _iov(tmp.data(), tmp.size());
            sub_iovs.push_back(_iov);
            auto req = img->wcache->writev(cur_offset / 512, &_iov);
            requests.push_back(req);

            cur_offset += _sectors * 512L;
        }

        for (auto r : requests)
            r->run(this);

        has_ran = true;
        try_complete(0);
    }

    void notify(request *req) override
    {
        free_child(req);

        auto old = n_req.fetch_sub(1, std::memory_order_seq_cst);
        if (old > 1)
            return;

        std::lock_guard<std::mutex> lock(mtx);
        img->wcache->release_room(req_bytes / 512);

        has_notified = true;
        try_complete(0); // TODO shouldn't we return bytes written?
    }
};

extern "C" int rbd_aio_read(rbd_image_t image, uint64_t offset, size_t len,
                            char *buf, rbd_completion_t c)
{
    rbd_image *img = (rbd_image *)image;
    auto p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);

    // auto req = new rbd_aio_req(OP_READ, img, p, offset, REQ_NOWAIT, buf,
    // len);
    auto req = new read_request(img, offset, smartiov(buf, len), p);
    p->delayed_init(img, req);
    p->run();
    return 0;
}

extern "C" int rbd_aio_readv(rbd_image_t image, const iovec *iov, int iovcnt,
                             uint64_t offset, rbd_completion_t c)
{
    rbd_image *img = (rbd_image *)image;
    auto p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);

    // p->req = new rbd_aio_req(OP_READ, img, p, offset, REQ_NOWAIT, iov,
    // iovcnt);
    auto req = new read_request(img, offset, smartiov(iov, iovcnt), p);
    p->delayed_init(img, req);
    p->run();
    return 0;
}

extern "C" int rbd_aio_writev(rbd_image_t image, const struct iovec *iov,
                              int iovcnt, uint64_t offset, rbd_completion_t c)
{
    rbd_image *img = (rbd_image *)image;
    lsvd_completion *p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);

    // p->req = new rbd_aio_req(OP_WRITE, img, p, offset, REQ_NOWAIT, iov,
    // iovcnt);
    auto req = new write_request(img, offset, smartiov(iov, iovcnt), p);
    p->delayed_init(img, req);
    p->run();
    return 0;
}

extern "C" int rbd_aio_write(rbd_image_t image, uint64_t offset, size_t len,
                             const char *buf, rbd_completion_t c)
{
    rbd_image *img = (rbd_image *)image;
    lsvd_completion *p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);

    // p->req =
    //     new rbd_aio_req(OP_WRITE, img, p, offset, REQ_NOWAIT, (char *)buf,
    //     len);
    auto req = new write_request(img, offset, smartiov((char *)buf, len), p);
    p->delayed_init(img, req);
    p->run();

    return 0;
}

/* note that rbd_aio_read handles aligned bounce buffers for us
 */
extern "C" int rbd_read(rbd_image_t image, uint64_t off, size_t len, char *buf)
{
    rbd_image *img = (rbd_image *)image;
    // auto req = new rbd_aio_req(OP_READ, img, NULL, off, REQ_WAIT, buf, len);
    auto req = new read_request(img, off, smartiov(buf, len), NULL);

    req->run(NULL);
    req->wait();
    return 0;
}

extern "C" int rbd_write(rbd_image_t image, uint64_t off, size_t len,
                         const char *buf)
{
    rbd_image *img = (rbd_image *)image;
    // auto req =
    //     new rbd_aio_req(OP_WRITE, img, NULL, off, REQ_WAIT, (char *)buf,
    //     len);

    auto req = new write_request(img, off, smartiov((char *)buf, len), NULL);
    req->run(NULL);
    req->wait();
    return 0;
}

extern "C" int rbd_aio_wait_for_complete(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    assert(p->magic == LSVD_MAGIC);
    p->wait();
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
    // TODO expose shared_read_cache and wlog invalidation
    return 0;
}

/* These RBD functions are unimplemented and return errors
 */
extern "C" int rbd_resize(rbd_image_t image, uint64_t size) { UNIMPLEMENTED(); }

extern "C" int rbd_snap_create(rbd_image_t image, const char *snapname)
{
    UNIMPLEMENTED();
}
extern "C" int rbd_snap_list(rbd_image_t image, rbd_snap_info_t *snaps,
                             int *max_snaps)
{
    UNIMPLEMENTED();
}
extern "C" void rbd_snap_list_end(rbd_snap_info_t *snaps) { UNIMPLEMENTED(); }
extern "C" int rbd_snap_remove(rbd_image_t image, const char *snapname)
{
    UNIMPLEMENTED();
}
extern "C" int rbd_snap_rollback(rbd_image_t image, const char *snapname)
{
    UNIMPLEMENTED();
}

/* */
extern "C" int rbd_diff_iterate2(rbd_image_t image, const char *fromsnapname,
                                 uint64_t ofs, uint64_t len,
                                 uint8_t include_parent, uint8_t whole_object,
                                 int (*cb)(uint64_t, size_t, int, void *),
                                 void *arg)
{
    UNIMPLEMENTED();
}

extern "C" int rbd_encryption_format(rbd_image_t image,
                                     rbd_encryption_format_t format,
                                     rbd_encryption_options_t opts,
                                     size_t opts_size)
{
    UNIMPLEMENTED();
}

extern "C" int rbd_encryption_load(rbd_image_t image,
                                   rbd_encryption_format_t format,
                                   rbd_encryption_options_t opts,
                                   size_t opts_size)
{
    UNIMPLEMENTED();
}

extern "C" int rbd_get_features(rbd_image_t image, uint64_t *features)
{
    UNIMPLEMENTED();
}

extern "C" int rbd_get_flags(rbd_image_t image, uint64_t *flags)
{
    UNIMPLEMENTED();
}

#define ASSERT_FAIL() __builtin_trap()

extern "C" void rbd_image_spec_cleanup(rbd_image_spec_t *image)
{
    UNIMPLEMENTED();
}

extern "C" void rbd_linked_image_spec_cleanup(rbd_linked_image_spec_t *image)
{
    UNIMPLEMENTED();
}

extern "C" int rbd_mirror_image_enable(rbd_image_t image) { UNIMPLEMENTED(); }

extern "C" int rbd_mirror_image_enable2(rbd_image_t image,
                                        rbd_mirror_image_mode_t mode)
{
    UNIMPLEMENTED();
}

extern "C" void
rbd_mirror_image_get_info_cleanup(rbd_mirror_image_info_t *mirror_image_info)
{
    UNIMPLEMENTED();
}

extern "C" void rbd_mirror_image_global_status_cleanup(
    rbd_mirror_image_global_status_t *mirror_image_global_status)
{
    UNIMPLEMENTED();
}

extern "C" int rbd_mirror_peer_site_add(rados_ioctx_t io_ctx, char *uuid,
                                        size_t uuid_max_length,
                                        rbd_mirror_peer_direction_t direction,
                                        const char *site_name,
                                        const char *client_name)
{
    UNIMPLEMENTED();
}

extern "C" int rbd_mirror_peer_site_get_attributes(
    rados_ioctx_t p, const char *uuid, char *keys, size_t *max_key_len,
    char *values, size_t *max_value_len, size_t *key_value_count)
{
    UNIMPLEMENTED();
}

extern "C" int rbd_mirror_peer_site_remove(rados_ioctx_t io_ctx,
                                           const char *uuid)
{
    UNIMPLEMENTED();
}

extern "C" int rbd_mirror_peer_site_set_attributes(rados_ioctx_t p,
                                                   const char *uuid,
                                                   const char *keys,
                                                   const char *values,
                                                   size_t key_value_count)
{
    UNIMPLEMENTED();
}

extern "C" int rbd_mirror_peer_site_set_name(rados_ioctx_t io_ctx,
                                             const char *uuid,
                                             const char *site_name)
{
    UNIMPLEMENTED();
}

extern "C" int rbd_mirror_peer_site_set_client_name(rados_ioctx_t io_ctx,
                                                    const char *uuid,
                                                    const char *client_name)
{
    UNIMPLEMENTED();
}

extern "C" void rbd_pool_stats_create(rbd_pool_stats_t *stats)
{
    UNIMPLEMENTED();
}

extern "C" void rbd_pool_stats_destroy(rbd_pool_stats_t stats)
{
    UNIMPLEMENTED();
}

extern "C" int rbd_pool_stats_option_add_uint64(rbd_pool_stats_t stats,
                                                int stat_option,
                                                uint64_t *stat_val)
{
    UNIMPLEMENTED();
}

extern "C" void rbd_trash_get_cleanup(rbd_trash_image_info_t *info)
{
    UNIMPLEMENTED();
}

extern "C" void rbd_version(int *major, int *minor, int *extra)
{
    *major = 0;
    *minor = 0;
    *extra = 1;
}
