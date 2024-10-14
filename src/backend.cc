#include <boost/outcome.hpp>
#include <boost/outcome/success_failure.hpp>
#include <cassert>
#include <folly/experimental/coro/Promise.h>
#include <folly/experimental/coro/Task.h>
#include <rados/buffer.h>
#include <rados/librados.h>
#include <rados/librados.hpp>
#include <system_error>

#include "absl/status/status.h"
#include "backend.h"
#include "folly/logging/xlog.h"
#include "utils.h"

class Rados : public ObjStore
{
  private:
    librados::Rados cluster;
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
            return absl::ErrnoToStatus(-iores, "Failed backend op");
        else
            return iores;
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
    Rados() {}

    ~Rados() override
    {
        io.close();
        cluster.shutdown();
    }

#define FAIL_IF_NEGERR(rc, umsg)                                               \
    do {                                                                       \
        if (rc < 0) [[unlikely]] {                                             \
            auto errc = std::error_code(-rc, std::system_category());          \
            auto msg = fmt::format("{}: {}", umsg, errc.message());            \
            XLOG(ERR, msg);                                                    \
            return absl::ErrnoToStatus(-rc, umsg);                             \
        }                                                                      \
    } while (0)

    static auto connect(fstr pool) -> Result<uptr<Rados>>
    {
        auto s3 = uptr<Rados>(new Rados());

        int ret = 0;
        ret = s3->cluster.init2("client.admin", "ceph", 0);
        FAIL_IF_NEGERR(ret, "Init rados cluster failed");
        ret = s3->cluster.conf_read_file("/etc/ceph/ceph.conf");
        FAIL_IF_NEGERR(ret, "Couldn't read ceph config file");
        ret = s3->cluster.connect();
        FAIL_IF_NEGERR(ret, "Couldn't connect to rados cluster");
        ret = s3->cluster.ioctx_create(pool.c_str(), s3->io);
        FAIL_IF_NEGERR(ret, "Failed to connect to pool");

        return std::move(s3);
    }

    auto get_size(fstr name) -> TaskRes<usize> override
    {
        auto &&[p, f] = folly::coro::makePromiseContract<int>();
        RadosCbWrap cb(std::move(p));

        u64 size;
        time_t mtime;
        auto rc = io.aio_stat(name.toStdString(), cb.cb, &size, &mtime);
        ENSURE(rc == 0);

        rc = co_await std::move(f);
        if (rc < 0)
            co_return absl::ErrnoToStatus(-rc, "Failed to stat object");

        co_return size;
    }

    auto read(fstr name, off_t offset, smartiov &v) -> TaskRes<usize> override
    {
        XLOGF(DBG8, "Reading object '{}'", name);
        auto &&[p, f] = folly::coro::makePromiseContract<int>();
        RadosCbWrap cb(std::move(p));
        auto bl = iov_to_bl(v);
        auto rc =
            io.aio_read(name.toStdString(), cb.cb, &bl, bl.length(), offset);
        ENSURE(rc == 0);
        co_return neg_ec_to_result(co_await std::move(f));
    }

    auto read(fstr name, off_t offset, iovec v) -> TaskRes<usize> override
    {
        XLOGF(DBG8, "Reading object '{}'", name);
        auto &&[p, f] = folly::coro::makePromiseContract<int>();
        RadosCbWrap cb(std::move(p));
        auto bl = iov_to_bl(v);
        auto rc =
            io.aio_read(name.toStdString(), cb.cb, &bl, bl.length(), offset);
        ENSURE(rc == 0);
        co_return neg_ec_to_result(co_await std::move(f));
    }

    auto read_all(fstr name) -> TaskRes<vec<byte>> override
    {
        auto size_res = co_await get_size(name);
        CRET_IF_NOTOK(size_res);

        auto size = size_res.value();
        vec<byte> buf(size);
        auto iores = co_await read(name, 0, iovec{buf.data(), buf.size()});
        CRET_IF_NOTOK(iores);

        if (iores.value() != size)
            co_return absl::InternalError("Failed to read entire object");

        co_return buf;
    }

    auto write(fstr name, smartiov &v) -> TaskRes<usize> override
    {
        XLOGF(DBG8, "Writing object '{}'", name);
        auto &&[p, f] = folly::coro::makePromiseContract<int>();
        RadosCbWrap cb(std::move(p));
        auto bl = iov_to_bl(v);
        auto rc = io.aio_write(name.toStdString(), cb.cb, bl, bl.length(), 0);
        ENSURE(rc == 0);
        co_return neg_ec_to_result(co_await std::move(f));
    }

    auto write(fstr name, iovec v) -> TaskRes<usize> override
    {
        XLOGF(DBG8, "Writing object '{}'", name);
        auto &&[p, f] = folly::coro::makePromiseContract<int>();
        RadosCbWrap cb(std::move(p));
        auto bl = iov_to_bl(v);
        auto rc = io.aio_write(name.toStdString(), cb.cb, bl, bl.length(), 0);
        ENSURE(rc == 0);
        co_return neg_ec_to_result(co_await std::move(f));
    }

    auto remove(fstr name) -> TaskUnit override
    {
        XLOGF(DBG8, "Removing object '{}'", name);
        auto &&[p, f] = folly::coro::makePromiseContract<int>();
        RadosCbWrap cb(std::move(p));
        auto rc = io.aio_remove(name.toStdString(), cb.cb);
        ENSURE(rc == 0);

        auto iores = co_await std::move(f);
        if (iores < 0)
            co_return absl::ErrnoToStatus(-iores, "Failed to remove object");
        else
            co_return folly::Unit();
    }
};

Result<sptr<ObjStore>> ObjStore::connect_to_pool(fstr pool_name)
{
    return Rados::connect(pool_name);
}
