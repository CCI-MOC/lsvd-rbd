#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <argp.h>

#include <random>
#include <chrono>
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#include <iostream>
#include <queue>
#include <map>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <fstream>

#include "fake_rbd.h"
#include "objects.h"
#include "lsvd_types.h"
#include "lsvd_debug.h"
#include "objname.h"

extern bool __lsvd_dbg_reverse;
extern bool __lsvd_dbg_be_delay;
extern bool __lsvd_dbg_be_delay_ms;
extern bool __lsvd_dbg_be_threads;

std::mt19937_64 rng;

struct cfg {
    const char *cache_dir;
    const char *obj_prefix;
    int    run_len;
    size_t window;
    int    image_sectors;
    float  read_fraction;
    int    n_runs;
    std::vector<long> seeds;
    bool   restart;
    bool   verbose;
    bool   existing;
    int    lose_objs;
    bool   wipe_cache;
    int    lose_writes;
};


/* empirical fit to the pattern of writes in the ubuntu install,
 * except it has lots more writes in the 128..32768 sector range
 */
int n_sectors(void) {
    struct { float p; int max; } params[] = {
	{0.5, 8},
	{0.25, 32},
	{0.125, 64},
	{0.0625, 128},
	{1.0, 4096}};
    std::uniform_real_distribution<> uni(0.0,1.0);
    float r = uni(rng);
    int max = 8;

    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
	if (r < params[i].p) {
	    max = params[i].max;
	    break;
	}
	r -= params[i].p;
    }

    std::uniform_int_distribution<int> uni_max(8/8,max/8);
    return uni_max(rng) * 8;
}

int gen_lba(int max, int n) {
    std::uniform_int_distribution<int> uni(0,(max-n)/8);
    return uni(rng) * 8;
}

char *rnd_data;
const int max_sectors = 32768;

void init_random(void) {
    size_t bytes = max_sectors * 512;
    rnd_data = (char*)malloc(bytes);
    for (long *p = (long*)rnd_data, *end = (long*)(rnd_data+bytes); p<end; p++)
	*p = rng();
}

void get_random(char *buf, int lba, int sectors, int seq) {
    int slack = (max_sectors - sectors) * 512;
    std::uniform_int_distribution<int> uni(0,slack);
    int offset = uni(rng);
    memcpy(buf, rnd_data+offset, sectors*512);
    for (auto p = (int*)buf; sectors > 0; sectors--, p += 512/4) {
	p[0] = lba++;
	p[1] = seq;
    }
}

void clean_image(std::string name) {
    auto dir = fs::path(name).parent_path();
    std::string prefix = fs::path(name).filename();

    if (!fs::exists(dir))
	fs::create_directory(dir);
    else {
	for (auto const& dir_entry : fs::directory_iterator{dir}) {
	    std::string entry{dir_entry.path().filename()};
	    if (strncmp(entry.c_str(), prefix.c_str(), prefix.size()) == 0)
		fs::remove(dir_entry.path());
	}
    }
}

void clean_cache(std::string cache_dir) {
    const char *suffix = ".cache";
    for (auto const& dir_entry : fs::directory_iterator{cache_dir}) {
	std::string entry{dir_entry.path().filename()};
	if (!strcmp(suffix, entry.c_str() + entry.size() - strlen(suffix)))
	    fs::remove(dir_entry.path());
	if (!strncmp(entry.c_str(), "gc.", 3))
	    fs::remove(dir_entry.path());
    }
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

    auto _super = (super_hdr*)(_hdr + 1);
    *_super = (super_hdr){(uint64_t)sectors, // vol_size
			  0,	   // total data sectors
			  0,	   // live sectors
			  1,	   // next object
			  0, 0,	   // checkpoint offset, len
			  0, 0,	   // clone offset, len
			  0, 0};   // snap offset, len

    unlink(name.c_str());
    FILE *fp = fopen(name.c_str(), "w");
    if (fp == NULL)
	perror("image create");
    fwrite(buf, sizeof(buf), 1, fp);
    fclose(fp);
}

#include <zlib.h>


typedef std::pair<rbd_completion_t,char*> opinfo;

void drain(std::queue<opinfo> &q, size_t window) {
    while (q.size() > window) {
	auto [c,ptr] = q.front();
	q.pop();
	rbd_aio_wait_for_complete(c);
	rbd_aio_release(c);
	free(ptr);
    }
}

bool started = false;
std::vector<std::tuple<int,int>> io_list;
void print_iolist(void) {
    for (auto [lba,len] : io_list)
	printf("%d %d\n", lba, len);
}

void obj_delete(struct cfg *cfg) {
    std::vector<std::pair<int, std::string>> files;
    std::string dir = fs::path(cfg->obj_prefix).parent_path();
    auto stem = fs::path(cfg->obj_prefix).filename();
    size_t stem_len = strlen(stem.c_str());
    
    for (auto const& dir_entry : fs::directory_iterator{dir}) {
	std::string entry{dir_entry.path().filename()};
	if (strncmp(entry.c_str(), stem.c_str(), stem_len) == 0)
	    if (entry.size() == stem_len + 9) {
		int seq = strtol(entry.c_str() + stem_len+1, NULL, 16);
		files.push_back(std::make_pair(seq, entry));
	    }
	}

    std::sort(files.begin(), files.end());

    /* find the last checkpoint - we're guaranteed not to have lost
     * that or any preceding objects
     */
    int last_ckpt = 0;
    for (auto [seq, name] : files) {
	std::string path = dir + "/" + name;
	std::ifstream fp(path, std::ios::binary);
	char buf[512];
	assert(fp.read(buf, sizeof(buf)));
	auto _hdr = (obj_hdr*) buf;
	if (_hdr->type == LSVD_CKPT)
	    last_ckpt = seq;
    }

    printf("last_ckpt %08x last_file %08x\n", last_ckpt, files[files.size()-1].first);
    
    std::uniform_real_distribution<> uni(0.0,1.0);

    for (int i = 0, j = files.size()-1; i < cfg->lose_objs; i++, j--) {
	auto [seq, file] = files[j];
	if (seq <= last_ckpt)
	    break;
	if (uni(rng) < 0.5) {
	    fs::remove(dir + "/" + file);
	    printf("rm %s\n", file.c_str());
	}
    }
}

void lose_writes(struct cfg *cfg) {
    printf("lose_writes: not implemented\n");
}

void kill_image(rbd_image_t img, struct cfg *cfg) {
    auto prefix = std::string(cfg->obj_prefix);
    auto stem = fs::path(prefix).filename();
    auto parent = fs::path(prefix).parent_path();
    size_t stem_len = strlen(stem.c_str());

    /* save a copy of the superblock
     */
    char buf[4096];
    std::ifstream fp(prefix, std::ios::binary);
    assert(fp.read(buf, sizeof(buf)));
    fp.close();
    auto _hdr = (obj_hdr*)buf;
    auto super = (super_hdr*)(_hdr+1);

    std::vector<uint32_t> ckpts;
    decode_offset_len<uint32_t>(buf, super->ckpts_offset, super->ckpts_len, ckpts);
    
    printf("ckpts: ");
    for (auto c : ckpts)
	printf(" %x", c);
    printf("\n");
    
    /* find the most recent object written
     */
    std::vector<std::pair<int, std::string>> files;
    for (auto const& dir_entry : fs::directory_iterator{parent}) {
	std::string entry{dir_entry.path().filename()};
	if (strncmp(entry.c_str(), stem.c_str(), stem_len) == 0)
	    if (entry.size() == stem_len + 9) {
		int seq = strtol(entry.c_str() + stem_len+1, NULL, 16);
		files.push_back(std::make_pair(seq, entry));
	    }
	}

    std::sort(files.begin(), files.end());
    auto latest = files[files.size()-1].first;

    /* after we close the image, set everything back to the state from just
     * before closing
     */
    rbd_kill(img);
    std::ofstream fp2(prefix, std::ios::binary);
    assert(fp2.write(buf, sizeof(buf)));
    fp2.close();

    for (auto const& dir_entry : fs::directory_iterator{parent}) {
	std::string entry{dir_entry.path().filename()};
	if (strncmp(entry.c_str(), stem.c_str(), stem_len) == 0) {
	    if (entry.size() == stem_len + 9) {
		int seq = strtol(entry.c_str() + stem_len+1, NULL, 16);
		if (seq > latest) {
		    printf("kill: removing %s\n", entry.c_str());
		    fs::remove(dir_entry.path());
		}
	    }
	}
    }
}

void run_test(unsigned long seed, struct cfg *cfg) {
    printf("seed: 0x%lx\n", seed);
    rng.seed(seed);

    init_random();

    if (!started || cfg->restart) {
	clean_cache(cfg->cache_dir);
	clean_image(cfg->obj_prefix);
	create_image(cfg->obj_prefix, cfg->image_sectors);
	started = true;
    }

    setenv("LSVD_CACHE_SIZE", "100M", 1);
    setenv("LSVD_BACKEND", "file", 1);
    setenv("LSVD_CACHE_DIR", cfg->cache_dir, 1);
    
    rados_ioctx_t io = 0;
    rbd_image_t img;

    if (rbd_open(io, cfg->obj_prefix, &img, NULL) < 0)
	printf("failed: rbd_open\n"), exit(1);

    std::queue<std::pair<rbd_completion_t,char*>> q;
    std::uniform_real_distribution<> uni(0.0,1.0);

    int n_requests = cfg->run_len + (uni(rng)-0.5)*100;
    int seq = 0;

    struct op {
	int sector;
	int len;
	int seq;
	std::vector<uint32_t> *crcs;
    };
    
    struct triple {
	int lba;
	int seq;
	uint32_t crc;
    };

    std::vector<op> op_crcs;
    
    for (int i = 0; i < n_requests; i++) {
	drain(q, cfg->window-1);
	if (i % 1000 == 999)
	    printf("+"), fflush(stdout);
	rbd_completion_t c;
	rbd_aio_create_completion(NULL, NULL, &c);

	int n = n_sectors();
	int lba = gen_lba(cfg->image_sectors, n);
	io_list.push_back(std::make_tuple(lba, n));
	
	auto ptr = (char*)aligned_alloc(512, n*512);
	
	q.push(std::make_pair(c, ptr));
	if (uni(rng) < cfg->read_fraction) {
	    rbd_aio_read(img, 512L * lba, 512L * n, ptr, c);
	}
	else {
	    auto s = ++seq;
	    get_random(ptr, lba, n, s);
	    rbd_aio_write(img, 512L * lba, 512L * n, ptr, c);
	    auto crc_list = new std::vector<uint32_t>();
	    for (size_t j = 0; j < 512UL*n; j += 512) {
		auto p = (const unsigned char*)ptr + j;
		auto crc = (uint32_t)crc32(0, p, 512);
		crc_list->push_back(crc);
	    }
	    op_crcs.push_back((op){lba, n, s, crc_list});
	}
    }
    printf(" K "); fflush(stdout);
    kill_image(img, cfg);
    
    if (cfg->lose_objs)
	obj_delete(cfg);
    if (cfg->wipe_cache)
	clean_cache(cfg->cache_dir);
    else if (cfg->lose_writes)
	lose_writes(cfg);

    if (rbd_open(io, cfg->obj_prefix, &img, NULL) < 0)
	printf("failed: rbd_open\n"), exit(1);


    auto tmp = __lsvd_dbg_be_delay;
    __lsvd_dbg_be_delay = false;

    auto buf = (char*)aligned_alloc(512, 64*1024);
    std::vector<triple> image_info(cfg->image_sectors);
    
    int i = 0;
    for (int sector = 0; sector < cfg->image_sectors; sector += 64*2) {
	rbd_read(img, sector*512L, 64*1024, buf);
	for (int j = 0; j < 64*1024; j += 512) {
	    auto p = (const unsigned char*)buf + j;
	    uint32_t crc = crc32(0, p, 512);
	    image_info[sector + j/512] = (triple){sector+j/512,
						  *(int*)(p+4), crc};
	    assert(*(int*)(p+4) <= (int)image_info.size()+1);
	}
	if (++i > cfg->image_sectors / 128 / 10) {
	    printf("-"); fflush(stdout);
	    i = 0;
	}
    }
    printf("\n");
    free(buf);
    rbd_close(img);
    __lsvd_dbg_be_delay = tmp;
    
    int max_seq = 0;
    for (int i = 0; i < cfg->image_sectors; i++)
	if (image_info[i].seq > max_seq) {
	    assert(max_seq <= (int)image_info.size()+1);
	    max_seq = image_info[i].seq;
	}

    printf("lost %d writes\n", n_requests - max_seq);
    
    std::vector<triple> should_be_crcs(cfg->image_sectors);
    for (int i = 0; i < cfg->image_sectors; i++)
	should_be_crcs[i].crc = 0xb2aa7578;

    for (int i = 0; i < max_seq; i++) {
	auto [lba,len,seq,crcs] = op_crcs[i];
	assert(seq == i+1);
	int j = 0;
	for (auto c : *crcs) {
	    should_be_crcs[lba+j] = (triple){lba+j,seq,c};
	    j++;
	}
    }
    int failed = 0;
    for (int i = 0; i < cfg->image_sectors && failed < 100; i++)
	if (should_be_crcs[i].crc != image_info[i].crc) {
	    printf("%d : %08x (seq %d) should be: %08x (%d)\n", i,
		   image_info[i].crc, image_info[i].seq,
		   should_be_crcs[i].crc, should_be_crcs[i].seq);
	    failed++;
	}
    assert(!failed);
    printf("ok\n");
}


static char args_doc[] = "RUNS";

static struct argp_option options[] = {
    {"seed",     's', "S",    0, "use this seed (one run)"},
    {"len",      'l', "N",    0, "run length"},
    {"window",   'w', "W",    0, "write window"},
    {"size",     'z', "S",    0, "volume size (e.g. 1G, 100M)"},
    {"cache-dir",'d', "DIR",  0, "cache directory"},
    {"prefix",   'p', "PREFIX", 0, "object prefix"},
    {"reads",    'r', "FRAC", 0, "fraction reads (0.0-1.0)"},
    {"keep",     'k', 0,      0, "keep data between tests"},
    {"verbose",  'v', 0,      0, "print LBAs and CRCs"},
    {"reverse",  'R', 0,      0, "reverse NVMe completion order"},
    {"existing", 'x', 0,      0, "don't delete existing cache"},
    {"delay",    'D', 0,      0, "add random backend delays"},
    {"lose-objs",'o', "N",    0, "delete some of last N objects"},
    {"wipe-cache",'W', 0,     0, "delete cache on restart"},
    {"lose-writes",'L', "N",    0, "delete some of last N cache writes"},
    {0},
};

struct cfg _cfg = {
    "/tmp",			// cache_dir
    "/tmp/bkt/obj",		// obj_prefix
    10000, 			// run_len
    16,				// window
    1024*1024*2,		// image_sectors,
    0.0,			// read_fraction
    1,				// n_runs
    {},				// seeds
    true,			// restart
    false,			// verbose
    false,			// existing
    0,				// lose_objs
    false,			// wipe_cache
    0};				// lose_writes

off_t parseint(char *s)
{
    off_t val = strtoul(s, &s, 0);
    if (toupper(*s) == 'G')
        val *= (1024*1024*1024);
    if (toupper(*s) == 'M')
        val *= (1024*1024);
    if (toupper(*s) == 'K')
        val *= 1024;
    return val;
}

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case ARGP_KEY_INIT:
	break;
    case ARGP_KEY_ARG:
        _cfg.n_runs = atoi(arg);
        break;
    case 's':
	_cfg.seeds.push_back(strtoul(arg, NULL, 0));
	break;
    case 'l':
	_cfg.run_len = atoi(arg);
	break;
    case 'w':
	_cfg.window = atoi(arg);
	break;
    case 'z':
	_cfg.image_sectors = parseint(arg) / 512;
	break;
    case 'd':
	_cfg.cache_dir = arg;
	break;
    case 'p':
	_cfg.obj_prefix = arg;
	break;
    case 'r':
	_cfg.read_fraction = atof(arg);
	break;
    case 'k':
	_cfg.restart = false;
	break;
    case 'v':
	_cfg.verbose = true;
	break;
    case 'R':
	__lsvd_dbg_reverse = true;
	break;
    case 'x':
	_cfg.existing = true;
	break;
    case 'D':
	__lsvd_dbg_be_delay = true;
	break;
    case 'o':
	_cfg.lose_objs = atoi(arg);
	break;
    case 'W':
	_cfg.wipe_cache = true;
	break;
    case 'L':
	_cfg.lose_writes = atoi(arg);
	break;
    case ARGP_KEY_END:
        break;
    }
    return 0;
}
static struct argp argp = { options, parse_opt, NULL, args_doc};

int main(int argc, char **argv) {
    argp_parse (&argp, argc, argv, 0, 0, 0);

    if (_cfg.seeds.size() > 0) {
	for (int i = 0; i < _cfg.n_runs; i++)
	    for (auto s : _cfg.seeds)
		run_test(s, &_cfg);
    }
    else {
	auto now = std::chrono::system_clock::now();
	auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
	auto value = now_ms.time_since_epoch();
	long seed = value.count();

	rng.seed(seed);

	std::vector<unsigned long> seeds;
	for (int i = 0; i < _cfg.n_runs; i++)
	    seeds.push_back(rng());
	for (auto s : seeds)
	    run_test(s, &_cfg);
    }
}
    
