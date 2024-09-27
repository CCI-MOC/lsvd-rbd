#include "folly/String.h"
#include "folly/init/Init.h"
#include <array>
#include <cassert>
#include <folly/logging/Init.h>
#include <iostream>
#include <rados/librados.h>

#include "backend.h"
#include "image.h"
#include "utils.h"

FOLLY_INIT_LOGGING_CONFIG(".=INFO,folly=INFO");

const size_t LSVD_BLOCK_SIZE = 4096;
using comp_buf = std::array<uint8_t, LSVD_BLOCK_SIZE>;

// https://stackoverflow.com/a/51061314/21281619
// There's a memory leak that doesn't affect the functionality.
// Temporarily disable memory leak checks. Remove the following code
// block once leak is fixed.
#ifdef __cplusplus
extern "C"
#endif
    const char *
    __asan_default_options()
{
    return "detect_leaks=0";
}

/*
void fill_buf_rand_bytes(uint64_t block_num, comp_buf &buf)
{
    static const uint64_t MAGIC_SEED = 0xdeadbeefff00cab3ull;
    auto seed = block_num ^ MAGIC_SEED;

    std::mt19937 gen;
    gen.seed(seed);
    gen.discard(10'000);

    std::uniform_int_distribution<uint8_t> dis(0, 255);

    for (size_t i = 0; i < buf.size(); i++) {
        buf[i] = dis(gen);
    }
}
*/

void fill_buf_blocknum(uint64_t block_num, comp_buf &buf)
{
    assert(sizeof(buf) % 8 == 0);
    uint32_t trunc_num = block_num & 0xffffffff;
    auto *bp = reinterpret_cast<uint32_t *>(buf.data());
    for (size_t i = 0; i < buf.size() / 4; i++) {
        *bp = trunc_num;
        bp++;
    }
}

bool verify_buf(uint64_t block_num, const comp_buf &buf,
                void (*fill_buf)(uint64_t, comp_buf &) = fill_buf_blocknum)
{
    comp_buf expected_buf;
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
    size_t img_size = 100 * 1024 * 1024;

    // create the image for our own use
    XLOGF(INFO, "Creating image {} of size {}", "random-test-img", img_size);
    auto res = co_await LsvdImage::create(s3, "random-test-img", img_size);
    res.value();

    // open the image
    XLOGF(INFO, "Opening image {}", "random-test-img");
    auto img = (co_await LsvdImage::mount(s3, "random-test-img", "")).value();

    // TODO use aio variants to ensure concurrency

    // write out the image
    for (uint64_t i = 0; i < img_size / LSVD_BLOCK_SIZE; i++) {
        comp_buf buf;
        fill_buf_blocknum(i, buf);
        auto iov = smartiov::from_ptr(buf.data(), buf.size());
        res = co_await img->write(i * LSVD_BLOCK_SIZE, iov);
        res.value();

        if (i % 1000 == 0)
            std::cout << "." << std::flush;
    }

    XLOGF(INFO, "Wrote {} blocks", img_size / LSVD_BLOCK_SIZE);

    // read back and make sure it's the same
    for (uint64_t i = 0; i < img_size / LSVD_BLOCK_SIZE; i++) {
        comp_buf buf;
        auto iov = smartiov::from_ptr(buf.data(), buf.size());
        res = co_await img->read(i * LSVD_BLOCK_SIZE, iov);
        res.value();

        if (!verify_buf(i, buf, fill_buf_blocknum))
            XLOGF(ERR, "Error verifying block {:08x}", i);

        if (i % 1000 == 0)
            std::cout << "." << std::flush;
    }

    fmt::print("\nVerified {} blocks\n", img_size / LSVD_BLOCK_SIZE);

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
    ReadCache::init_cache(1 * GIB, 1 * GIB, "/tmp/lsvd.rcache");

    std::string pool_name = "pone";
    auto s3 = ObjStore::connect_to_pool(pool_name).value();

    auto ret =
        run_test(s3).scheduleOn(folly::getGlobalCPUExecutor()).start().wait();
    auto res = ret.result();
    if (res.hasException())
        XLOGF(FATAL, "Error:\n{}", res.exception().what());
    return 0;
}
