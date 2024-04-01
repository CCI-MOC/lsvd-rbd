#pragma once

#include "image.h"

class lsvd_spdk;
class lsvd_image;

class spdk_completion
{
  public:
    const int magic = LSVD_MAGIC;

  private:
    std::atomic_int refcount = 2;
    std::atomic_flag done = ATOMIC_FLAG_INIT;

    rbd_callback_t cb;

    lsvd_spdk *img = nullptr;
    int retval = -1;

    request *req = nullptr;
    void dec_and_free();

  public:
    void *cb_arg;

    spdk_completion(rbd_callback_t cb, void *cb_arg);
    ~spdk_completion();

    void delayed_init(lsvd_spdk *img, request *req);

    void run();
    void wait();
    void complete(int val);

    void release();
    int get_retval();
};

struct event_socket {
    int socket;
    int type;

  public:
    event_socket(int fd, int t) : socket(fd), type(t) {}

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

/**
 * Wrapper around lsvd_image for SPDK's RBD api
 */
class lsvd_spdk
{
  public:
    static lsvd_spdk *open_image(rados_ioctx_t io, std::string name);
    void close_image();

  private:
    lsvd_image img;

    std::queue<spdk_completion *> completions;
    std::mutex completions_mtx;
    std::optional<event_socket> ev;

  public:
    static spdk_completion *create_completion(rbd_callback_t cb, void *cb_arg);
    static void release_completion(spdk_completion *c);

    int switch_to_poll(event_socket &&ev);
    int poll_io_events(spdk_completion **comps, int numcomp);

    request *read(size_t offset, smartiov iov, spdk_completion *c);
    request *write(size_t offset, smartiov iov, spdk_completion *c);
    request *trim(size_t offset, size_t len, spdk_completion *c);
    request *flush(spdk_completion *c);

    void on_request_complete(spdk_completion *req);

    inline lsvd_image &get_img() { return img; }
};