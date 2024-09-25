#include <folly/String.h>
#include <folly/init/Init.h>

#include "backend.h"
#include "image.h"
#include "representation.h"
#include "utils.h"

ResTask<std::string> main_task()
{
    sptr<ObjStore> pool = ObjStore::connect_to_pool("pone").value();
    (co_await LsvdImage::create(pool, "testimg", 1 * 1024 * 1024)).value();
    auto img = (co_await LsvdImage::mount(pool, "testimg", "")).value();

    vec<byte> bufin(4096);
    auto instr = "hello world";
    std::memcpy(bufin.data(), instr, std::strlen(instr));
    (co_await img->write(0, smartiov::from_buf(bufin))).value();

    vec<byte> buf(4096);
    (co_await img->read(0, smartiov::from_buf(buf))).value();
    auto dump = folly::hexDump(buf.data(), 512);

    co_await img->unmount();
    co_return dump;
}

const usize GIB = 1024 * 1024 * 1024;

int main(int argc, char **argv)
{
    auto folly_init = folly::Init(&argc, &argv);
    ReadCache::init_cache(4 * GIB, 4 * GIB, "/tmp/lsvd.rcache");

    auto sf = main_task().scheduleOn(folly::getGlobalCPUExecutor()).start();
    sf.wait();
    auto res = sf.result();
    if (res.hasException())
        XLOGF(ERR, "Error:\n{}", res.exception().what());
    else if (res->has_error())
        XLOGF(ERR, "Error:\n{}", res->error().message());
    else
        XLOGF(INFO, "Success:\n{}", res->value());
    return 0;
}