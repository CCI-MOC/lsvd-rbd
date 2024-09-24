#include <folly/String.h>
#include <folly/init/Init.h>

#include "image.h"
#include "representation.h"
#include "src/backend.h"
#include "utils.h"

ResTask<std::string> main_task()
{
    sptr<ObjStore> pool = ObjStore::connect_to_pool("pone").value();
    (co_await LsvdImage::create(pool, "testimg", 1 * 1024 * 1024)).value();
    auto img = (co_await LsvdImage::mount(pool, "testimg", "")).value();

    (co_await img->write(0, smartiov::from_str("hello world"))).value();

    vec<byte> buf(512);
    (co_await img->read(0, smartiov::from_buf(buf))).value();
    auto dump = folly::hexDump(buf.data(), buf.size());

    co_await img->unmount();
    co_return dump;
}

int main(int argc, char **argv)
{
    auto folly_init = folly::Init(&argc, &argv);

    auto sf = main_task().scheduleOn(folly::getGlobalCPUExecutor()).start();
    sf.wait();
    auto res = sf.result();
    if (res.hasException())
        XLOG(ERR, "Error: {}", res.exception().what());
    else if (res->has_error())
        XLOG(ERR, "Error: {}", res->error().message());
    else
        XLOG(INFO, "Success: {}", res->value());
    return 0;
}