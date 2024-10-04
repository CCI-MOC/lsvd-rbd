#include <folly/String.h>
#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <folly/logging/xlog.h>

#include "folly/futures/Future.h"
#include "image.h"
#include "representation.h"

FOLLY_INIT_LOGGING_CONFIG(".=WARN,src=DBG9");
const usize GIB = 1024 * 1024 * 1024;

auto random_task() -> Task<void>
{
    auto s3 = ObjStore::connect_to_pool("pone").value();
    auto imgname = "lsvddev";

    XLOGF(INFO, "Opening image {}", imgname);
    auto img = (co_await LsvdImage::mount(s3, imgname, "")).value();

    vec<byte> buf(4096);
    auto iov = smartiov::from_buf(buf);

    std::ignore = co_await img->read(0, iov);
    std::ignore = co_await img->read(4096, iov);
    std::ignore = co_await img->read(12288, iov);

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