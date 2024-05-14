#pragma once

#include <functional>
#include <map>
#include <thread>

#include "backend.h"
#include "config.h"
#include "extent.h"
#include "shared_read_cache.h"
#include "translate.h"
#include "write_cache.h"

/**
 * Core LSVD image class. An LSVD image supports 4 operations: read, write,
 * trim, and flush. All are async to prevent function colour issues.
 */
class lsvd_image
{
  private:
    // no copying
    lsvd_image(const lsvd_image &) = delete;
    lsvd_image operator=(const lsvd_image &) = delete;

  public:
    std::string image_name;

    lsvd_config cfg;
    ssize_t size; // bytes

    // LBA -> object id, object offset
    extmap::objmap map;
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
    std::shared_ptr<read_cache> shared_cache;
    std::unique_ptr<write_cache> wcache;
    std::unique_ptr<translate> xlate;
    int write_fd; /* write cache file */

    int refcount = 0;

    std::thread dbg;
    bool done = false;

    lsvd_image() {}
    ~lsvd_image();

    int try_open(std::string name, rados_ioctx_t io);
    static uptr<lsvd_image> open_image(std::string name, rados_ioctx_t io);

    class aio_request;
    class trivial_request;
    class read_request;
    class write_request;
    request *read(size_t offset, smartiov iov, std::function<void(int)> cb);
    request *write(size_t offset, smartiov iov, std::function<void(int)> cb);
    request *trim(size_t offset, size_t len, std::function<void(int)> cb);
    request *flush(std::function<void(int)> cb);

  private:
    void handle_reads(size_t offset, smartiov iovs,
                      std::vector<request *> &requests);
};
