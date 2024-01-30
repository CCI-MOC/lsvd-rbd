#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "image.h"
#include "journal.h"
#include "lsvd_types.h"
#include "spdk_wrap.h"

extern int init_wcache(int fd, uuid_t &uuid, int n_pages);

lsvd_image::~lsvd_image()
{
    wcache->flush();
    wcache->do_write_checkpoint();
    if (!cfg.no_gc)
        xlate->stop_gc();
    xlate->checkpoint();

    close(write_fd);
}

int lsvd_image::try_open(std::string name, rados_ioctx_t io)
{
    this->image_name = name;

    if (cfg.read() < 0)
        throw std::runtime_error("Failed to read config");

    objstore = get_backend(&cfg, io, name.c_str());
    shared_cache =
        get_read_cache_instance(cfg.rcache_dir, cfg.cache_size, objstore);

    /* read superblock and initialize translation layer
     */
    xlate = make_translate(objstore, &cfg, &map, &bufmap, &map_lock,
                           &bufmap_lock, shared_cache);
    size = xlate->init(name.c_str(), true);
    check_cond(size < 0, "Failed to initialize translation layer err={}", size);

    /* figure out cache file name, create it if necessary
     */

    /*
     * TODO: Open 2 files. One for wcache and one for reader
     */
    std::string wcache_name =
        cfg.cache_filename(xlate->uuid, name.c_str(), LSVD_CFG_WRITE);

    if (access(wcache_name.c_str(), R_OK | W_OK) < 0) {
        log_info("Creating write cache file {}", wcache_name);
        int cache_pages = cfg.wlog_size / 4096;

        int fd = open(wcache_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
        check_ret_errno(fd, "Can't open wcache file");

        if (init_wcache(fd, xlate->uuid, cache_pages) < 0)
            throw std::runtime_error("Failed to initialize write cache");
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

    wcache = make_write_cache(0, write_fd, xlate.get(), &cfg);
    reader = make_reader(0, xlate.get(), &cfg, &map, &bufmap, &map_lock,
                         &bufmap_lock, objstore, shared_cache);
    free(jws);

    if (!cfg.no_gc)
        xlate->start_gc();
    return 0;
}

/**
 * This is the base for aio read and write requests. It's copied from
 * the old rbd_aio_req omniclass, with the read and write paths split out and
 * the common completion handling moved here.
 */
class lsvd_image::aio_request : public self_refcount_request
{
  private:
    spdk_completion *p;
    std::atomic_flag done = false;

  protected:
    lsvd_image *img = nullptr;
    smartiov iovs;

    size_t req_offset;
    size_t req_bytes;

    aio_request(lsvd_image *img, size_t offset, smartiov iovs,
                spdk_completion *c)
        : p(c), img(img), iovs(iovs), req_offset(offset)
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

  public:
    inline virtual void wait() override
    {
        refcount++;
        done.wait(false, std::memory_order_seq_cst);
        dec_and_free();
    }
};

class lsvd_image::read_request : public lsvd_image::aio_request
{
  private:
    std::atomic_int num_subreqs = 0;

  public:
    read_request(lsvd_image *img, size_t offset, smartiov iovs,
                 spdk_completion *c)
        : aio_request(img, offset, iovs, c)
    {
    }

    void run(request *parent) override
    {
        assert(parent == nullptr);

        std::vector<request *> requests;
        img->reader->handle_read(req_offset, &iovs, requests);
        num_subreqs = requests.size();

        // We might sometimes instantly complete; in that case, there will be
        // no notifiers, we must notify ourselves.
        // NOTE lookout for self-deadlock
        if (num_subreqs == 0) {
            notify(nullptr);
        } else {
            for (auto const &r : requests)
                r->run(this);
        }
    }

    void notify(request *child) override
    {
        free_child(child);

        auto old = num_subreqs.fetch_sub(1, std::memory_order_seq_cst);
        if (old > 1)
            return;

        complete_request(req_bytes);
    }
};

class lsvd_image::write_request : public lsvd_image::aio_request
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
    write_request(lsvd_image *img, size_t offset, smartiov iovs,
                  spdk_completion *c)
        : aio_request(img, offset, iovs, c)
    {
    }

    void notify(request *req) override
    {
        free_child(req);
        auto old = n_req.fetch_sub(1, std::memory_order_seq_cst);
        if (old > 1)
            return;

        img->wcache->release_room(req_bytes / 512);
        complete_request(0); // TODO shouldn't we return bytes written?
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
    }
};

class trim_request : public lsvd_image::aio_request
{
  public:
    trim_request(lsvd_image *img, size_t offset, size_t len, spdk_completion *c)
        : aio_request(img, offset, smartiov(), c)
    {
        req_bytes = len;
    }

    void run(request *parent) override
    {
        assert(parent == nullptr);
        img->xlate->trim(req_offset, req_bytes);
        complete_request(0);
    }

    void notify(request *req) override { UNIMPLEMENTED(); }
};

class flush_request : public lsvd_image::aio_request
{
  public:
    flush_request(lsvd_image *img, spdk_completion *c)
        : aio_request(img, 0, smartiov(), c)
    {
    }

    void run(request *parent) override
    {
        assert(parent == nullptr);
        img->xlate->flush();
        complete_request(0);
    }

    void notify(request *req) override { UNIMPLEMENTED(); }
};

request *lsvd_image::read(size_t offset, smartiov iov, spdk_completion *c)
{
    return new read_request(this, offset, iov, c);
}

request *lsvd_image::write(size_t offset, smartiov iov, spdk_completion *c)
{
    return new write_request(this, offset, iov, c);
}

request *lsvd_image::trim(size_t offset, size_t len, spdk_completion *c)
{
    return new trim_request(this, offset, len, c);
}

request *lsvd_image::flush(spdk_completion *c)
{
    return new flush_request(this, c);
}
