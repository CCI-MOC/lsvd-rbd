#include <boost/program_options.hpp>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

#include "../fake_rbd.h"
#include "../utils.h"

const uint64_t MAGIC_SEED = 0xdeadbeefff00cab3ull;
using comp_buf = std::array<uint8_t, 4096>;
namespace po = boost::program_options;

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
    unsigned char buff[perLine + 1];
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

void fill_buf_rand_bytes(uint64_t block_num, comp_buf &buf)
{
    auto seed = block_num ^ MAGIC_SEED;

    std::mt19937 gen;
    gen.seed(seed);
    gen.discard(10'000);

    std::uniform_int_distribution<uint8_t> dis(0, 255);

    for (size_t i = 0; i < buf.size(); i++) {
        buf[i] = dis(gen);
    }
}

bool verify_buf_rand_bytes(uint64_t block_num, const comp_buf &buf)
{
    std::array<uint8_t, 4096> expected_buf;
    fill_buf_rand_bytes(block_num, expected_buf);

    if (buf != expected_buf) {
        log_error("Error reading block {}", block_num);
        hexdump("Expected", expected_buf.data(), expected_buf.size());
        hexdump("Actual", buf.data(), buf.size());
    }

    return true;
}

void run_test()
{
    // delete existing image if it exists
    rbd_remove(nullptr, "pone/random-test-img");

    size_t img_size = 1 * 1024 * 1024 * 1024;

    // create the image for our own use
    auto ret = rbd_create(nullptr, "pone/random-test-img", img_size, 0);
    check_cond(ret < 0, "Error creating image");

    // open the image
    rbd_image_t img;
    ret = rbd_open(nullptr, "pone/random-test-img", &img, nullptr);
    check_cond(ret < 0, "Error opening image");

    // TODO use aio variants to ensure concurrency

    // write out the image
    for (uint64_t i = 0; i < img_size / 4096; i++) {
        comp_buf buf;
        fill_buf_rand_bytes(i, buf);
        ret = rbd_write(img, i * 4096, 4096,
                        reinterpret_cast<const char *>(buf.data()));
        check_cond(ret < 0, "Error writing to image");
    }

    // read back and make sure it's the same
    for (uint64_t i = 0; i < img_size / 4096; i++) {
        comp_buf buf;
        ret =
            rbd_read(img, i * 4096, 4096, reinterpret_cast<char *>(buf.data()));
        check_cond(ret < 0, "Error reading from image");

        auto pass = verify_buf_rand_bytes(i, buf);
        check_cond(!pass, "Error verifying block {}", i);
    }

    // step 3: close the image
    ret = rbd_close(img);
    check_cond(ret < 0, "Error closing image");

    // step 4: delete the image
    ret = rbd_remove(nullptr, "pone/random-test-img");
    check_cond(ret < 0, "Error deleting image");
}

int main(int argc, char *argv[])
{
    std::string pool_name = "pone";
    std::string img_name = "random-test-img";

    run_test();

    return 0;
}
