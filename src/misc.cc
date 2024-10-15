#include "cppcoro/task.hpp"
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

FOLLY_INIT_LOGGING_CONFIG(".=WARN,src=DBG9");
const usize GIB = 1024 * 1024 * 1024;

auto img_task() -> Task<void>
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

    // buf.resize(4 * 4096);
    // auto iov4 = smartiov::from_buf(buf);
    // for (usize off = 0; off + (4 * 4096) <= img->get_size(); off += 4096)
    //     std::ignore = co_await img->read(off, iov4);

    std::ignore = co_await img->write(0, iov);
    std::ignore = co_await img->write(8192, iov);
    std::ignore = co_await img->read(4096, iov);

    std::ignore = co_await img->flush();
    std::ignore = co_await img->write(8192, iov);
    std::ignore = co_await img->write(8192, iov);
    std::ignore = co_await img->write(8192, iov);

    std::ignore = co_await img->flush();
    std::ignore = co_await img->write(8192, iov);
    std::ignore = co_await img->write(8192, iov);
    std::ignore = co_await img->write(8192, iov);

    std::ignore = co_await img->flush();

    co_await img->unmount();
    co_return;
}

using tp = std::chrono::time_point<std::chrono::high_resolution_clock>;

std::atomic<u64> total_lat_us{0};
vec<byte> buf(4096);

auto imgread_task(tp start, LsvdImage &img, vec<byte> &buf) -> Task<u64>
{
    std::ignore = co_await img.read(0, smartiov::from_buf(buf));
    auto end = std::chrono::high_resolution_clock::now();
    auto lat =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    co_return lat.count();
}

auto noop_task(tp start, int num) -> Task<u64>
{
    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(now - start)
            .count();

    total_lat_us += elapsed;
    co_return elapsed;
}

auto qd_test_th(LsvdImage &img)
{
    XLOGF(INFO, "Starting thread");
    auto exe = folly::getGlobalCPUExecutor();

    u64 total = 0, total_e2e = 0;
    vec<byte> buf(4096);
    for (int i = 0; i < 1'000'000; i++) {
        auto now = std::chrono::high_resolution_clock::now();
        total +=
            imgread_task(now, img, buf).scheduleOn(exe).start().wait().value();
        auto end = std::chrono::high_resolution_clock::now();
        auto e2e = std::chrono::duration_cast<std::chrono::microseconds>(end - now);
        total_e2e += e2e.count();
    }

    XLOGF(INFO, "Total {}, per iter {}", total, total / 1'000'000);
    XLOGF(INFO, "Total e2e {}, per iter {}", total_e2e, total_e2e / 1'000'000);
}

auto folly_qd_latency_test()
{
    auto exe = folly::getGlobalCPUExecutor();
    auto s3 = ObjStore::connect_to_pool("pone").value();
    auto imgname = "lsvddev";

    XLOGF(INFO, "Opening image {}", imgname);
    auto img = LsvdImage::mount(s3, imgname, "")
                   .scheduleOn(exe)
                   .start()
                   .wait()
                   .value()
                   .value();

    vec<std::jthread> ths;
    for (auto i = 0; i < 16; i++)
        ths.push_back(std::jthread(qd_test_th, std::ref(*img)));

    for (auto &th : ths)
        th.join();

    XLOGF(INFO, "Total elapsed {}", total_lat_us.load());
    XLOGF(INFO, "Total per iter {}", total_lat_us.load() / (16 * 1'000'000));
}

int main(int argc, char **argv)
{
    int fake_argc = 0;
    auto folly_init = folly::Init(&fake_argc, &argv, false);
    ReadCache::init_cache(10 * GIB, 10 * GIB, "/tmp/lsvd.rcache");

    auto exe = folly::getGlobalCPUExecutor();
    auto s = img_task().scheduleOn(exe).start().wait();
    // folly_qd_latency_test();

    return 0;
}