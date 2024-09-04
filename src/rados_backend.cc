#include <boost/outcome/success_failure.hpp>
#include <rados/librados.h>
#include <rados/librados.hpp>

#include "backend.h"
#include "request.h"
#include "smartiov.h"
#include "utils.h"

class rados_io_req : public self_refcount_request
{
  protected:
    std::string name;
    size_t off;
    librados::IoCtx ctx;
    librados::AioCompletion *comp;
    request *parent;

  public:
    int ret = 0;

    rados_io_req(std::string name, librados::IoCtx ctx) : name(name), ctx(ctx)
    {
        comp = librados::Rados::aio_create_completion(
            this, rados_io_req::rados_callback);
    }

    ~rados_io_req() { comp->release(); }

    static void rados_callback(rados_completion_t cb, void *arg)
    {
        rados_io_req *req = (rados_io_req *)arg;

        // TODO handle failure
        req->ret = rados_aio_get_return_value(cb);
        req->notify(nullptr);
        req->notify_parent(req->parent);
        req->dec_and_free();
    }

    void wait() override
    {
        refcount++;
        comp->wait_for_complete_and_cb();
        dec_and_free();
    }

    void notify(request *parent) override {}

    Result<int> run_getres_and_free()
    {
        run(nullptr);
        wait();
        auto r = this->ret;
        dec_and_free();
        if (r < 0)
            return errcode_to_result<int>(-r);
        else
            return r;
    }
};

class rados_read_req : public rados_io_req
{
    smartiov iov;
    size_t off;
    librados::bufferlist bl;

  public:
    rados_read_req(librados::IoCtx ctx, std::string name, size_t off,
                   smartiov &iov)
        : rados_io_req(name, ctx), iov(iov), off(off)
    {
    }

    void run(request *parent) override
    {
        this->parent = parent;
        auto rv = ctx.aio_read(name, comp, &bl, iov.bytes(), off);
        THROW_ERRNO_ON(rv < 0, -rv, "Failed to start RADOS aio read");
    }

    void notify(request *parent) override
    {
        if (ret > 0)
            iov.copy_in(bl.c_str(), bl.length());
    }
};

class rados_write_req : public rados_io_req
{
    librados::bufferlist bl;

  public:
    rados_write_req(librados::IoCtx ctx, std::string name, smartiov &iov)
        : rados_io_req(name, ctx)
    {
        auto [v, c] = iov.c_iov();
        for (auto i = 0; i < c; i++)
            bl.append((char *)v[i].iov_base, v[i].iov_len);
    }

    void run(request *parent) override
    {
        this->parent = parent;
        auto rv = ctx.aio_write(name, comp, bl, bl.length(), 0);
        THROW_ERRNO_ON(rv < 0, -rv, "Failed to start RADOS aio write");
    }
};

class rados_delete_req : public rados_io_req
{
  public:
    rados_delete_req(librados::IoCtx ctx, std::string name)
        : rados_io_req(name, ctx)
    {
    }

    void run(request *parent) override
    {
        this->parent = parent;
        auto rv = ctx.aio_remove(name, comp);
        THROW_ERRNO_ON(rv < 0, -rv, "Failed to start RADOS aio remove");
    }
};

class rados_backend : public backend
{
    librados::IoCtx ctx;

  public:
    rados_backend(rados_ioctx_t ctx_)
    {
        check_cond(ctx_ == nullptr, "io_ctx is null");
        librados::IoCtx::from_rados_ioctx_t(ctx_, this->ctx);
    }

    ~rados_backend() override {}

    Result<int> write(std::string name, smartiov &iov) override
    {
        auto req = dynamic_cast<rados_write_req *>(aio_write(name, iov));
        return req->run_getres_and_free();
    }

    Result<int> read(std::string name, off_t offset, smartiov &iov) override
    {
        auto req = dynamic_cast<rados_read_req *>(aio_read(name, offset, iov));
        return req->run_getres_and_free();
    }

    Result<void> delete_obj(std::string name) override
    {
        auto req = dynamic_cast<rados_delete_req *>(aio_delete(name));
        auto rc = BOOST_OUTCOME_TRYX(req->run_getres_and_free());
        assert(rc == 0);
        return outcome::success();
    }

    request *aio_write(std::string name, smartiov &iov) override
    {
        return new rados_write_req(ctx, name, iov);
    }

    request *aio_read(std::string name, off_t offset, smartiov &iov) override
    {
        return new rados_read_req(ctx, name, offset, iov);
    }

    request *aio_delete(std::string name) override
    {
        return new rados_delete_req(ctx, name);
    }

    bool exists(std::string name) override
    {
        return ctx.stat(name, nullptr, nullptr) == 0;
    }

    Result<u64> get_size(std::string name) override
    {
        u64 size;
        time_t mtime;
        int rv = ctx.stat(name, &size, &mtime);
        FAILURE_IF_NEGATIVE(rv);
        return size;
    }

    Result<vec<byte>> read_whole_obj(std::string name) override
    {
        auto size = BOOST_OUTCOME_TRYX(get_size(name));

        vec<byte> buf(size);
        smartiov iov((char *)buf.data(), buf.size());
        auto r = BOOST_OUTCOME_TRYX(read(name, 0, iov));
        if (std::cmp_less(r, size))
            return outcome::failure(LsvdError::MissingData);
        return buf;
    }
};

std::shared_ptr<backend> make_rados_backend(rados_ioctx_t io)
{
    return std::make_shared<rados_backend>(io);
}

Result<rados_ioctx_t> connect_to_pool(str pool_name)
{
    rados_t cluster;
    auto err = rados_create2(&cluster, "ceph", "client.admin", 0);
    PR_FAILURE_IF_NEGATIVE(err, "Failed to create cluster handle");

    err = rados_conf_read_file(cluster, "/etc/ceph/ceph.conf");
    PR_FAILURE_IF_NEGATIVE(err, "Failed to read config file");

    err = rados_connect(cluster);
    PR_FAILURE_IF_NEGATIVE(err, "Failed to connect to cluster");

    rados_ioctx_t io_ctx;
    err = rados_ioctx_create(cluster, pool_name.c_str(), &io_ctx);
    PR_FAILURE_IF_NEGATIVE(err, "Failed to connect to pool {}", pool_name);

    return io_ctx;
}
