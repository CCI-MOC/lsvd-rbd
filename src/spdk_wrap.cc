#include "spdk_wrap.h"
#include "config.h"
#include "utils.h"

spdk_completion::spdk_completion(rbd_callback_t cb, void *cb_arg)
    : cb(cb), cb_arg(cb_arg)
{
}

spdk_completion::~spdk_completion()
{
    if (req)
        req->release();
}

void spdk_completion::delayed_init(lsvd_rbd *img, request *req)
{
    this->img = img;
    this->req = req;
}

int spdk_completion::get_retval() { return retval; }

void spdk_completion::run()
{
    assert(req != nullptr && img != nullptr);
    // refcount++;
    req->run(nullptr);
}

void spdk_completion::complete(int val)
{
    retval = val;
    if (cb)
        cb((rbd_completion_t)this, cb_arg);
    img->on_request_complete(this);

    done.test_and_set();
    done.notify_all();
    dec_and_free();
}

void spdk_completion::release() { dec_and_free(); }

void spdk_completion::wait()
{
    refcount++;
    done.wait(false, std::memory_order_seq_cst);
    refcount--;
}

inline void spdk_completion::dec_and_free()
{
    auto old = refcount.fetch_sub(1, std::memory_order_seq_cst);
    if (old == 1)
        delete this;
}

lsvd_rbd *lsvd_rbd::open_image(rados_ioctx_t io, std::string name)
{
    try {
        auto cfg = lsvd_config::get_default();
        return new lsvd_rbd(name, io, cfg);
    } catch (std::runtime_error &e) {
        log_error("Failed to open image: {}", e.what());
        return nullptr;
    }
}

void lsvd_rbd::close_image() { delete this; }

lsvd_rbd::lsvd_rbd(std::string name, rados_ioctx_t io, lsvd_config cfg)
    : img(name, io, cfg)
{
}

lsvd_rbd::~lsvd_rbd() {}

spdk_completion *lsvd_rbd::create_completion(rbd_callback_t cb, void *cb_arg)
{
    return new spdk_completion(cb, cb_arg);
}

void lsvd_rbd::release_completion(spdk_completion *c) { c->release(); }

void lsvd_rbd::on_request_complete(spdk_completion *c)
{
    std::unique_lock lk(completions_mtx);
    if (ev.has_value()) {
        completions.push(c);
        ev->notify();
    }
}

int lsvd_rbd::switch_to_poll(event_socket &&ev)
{
    this->ev = std::move(ev);
    return 0;
}

int lsvd_rbd::poll_io_events(spdk_completion **comps, int numcomp)
{
    assert(ev.has_value());

    std::unique_lock lk(completions_mtx);
    int i;
    for (i = 0; i < numcomp && !completions.empty(); i++) {
        auto p = completions.front();
        completions.pop();

        assert(p->magic == LSVD_MAGIC);
        comps[i] = p;
    }
    return i;
}

std::function<void(int)> make_cb(spdk_completion *c)
{
    if (c == nullptr)
        return [](int rv) { return; };
    else
        return [c](int rv) { c->complete(rv); };
}

void init_completion(spdk_completion *c, lsvd_rbd *img, request *req)
{
    if (c != nullptr)
        c->delayed_init(img, req);
}

request *lsvd_rbd::read(size_t offset, smartiov iov, spdk_completion *c)
{
    auto req = img.read(offset, iov, make_cb(c));
    init_completion(c, this, req);
    return req;
}

request *lsvd_rbd::write(size_t offset, smartiov iov, spdk_completion *c)
{
    auto req = img.write(offset, iov, make_cb(c));
    init_completion(c, this, req);
    return req;
}

request *lsvd_rbd::trim(size_t offset, size_t len, spdk_completion *c)
{
    auto req = img.trim(offset, len, make_cb(c));
    init_completion(c, this, req);
    return req;
}

request *lsvd_rbd::flush(spdk_completion *c)
{
    auto req = img.flush(make_cb(c));
    init_completion(c, this, req);
    return req;
}