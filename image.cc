#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "journal.h"
#include "image.h"
#include "lsvd_types.h"

extern int init_wcache(int fd, uuid_t &uuid, int n_pages);

struct lsvd_completion;

int rbd_image::poll_io_events(rbd_completion_t *comps, int numcomp)
{
    std::unique_lock lk(m);
    int i;
    for (i = 0; i < numcomp && !completions.empty(); i++) {
        auto p = (lsvd_completion *)completions.front();
        // assert(p->magic == LSVD_MAGIC);
        comps[i] = p;
        completions.pop();
    }
    return i;
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

int rbd_image::image_open(rados_ioctx_t io, const char *name)
{
    this->image_name = name;

    if (cfg.read() < 0)
        throw std::runtime_error("Failed to read config");

    objstore = get_backend(&cfg, io, name);
    auto shared_cache_path = cfg.shared_read_cache_path;
    shared_cache = shared_read_cache::get_instance(
        shared_cache_path, cfg.cache_size / CACHE_CHUNK_SIZE, objstore);

    /* read superblock and initialize translation layer
     */
    xlate = make_translate(objstore, &cfg, &map, &bufmap, &map_lock,
                           &bufmap_lock, shared_cache);
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
