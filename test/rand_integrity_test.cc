#include "folly/String.h"
#include "folly/init/Init.h"
#include <array>
#include <cassert>
#include <folly/logging/Init.h>
#include <iostream>
#include <rados/librados.h>
#include <random>

#include "backend.h"
#include "image.h"
#include "utils.h"

const size_t LSVD_BLOCK_SIZE = 4096;
FOLLY_INIT_LOGGING_CONFIG(".=INFO,folly=INFO");

void fill_buf_blocknum(uint64_t block_num, vec<byte> &buf)
{
    assert(sizeof(buf) % 8 == 0);
    uint32_t trunc_num = block_num & 0xffffffff;
    auto *bp = reinterpret_cast<uint32_t *>(buf.data());
    for (size_t i = 0; i < buf.size() / 4; i++) {
        *bp = trunc_num;
        bp++;
    }
}

bool verify_buf(uint64_t block_num, const vec<byte> &buf,
                void (*fill_buf)(uint64_t, vec<byte> &) = fill_buf_blocknum)
{
    vec<byte> expected_buf;
    fill_buf(block_num, expected_buf);

    if (buf != expected_buf) {
        fmt::println("Error reading block {:08x}.\nExpected:\n{}\nActual:\n{}",
                     block_num,
                     folly::hexDump(expected_buf.data(), expected_buf.size()),
                     folly::hexDump(buf.data(), buf.size()));
        return false;
    }

    return true;
}

auto run_test(sptr<ObjStore> s3) -> Task<void>
{
    XLOGF(INFO, "Starting sequential write then readback test");

    XLOGF(INFO, "Removing old image if one exists");
    std::ignore = co_await LsvdImage::remove(s3, "random-test-img");

    // size_t img_size = 1 * 1024 * 1024 * 1024;
    size_t imgsize = 100 * 1024 * 1024;

    // create the image for our own use
    XLOGF(INFO, "Creating image {} of size {}", "random-test-img", imgsize);
    auto res = co_await LsvdImage::create(s3, "random-test-img", imgsize);
    res.value();

    // open the image
    XLOGF(INFO, "Opening image {}", "random-test-img");
    auto img = (co_await LsvdImage::mount(s3, "random-test-img", "")).value();

    // TODO use aio variants to ensure concurrency

    // write out the image
    vec<byte> buf1(LSVD_BLOCK_SIZE * 1);
    vec<byte> buf2(LSVD_BLOCK_SIZE * 2);
    vec<byte> buf3(LSVD_BLOCK_SIZE * 3);
    vec<byte> buf4(LSVD_BLOCK_SIZE * 4);
    vec<byte> buf5(LSVD_BLOCK_SIZE * 5);
    vec<byte> buf6(LSVD_BLOCK_SIZE * 6);
    vec<byte> buf7(LSVD_BLOCK_SIZE * 7);
    vec<byte> buf8(LSVD_BLOCK_SIZE * 8);
    vec<vec<byte>> bufs = {buf1, buf2, buf3, buf4, buf5, buf6, buf7, buf8};

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<> addr_dist(0, imgsize / LSVD_BLOCK_SIZE - 9);
    std::uniform_int_distribution<> size_dist(0, 7);

    auto iters = 1'000'000;
    for (auto i = 0; i < iters; i++) {
        auto addr = addr_dist(rng);
        auto size = size_dist(rng);
        auto buf = bufs.at(size);

        auto iov = smartiov::from_buf(buf);
        res = co_await img->write_and_verify(addr * LSVD_BLOCK_SIZE, iov);
        res.value();

        if (i % 1000 == 0)
            std::cout << "." << std::flush;
    }

    fmt::print("\nVerified {} blocks\n", imgsize / LSVD_BLOCK_SIZE);

    // step 3: close the image
    co_await img->unmount();

    // step 4: delete the image
    res = co_await LsvdImage::remove(s3, "random-test-img");
    res.value();
}

int main(int argc, char *argv[])
{
    auto folly_init = folly::Init(&argc, &argv);
    const usize GIB = 1024 * 1024 * 1024;
    ReadCache::init_cache(4 * GIB, 4 * GIB, "/tmp/lsvd.rcache");

    std::string pool_name = "pone";
    auto s3 = ObjStore::connect_to_pool(pool_name).value();

    auto ret =
        run_test(s3).scheduleOn(folly::getGlobalCPUExecutor()).start().wait();
    auto res = ret.result();
    if (res.hasException())
        XLOGF(FATAL, "Error:\n{}", res.exception().what());
    return 0;
}
