#include <folly/String.h>
#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <folly/logging/xlog.h>

#include "folly/futures/Future.h"
#include "image.h"
#include "representation.h"

FOLLY_INIT_LOGGING_CONFIG(".=WARN,src=DBG7");
const usize GIB = 1024 * 1024 * 1024;

auto random_task() -> Task<void>
{
    auto s3 = ObjStore::connect_to_pool("pone").value();
    auto imgname = "lsvddev";

    // std::ignore = co_await LsvdImage::remove(s3, imgname);
    // (co_await LsvdImage::create(s3, imgname, 4 * GIB)).value();

    XLOGF(INFO, "Opening image {}", imgname);
    auto img = (co_await LsvdImage::mount(s3, imgname, "")).value();

    vec<byte> buf(4096);
    auto iov = smartiov::from_buf(buf);

    // for (usize off = 0; off < img->get_size(); off += 4096)
    //     std::ignore = co_await img->read(off, iov);

    // buf.resize(2 * 4096);
    // auto iov2 = smartiov::from_buf(buf);
    // for (usize off = 0; off + (2 * 4096) <= img->get_size(); off += 4096)
    //     std::ignore = co_await img->read(off, iov2);

    // buf.resize(3 * 4096);
    // auto iov3 = smartiov::from_buf(buf);
    // for (usize off = 0; off + (3 * 4096) <= img->get_size(); off += 4096)
    //     std::ignore = co_await img->read(off, iov3);

    buf.resize(4 * 4096);
    auto iov4 = smartiov::from_buf(buf);
    for (usize off = 0; off + (4 * 4096) <= img->get_size(); off += 4096)
        std::ignore = co_await img->read(off, iov4);

    co_await img->unmount();
    co_return;
}

int main(int argc, char **argv)
{
    int fake_argc = 0;
    auto folly_init = folly::Init(&fake_argc, &argv, false);
    ReadCache::init_cache(4 * GIB, 4 * GIB, "/tmp/lsvd.rcache");

    auto exe = folly::getGlobalCPUExecutor();
    auto s = random_task().scheduleOn(exe).start().wait();

    return 0;
}