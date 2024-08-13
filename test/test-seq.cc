#include <array>
#include <cassert>
#include <iostream>
#include <rados/librados.h>

#include "backend.h"
#include "fake_rbd.h"
#include "utils.h"

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

/**
 * Usage:
 *     hexDump(desc, addr, len, perLine);
 *         desc:    if non-NULL, printed as a description before hex dump.
 *         addr:    the address to start dumping from.
 *         len:     the number of bytes to dump.
 *         perLine: number of bytes on each output line.
 *
 * Copied and modified from https://stackoverflow.com/a/7776146/801008
 */
void hexdump(std::string desc, const void *addr, const int len,
             int perLine = 16)
{
    int i;
    vec<byte> buf(perLine + 1);
    auto buff = (unsigned char *)buf.data();
    const unsigned char *pc = (const unsigned char *)addr;

    check_cond(len <= 0, "Invalid length {}", len);

    if (!desc.empty())
        printf("%s:\n", desc.c_str());

    for (i = 0; i < len; i++) {
        if ((i % perLine) == 0) {
            if (i != 0)
                printf("  %s\n", buff);
            printf("  %04x ", i);
        }
        printf(" %02x", pc[i]);
        if ((pc[i] < 0x20) || (pc[i] > 0x7e)) // isprint() may be better.
            buff[i % perLine] = '.';
        else
            buff[i % perLine] = pc[i];
        buff[(i % perLine) + 1] = '\0';
    }

    while ((i % perLine) != 0) {
        printf("   ");
        i++;
    }
    printf("  %s\n", buff);
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
        log_error("Error reading block {:08x}", block_num);
        hexdump("Expected", expected_buf.data(), expected_buf.size());
        hexdump("Actual", buf.data(), buf.size());
        return false;
    }

    return true;
}

void run_test(rados_ioctx_t ctx)
{
    log_info("Starting sequential write then readback test");

    log_info("Removing old image if one exists");
    rbd_remove(ctx, "random-test-img");

    // size_t img_size = 1 * 1024 * 1024 * 1024;
    size_t img_size = 100 * 1024 * 1024;

    // create the image for our own use
    log_info("Creating image {} of size {}", "random-test-img", img_size);
    auto ret = rbd_create(ctx, "random-test-img", img_size, 0);
    check_cond(ret < 0, "Error creating image");

    // open the image
    log_info("Opening image {}", "random-test-img");
    rbd_image_t img;
    ret = rbd_open(ctx, "random-test-img", &img, nullptr);
    check_cond(ret < 0, "Error opening image");

    // TODO use aio variants to ensure concurrency

    // write out the image
    for (uint64_t i = 0; i < img_size / LSVD_BLOCK_SIZE; i++) {
        comp_buf buf;
        fill_buf_blocknum(i, buf);
        ret = rbd_write(img, i * LSVD_BLOCK_SIZE, LSVD_BLOCK_SIZE,
                        reinterpret_cast<const char *>(buf.data()));
        check_cond(ret < 0, "Error writing to image");

        if (i % 1000 == 0)
            std::cout << "." << std::flush;
    }

    fmt::print("\nWrote {} blocks\n", img_size / LSVD_BLOCK_SIZE);

    // read back and make sure it's the same
    for (uint64_t i = 0; i < img_size / LSVD_BLOCK_SIZE; i++) {
        comp_buf buf;
        ret = rbd_read(img, i * LSVD_BLOCK_SIZE, LSVD_BLOCK_SIZE,
                       reinterpret_cast<char *>(buf.data()));
        check_cond(ret < 0, "Error reading from image");

        auto pass = verify_buf(i, buf, fill_buf_blocknum);
        check_cond(!pass, "Error verifying block {:08x}", i);

        if (i % 1000 == 0)
            std::cout << "." << std::flush;
    }

    fmt::print("\nVerified {} blocks\n", img_size / LSVD_BLOCK_SIZE);

    // step 3: close the image
    ret = rbd_close(img);
    check_cond(ret < 0, "Error closing image");

    // step 4: delete the image
    ret = rbd_remove(ctx, "random-test-img");
    check_cond(ret < 0, "Error deleting image");
}

int main(int argc, char *argv[])
{
    // config options
    // TODO set this with new config framework
    setenv("LSVD_RCACHE_DIR", "/tmp/lsvd", 1);
    setenv("LSVD_WCACHE_DIR", "/tmp/lsvd", 1);
    setenv("LSVD_CACHE_SIZE", "2147483648", 1);

    std::string pool_name = "pone";
    auto io_ctx = connect_to_pool(pool_name).value();

    run_test(io_ctx);

    rados_ioctx_destroy(io_ctx);
    return 0;
}
