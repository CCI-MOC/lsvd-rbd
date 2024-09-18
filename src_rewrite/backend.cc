#include <boost/outcome.hpp>
#include <boost/outcome/success_failure.hpp>
#include <cassert>
#include <folly/experimental/coro/Promise.h>
#include <folly/experimental/coro/Task.h>
#include <rados/buffer.h>
#include <rados/librados.h>
#include <rados/librados.hpp>
#include <system_error>

#include "backend.h"
#include "utils.h"

class Rados : public ObjStore
{
  private:
    librados::IoCtx io;

    struct RadosCbWrap {
        librados::AioCompletion *cb;
        folly::coro::Promise<int> p;

        RadosCbWrap(folly::coro::Promise<int> &&p) : p(std::move(p))
        {
            cb = librados::Rados::aio_create_completion();
            cb->set_complete_callback(
                this, [](rados_completion_t cb, void *arg) {
                    auto req = static_cast<RadosCbWrap *>(arg);
                    req->p.setValue(rados_aio_get_return_value(cb));
                });
        }
        ~RadosCbWrap() { cb->release(); }
    };

    static auto neg_ec_to_result(int iores) -> Result<usize>
    {
        if (iores < 0)
            return outcome::failure(
                std::error_code(-iores, std::system_category()));
        else
            return outcome::success(iores);
    }

    static auto iov_to_bl(smartiov &v)
    {
        using namespace ceph::buffer;
        librados::bufferlist bl;
        for (auto &iov : v.iovs_vec())
            bl.push_back(ptr_node::create(
                create_static(iov.iov_len, (char *)iov.iov_base)));
        return bl;
    }

    static auto iov_to_bl(iovec v)
    {
        return librados::bufferlist::static_from_mem((char *)v.iov_base,
                                                     v.iov_len);
    }

  public:
    Rados(rados_ioctx_t io)
    {
        assert(io != nullptr);
        librados::IoCtx::from_rados_ioctx_t(io, this->io);
    }

    auto get_size(str name) -> ResTask<usize> override
    {
        auto &&[p, f] = folly::coro::makePromiseContract<int>();
        RadosCbWrap cb(std::move(p));

        u64 size;
        time_t mtime;
        auto rc = io.aio_stat(name.toStdString(), cb.cb, &size, &mtime);
        assert(rc == 0);

        rc = co_await std::move(f);
        if (rc < 0)
            co_return std::error_code(-rc, std::system_category());

        co_return size;
    }

    auto exists(str name) -> ResTask<bool> override
    {
        co_return (co_await get_size(name)).has_value();
    }

    auto read(str name, off_t offset, smartiov &v) -> ResTask<usize> override
    {
        auto &&[p, f] = folly::coro::makePromiseContract<int>();
        RadosCbWrap cb(std::move(p));
        auto bl = iov_to_bl(v);
        auto rc =
            io.aio_read(name.toStdString(), cb.cb, &bl, bl.length(), offset);
        assert(rc == 0);
        co_return neg_ec_to_result(co_await std::move(f));
    }

    auto read(str name, off_t offset, iovec v) -> ResTask<usize> override
    {
        auto &&[p, f] = folly::coro::makePromiseContract<int>();
        RadosCbWrap cb(std::move(p));
        auto bl = iov_to_bl(v);
        auto rc =
            io.aio_read(name.toStdString(), cb.cb, &bl, bl.length(), offset);
        assert(rc == 0);
        co_return neg_ec_to_result(co_await std::move(f));
    }

    auto read_all(str name) -> ResTask<vec<byte>> override
    {
        auto size_res = co_await get_size(name);
        if (!size_res.has_value())
            co_return size_res.error();

        auto size = size_res.value();
        vec<byte> buf(size);
        auto iores = co_await read(name, 0, iovec{buf.data(), buf.size()});

        if (!iores.has_value())
            co_return outcome::failure(iores.error());

        if (iores.value() != size)
            co_return outcome::failure(std::errc::io_error);

        if (iores.value() < 0)
            co_return outcome::failure(
                std::error_code(-iores.value(), std::system_category()));

        co_return buf;
    }

    auto write(str name, smartiov &v) -> ResTask<usize> override
    {
        auto &&[p, f] = folly::coro::makePromiseContract<int>();
        RadosCbWrap cb(std::move(p));
        auto bl = iov_to_bl(v);
        auto rc = io.aio_write(name.toStdString(), cb.cb, bl, bl.length(), 0);
        assert(rc == 0);
        co_return neg_ec_to_result(co_await std::move(f));
    }

    auto write(str name, iovec v) -> ResTask<usize> override
    {
        auto &&[p, f] = folly::coro::makePromiseContract<int>();
        RadosCbWrap cb(std::move(p));
        auto bl = iov_to_bl(v);
        auto rc = io.aio_write(name.toStdString(), cb.cb, bl, bl.length(), 0);
        assert(rc == 0);
        co_return neg_ec_to_result(co_await std::move(f));
    }
};