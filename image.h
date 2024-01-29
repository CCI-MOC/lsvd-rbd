#pragma once

#include <queue>

#include "backend.h"
#include "config.h"
#include "extent.h"
#include "fake_rbd.h"
#include "img_reader.h"
#include "lsvd_types.h"
#include "objects.h"
#include "shared_read_cache.h"
#include "translate.h"
#include "write_cache.h"

struct event_socket {
    int socket;
    int type;

  public:
    event_socket() : socket(-1), type(0) {}
    bool is_valid() const { return socket != -1; }
    int init(int fd, int t)
    {
        socket = fd;
        type = t;
        return 0;
    }
    int notify()
    {
        int rv;
        switch (type) {
        case EVENT_TYPE_PIPE: {
            char buf[1] = {'i'}; // why 'i'???
            rv = write(socket, buf, 1);
            rv = (rv < 0) ? -errno : 0;
            break;
        }
        case EVENT_TYPE_EVENTFD: {
            uint64_t value = 1;
            rv = write(socket, &value, sizeof(value));
            rv = (rv < 0) ? -errno : 0;
            break;
        }
        default:
            rv = -1;
        }
        return rv;
    }
};

struct rbd_image {
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

    std::map<int, char *> buffers;

    std::shared_ptr<backend> objstore;
    std::shared_ptr<read_cache> shared_cache;
    translate *xlate;
    write_cache *wcache;
    img_reader *reader;
    int write_fd; /* write cache file */

    std::mutex m; /* protects completions */
    event_socket ev;
    std::queue<rbd_completion_t> completions;

    int refcount = 0;

    std::thread dbg;
    bool done = false;

    rbd_image() {}
    ~rbd_image();

    int image_open(rados_ioctx_t io, const char *name);
    int image_close(void);
    int poll_io_events(rbd_completion_t *comps, int numcomp);
    void notify(rbd_completion_t c);
};

using ckpt_id = uint32_t;

inline std::vector<ckpt_id> deserialise_checkpoint_ids(char *buf)
{
    auto obj = (obj_hdr *)buf;
    auto super = (super_hdr *)(buf + 1);

    assert(obj->magic == LSVD_MAGIC);
    assert(obj->type == LSVD_SUPER);

    std::vector<ckpt_id> checkpoint_ids;
    decode_offset_len<ckpt_id>(buf, super->ckpts_offset, super->ckpts_len,
                               checkpoint_ids);
    return checkpoint_ids;
}
