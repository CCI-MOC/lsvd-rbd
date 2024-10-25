#include <chrono>
#include <folly/String.h>
#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <folly/logging/xlog.h>

#include "folly/Unit.h"
#include "folly/executors/GlobalExecutor.h"
#include "folly/futures/Future.h"
#include "image.h"
#include "representation.h"

FOLLY_INIT_LOGGING_CONFIG(".=WARN,src=INFO");

auto img_task() -> Task<void>
{
    auto s3 = ObjStore::connect_to_pool("pone").value();
    auto imgname = "lsvd_misc";

    std::ignore = co_await LsvdImage::remove(s3, imgname);
    (co_await LsvdImage::create(s3, imgname, 4 * GIB)).value();
    auto img = (co_await LsvdImage::mount(s3, imgname, "")).value();

    vec<byte> buf(4096);
    auto iov = smartiov::from_buf(buf);

    io_timing tim;
    auto a = co_await img->write(0, iov, tim);
    raise(SIGTRAP);

    co_await img->unmount();
    co_return;
}

int main(int argc, char **argv)
{
    int fake_argc = 0;
    auto folly_init = folly::Init(&fake_argc, &argv, false);
    ReadCache::init_cache(10 * GIB, 10 * GIB, "/mnt/lsvd/lsvd.rcache");

    auto exe = folly::getGlobalCPUExecutor();
    img_task().scheduleOn(exe).start().wait();
    return 0;
}