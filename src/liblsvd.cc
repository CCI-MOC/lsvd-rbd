#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <uuid/uuid.h>

#include "fake_rbd.h"
#include "image.h"
#include "lsvd_debug.h"
#include "lsvd_types.h"
#include "request.h"
#include "smartiov.h"
#include "spdk_wrap.h"
#include "translate.h"
#include "utils.h"

extern "C" int rbd_open(rados_ioctx_t io, const char *name, rbd_image_t *image,
                        const char *snap_name)
{
    auto img = lsvd_rbd::open_image(io, name);
    if (img == nullptr)
        return -1;

    *image = (void *)img;
    log_info("Opened image: {}, size {}", name, img->get_img().size);
    return 0;
}

extern "C" int rbd_close(rbd_image_t image)
{
    lsvd_rbd *img = (lsvd_rbd *)image;
    log_info("Closing image {}", img->get_img().imgname);

    // poor man's race prevention. wait for in-flight requests
    sleep(2);
    img->close_image();

    return 0;
}

extern "C" int rbd_poll_io_events(rbd_image_t image, rbd_completion_t *comps,
                                  int numcomp)
{
    lsvd_rbd *img = (lsvd_rbd *)image;
    return img->poll_io_events(reinterpret_cast<spdk_completion **>(comps),
                               numcomp);
}

extern "C" int rbd_set_image_notification(rbd_image_t image, int fd, int type)
{
    lsvd_rbd *img = (lsvd_rbd *)image;
    assert(type == EVENT_TYPE_EVENTFD);

    event_socket ev(fd, EVENT_TYPE_EVENTFD);
    return img->switch_to_poll(std::move(ev));
}

extern "C" int rbd_aio_create_completion(void *cb_arg,
                                         rbd_callback_t complete_cb,
                                         rbd_completion_t *c)
{
    auto nc = lsvd_rbd::create_completion(complete_cb, cb_arg);
    *c = (rbd_completion_t)nc;
    return 0;
}

extern "C" void rbd_aio_release(rbd_completion_t c)
{
    lsvd_rbd::release_completion((spdk_completion *)c);
}

extern "C" int rbd_discard(rbd_image_t image, uint64_t ofs, uint64_t len)
{
    auto img = (lsvd_rbd *)image;
    auto req = img->trim(ofs, len, nullptr);
    req->run(nullptr);
    req->wait();
    return 0;
}

extern "C" int rbd_aio_discard(rbd_image_t image, uint64_t off, uint64_t len,
                               rbd_completion_t c)
{
    auto p = (spdk_completion *)c;
    auto img = (lsvd_rbd *)image;
    img->trim(off, len, p);
    p->run();
    return 0;
}

extern "C" int rbd_aio_flush(rbd_image_t image, rbd_completion_t c)
{
    auto *p = (spdk_completion *)c;
    auto img = (lsvd_rbd *)image;
    img->flush(p);
    p->run();
    return 0;
}

extern "C" int rbd_flush(rbd_image_t image)
{
    auto img = (lsvd_rbd *)image;
    auto req = img->flush(nullptr);
    req->run(nullptr);
    req->wait();
    req->release();
    return 0;
}

extern "C" void *rbd_aio_get_arg(rbd_completion_t c)
{
    auto *p = (spdk_completion *)c;
    return p->cb_arg;
}

extern "C" ssize_t rbd_aio_get_return_value(rbd_completion_t c)
{
    auto *p = (spdk_completion *)c;
    return p->get_retval();
}

extern "C" int rbd_aio_read(rbd_image_t image, uint64_t offset, size_t len,
                            char *buf, rbd_completion_t c)
{
    lsvd_rbd *img = (lsvd_rbd *)image;
    auto p = (spdk_completion *)c;
    img->read(offset, smartiov(buf, len), p);
    p->run();
    return 0;
}

extern "C" int rbd_aio_readv(rbd_image_t image, const iovec *iov, int iovcnt,
                             uint64_t offset, rbd_completion_t c)
{
    lsvd_rbd *img = (lsvd_rbd *)image;
    auto p = (spdk_completion *)c;
    img->read(offset, smartiov(iov, iovcnt), p);
    p->run();
    return 0;
}

extern "C" int rbd_aio_writev(rbd_image_t image, const struct iovec *iov,
                              int iovcnt, uint64_t offset, rbd_completion_t c)
{
    lsvd_rbd *img = (lsvd_rbd *)image;
    auto *p = (spdk_completion *)c;
    img->write(offset, smartiov(iov, iovcnt), p);
    p->run();
    return 0;
}

extern "C" int rbd_aio_write(rbd_image_t image, uint64_t offset, size_t len,
                             const char *buf, rbd_completion_t c)
{
    lsvd_rbd *img = (lsvd_rbd *)image;
    auto *p = (spdk_completion *)c;
    img->write(offset, smartiov((char *)buf, len), p);
    p->run();
    return 0;
}

/* note that rbd_aio_read handles aligned bounce buffers for us
 */
extern "C" int rbd_read(rbd_image_t image, uint64_t off, size_t len, char *buf)
{
    lsvd_rbd *img = (lsvd_rbd *)image;
    auto req = img->read(off, smartiov(buf, len), NULL);
    req->run(NULL);
    req->wait();
    req->release();
    return 0;
}

extern "C" int rbd_write(rbd_image_t image, uint64_t off, size_t len,
                         const char *buf)
{
    lsvd_rbd *img = (lsvd_rbd *)image;
    auto req = img->write(off, smartiov((char *)buf, len), NULL);
    req->run(NULL);
    req->wait();
    req->release();
    return 0;
}

extern "C" int rbd_aio_wait_for_complete(rbd_completion_t c)
{
    auto *p = (spdk_completion *)c;
    p->wait();
    return 0;
}

/* note that obj_size and order should match chunk size in
 * rbd_aio_req::run_w
 */
extern "C" int rbd_stat(rbd_image_t image, rbd_image_info_t *info,
                        size_t infosize)
{
    lsvd_rbd *img = (lsvd_rbd *)image;
    memset(info, 0, sizeof(*info));
    info->size = img->get_img().size;
    info->obj_size = 1 << 22; // 2^21 bytes
    info->order = 22;         // 2^21 bytes
    info->num_objs = img->get_img().size / info->obj_size;
    return 0;
}

extern "C" int rbd_get_size(rbd_image_t image, uint64_t *size)
{
    lsvd_rbd *img = (lsvd_rbd *)image;
    *size = img->get_img().size;
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
    auto rc = lsvd_image::create_new(name, size, io);
    return result_to_rc(rc);
}

extern "C" int rbd_clone(rados_ioctx_t io, const char *source_img,
                         const char *dest_img)
{
    auto rc = lsvd_image::clone_image(source_img, dest_img, io);
    return result_to_rc(rc);
}

/* remove all objects and cache file.
 * this only removes objects pointed to by the last checkpoint, plus
 * a small range of following ones - there may be dangling objects after
 * removing a deeply corrupted image
 */
extern "C" int rbd_remove(rados_ioctx_t io, const char *name)
{
    auto rc = lsvd_image::delete_image(name, io);
    return result_to_rc(rc);
}

extern "C" void rbd_uuid(rbd_image_t image, uuid_t *uuid)
{
    lsvd_rbd *img = (lsvd_rbd *)image;
    memcpy(uuid, img->get_img().xlate->uuid, sizeof(uuid_t));
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
