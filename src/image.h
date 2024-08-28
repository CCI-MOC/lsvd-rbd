#pragma once

#include <functional>
#include <map>
#include <thread>

#include "backend.h"
#include "config.h"
#include "extent.h"
#include "objects.h"
#include "shared_read_cache.h"
#include "translate.h"
#include "write_cache.h"

/**
 * Core LSVD image class. An LSVD image supports 4 operations: read, write,
 * trim, and flush. All are async to prevent function colour issues.
 *
 * Currently, a lot of core image functionality is in the `translate` class.
 * The separation between what is here and what is there is not clear, and the
 * two classes really should be consolidated, and the GC function splitted out
 * into its own class.
 *
 * For now, all the core information about the image is owned by this class,
 * and `translate` only takes references to it. Most of the translate code was
 * from long ago, written by people who are no longer around. It's written like
 * a C program, and the ownership structure of most resources is unclear, with
 * sketchy concurrency control and C++ style.
 *
 * Eventually we'll have to rewrite the core translation class to clarify
 * resource ownership and to overhaul the disastrous locking situation, but
 * that's only a dream for now
 */
class lsvd_image
{
  private:
    // no copying or moving
    lsvd_image(const lsvd_image &) = delete;
    lsvd_image operator=(const lsvd_image &) = delete;
    lsvd_image(const lsvd_image &&) = delete;
    lsvd_image operator=(const lsvd_image &&) = delete;

    // Log recovery
    result<void> read_superblock();
    result<void> read_from_checkpoint(seqnum_t ckpt_id);
    result<void> apply_log(seqnum_t seq);

    seqnum_t roll_forward_from_last_checkpoint();
    void recover_from_wlog();

  public:
    lsvd_image(std::string name, rados_ioctx_t io, lsvd_config cfg);
    ~lsvd_image();

    std::string imgname;
    uuid_t uuid;
    usize size; // bytes
    lsvd_config cfg;

    rados_ioctx_t io;

    vec<clone_base> clones;    // Base images on which we're built
    vec<seqnum_t> checkpoints; // Checkpoints
    std::map<seqnum_t, data_obj_info> obj_info;

    // LBA -> object id, object offset
    extmap::objmap objmap;
    std::shared_mutex map_lock;

    // LBA -> in-memory, higher priority than the object map
    // We potentially want to consolidate this with the object map, but
    // this is currently not possible as we don't assign object IDs until the
    // objects are queued for dispatch to the backend
    // NOTE potential fix: use magic IDs that are in-memory
    extmap::bufmap bufmap;
    std::mutex bufmap_lock;

    // TODO only 1 read at a time? probably too coarse
    std::mutex reader_lock;

    std::map<int, char *> buffers;

    std::shared_ptr<backend> objstore;
    std::shared_ptr<read_cache> rcache;
    std::unique_ptr<write_cache> wlog;
    std::unique_ptr<translate> xlate;

    int refcount = 0;

    std::thread dbg;
    bool done = false;

    class aio_request;
    class trivial_request;
    class read_request;
    class write_request;
    request *read(size_t offset, smartiov iov, std::function<void(int)> cb);
    request *write(size_t offset, smartiov iov, std::function<void(int)> cb);
    request *trim(size_t offset, size_t len, std::function<void(int)> cb);
    request *flush(std::function<void(int)> cb);

    // Image management
    static result<void> create_new(str name, usize size, rados_ioctx_t io);
    static result<void> get_uuid(str name, uuid_t &uuid, rados_ioctx_t io);
    static result<void> delete_image(str name, rados_ioctx_t io);
    static result<void> clone_image(str oldname, str newname, rados_ioctx_t io);

  private:
    void handle_reads(usize offset, smartiov iovs, vec<request *> &requests);
};
