/* translated from test1.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>		// mkdir

#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fake_rbd.h"
#include "lsvd_debug.h"

std::string img = "/tmp/bkt/obj";
std::string dir = "/tmp/bkt";

/* setup/teardown fixtures:
 * class instance will be created before each TEST_CASE_FIXTURE,
 * protected fields will be accessible within the test
 */
class setup_test {
protected:
    _dbg *xlate = NULL;
    int   vol_size = 0;
    
public:
    setup_test() {
	if (access(dir.c_str(), R_OK|W_OK|X_OK) < 0)
	    mkdir(dir.c_str(), 0777);
	for (const auto & entry : fs::directory_iterator(dir))
	    unlink(entry.path().c_str());
	std::string cmd = "python3 mkdisk.py --size 10M " + img;
	system(cmd.c_str());
	vol_size = xlate_open((char*)img.c_str(), 1, false, (void**)&xlate);
    }
    ~setup_test() {
	sleep(1);
	if (xlate)
	    xlate_close(xlate);
    }
};

bool verify_bytes(const char *ptr, off_t start, off_t len, int val) {
    for (int i = 0; i < len; i++)
	if (ptr[start+i] != val)
	    return false;
    return true;
}

TEST_CASE_FIXTURE(setup_test, "Basic tests") {

    SUBCASE("volume size") {
	CHECK(vol_size == 10*1024*1024);
    }

    SUBCASE("Read zeros") {
	int base = 0;
	char buf[30*1024];
	std::vector<int> Ls = {512, 4096, 16384};
	std::vector<int> ns = {1, 10, 17, 49};
	for (auto L : Ls) {
	    for (auto n : ns) {
		memset(buf, 'x', L);
		auto rv = xlate_read(xlate, buf, base, L);
		CHECK(rv >= 0);
		CHECK(verify_bytes(buf, 0, L, 0));
		base += (n*512);
	    }
	}
    }

    SUBCASE("Map size") {
	CHECK(xlate_size(xlate) == 0);
	std::vector<char> d1(512, 'A');

	xlate_write(xlate, d1.data(), 0, d1.size());
	xlate_flush(xlate);
	CHECK(xlate_size(xlate) == 1);

	xlate_write(xlate, d1.data(), 10*512, d1.size());
	xlate_flush(xlate);
	CHECK(xlate_size(xlate) == 2);

	std::vector<char> d2(11*512, 'B');
	xlate_write(xlate, d2.data(), 0, d2.size());
	xlate_flush(xlate);
	CHECK(xlate_size(xlate) == 1);
    }

    SUBCASE("Read / write 1") {
	std::vector<char> d(4096,'X');
	xlate_write(xlate, d.data(), 0, d.size());
	xlate_flush(xlate);
	char buf[1024];
	xlate_read(xlate, buf, 0, sizeof(buf));
	CHECK(verify_bytes(buf, 0, sizeof(buf), 'X'));
    }

    SUBCASE("Read / write 2") {
	std::vector<char> dA(8192,'A');
	std::vector<char> dB(4096,'B');
	xlate_write(xlate, dA.data(), 8*1024, dA.size());
	xlate_write(xlate, dB.data(), 12*1024, dB.size());
	xlate_flush(xlate);

	char buf[8192];
	xlate_read(xlate, buf, 8192, 8192);
	CHECK(verify_bytes(buf, 0, 4096, 'A'));
	CHECK(verify_bytes(buf, 4096, 4096, 'B'));
    }
}
