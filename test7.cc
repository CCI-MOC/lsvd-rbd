#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <random>
#include <chrono>
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#include <iostream>
#include <queue>

#include "fake_rbd.h"
#include "objects.h"
#include "lsvd_types.h"

std::mt19937_64 rng;
char *rnd_data;
const int max_sectors = 32768;

/* empirical fit to the pattern of writes in the ubuntu install,
 * except it has lots more writes in the 128..32768 sector range
 */
int n_sectors(void) {
    std::uniform_int_distribution<int> uni16(0,16);
    auto n = uni16(rng);
    if (n < 8)
	return 8;
    int max;
    if (n < 12)
	max = 32;
    else if (n < 14)
	max = 64;
    else if (n < 15)
	max = 128;
    else
	max = 32768;
    std::uniform_int_distribution<int> uni(8/8,max/8);
    return uni(rng) * 8;
}

int gen_lba(int max, int n) {
    std::uniform_int_distribution<int> uni(0,(max-n)/8);
    return uni(rng) * 8;
}

void init_random(void) {
    size_t bytes = max_sectors * 512;
    rnd_data = (char*)malloc(bytes);
    for (long *p = (long*)rnd_data, *end = (long*)(rnd_data+bytes); p<end; p++)
	*p = rng();
}

void get_random(char *buf, int lba, int sectors) {
    int slack = (max_sectors - sectors) * 512;
    std::uniform_int_distribution<int> uni(0,slack);
    int offset = uni(rng);
    memcpy(buf, rnd_data+offset, sectors*512);
    for (auto p = (int*)buf; sectors > 0; sectors--)
	*p++ = lba++;
}


void clean_image(std::string name) {
    auto dir = fs::path(name).parent_path();
    std::string prefix = fs::path(name).filename();
    
    for (auto const& dir_entry : fs::directory_iterator{dir}) {
	std::string entry{dir_entry.path().filename()};
	if (strncmp(entry.c_str(), prefix.c_str(), prefix.size()) == 0)
	    fs::remove(dir_entry.path());
    }
}

void clean_cache(std::string cache_dir, std::string name) {
    auto path = cache_dir + "/" + name + ".cache";
    unlink(path.c_str());
}

void create_image(std::string name, int sectors) {
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    auto _hdr = (obj_hdr*) buf;

    *_hdr = (obj_hdr){LSVD_MAGIC,
		      1,	  // version
		      {0},	  // UUID
		      LSVD_SUPER, // type
		      0,	  // seq
		      8,	  // hdr_sectors
		      0};	  // data_sectors
    uuid_generate_random(_hdr->vol_uuid);

    unlink(name.c_str());
    FILE *fp = fopen(name.c_str(), "w");
    if (fp == NULL)
	perror("image create");
    fwrite(buf, sizeof(buf), 1, fp);
    fclose(fp);
}

auto image_name = "/tmp/bkt/obj";
    
void drain(std::queue<std::pair<rbd_completion_t,char*>> &q, int window) {
    while (q.size() >= window) {
	auto [c,ptr] = q.front();
	q.pop();
	rbd_aio_wait_for_complete(c);
	rbd_aio_release(c);
	free(ptr);
    }
}
    
void run_test(unsigned long seed) {
    printf("seed: 0x%lx\n", seed);

    //int image_sectors = 10*1024*1024*2; // 10G
    int image_sectors = 1024*1024*2; // 1G

    clean_cache("/tmp", "obj");
    clean_image(image_name);
    create_image(image_name, image_sectors);

    setenv("LSVD_CACHE_SIZE", "300M", 1);
    setenv("LSVD_BACKEND", "file", 1);
    
    rados_ioctx_t io = 0;
    rbd_image_t img;

    if (rbd_open(io, image_name, &img, NULL) < 0)
	printf("failed: rbd_open\n"), exit(1);

    std::queue<std::pair<rbd_completion_t,char*>> q;
    int window = 16;
    int n_writes = 10000;

    for (int i = 0; i < n_writes; i++) {
	drain(q, window);
	if (i % 1000 == 999)
	    printf("+"), fflush(stdout);
	rbd_completion_t c;
	rbd_aio_create_completion(NULL, NULL, &c);

	int n = n_sectors();
	int lba = gen_lba(image_sectors, n);
	auto ptr = (char*)aligned_alloc(512, n*512);

	q.push(std::make_pair(c, ptr));
	rbd_aio_write(img, 512L * lba, 512L * n, ptr, c);
    }
    drain(q, 0);
    printf("\ndone\n");
    rbd_close(img);
}


int main(int argc, char **argv) {
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    auto value = now_ms.time_since_epoch();
    long seed = value.count();

    rng.seed(seed);

    clean_image("/tmp/bkt/obj");
    
    std::vector<unsigned long> seeds;
    for (int i = 0; i < 20; i++)
	seeds.push_back(rng());
    for (auto s : seeds)
	run_test(s);
}
    
