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
#include <thread>

#include "fake_rbd.h"
#include "objects.h"
#include "lsvd_types.h"

extern bool __lsvd_dbg_reverse;
extern bool __lsvd_dbg_be_delay;
extern bool __lsvd_dbg_be_delay_ms;
extern bool __lsvd_dbg_be_threads;
extern bool __lsvd_dbg_rename;

std::mt19937_64 rng;

struct cfg {
    const char *cache_dir;
    const char *obj_prefix;
    int    run_len;
    size_t window;
    int    image_sectors;
    int    n_runs;
    std::vector<long> seeds;
    bool   restart;
    bool   verbose;
    bool   existing;
    int    threads;
};


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

char *rnd_data;
const int max_sectors = 32768;

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
    for (auto p = (int*)buf; sectors > 0; sectors--, p += 512/4) 
	*p = lba++;
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

void calc_crcs(const char *_buf, size_t bytes, std::vector<uint32_t> *crcs) {
    auto buf = (const unsigned char *)_buf;
    for (size_t i = 0; i < bytes; i += 512) {
	const unsigned char *ptr = buf + i;
	crcs->push_back((uint32_t)crc32(0, ptr, 512));
    }
}

struct opinfo {
    rbd_completion_t       c;
    char                  *buf;
    sector_t               sector;
    std::vector<uint32_t> *crcs;
};

std::atomic<uint32_t> op_sequence;

struct x_info {
    uint32_t seq;
    bool     launch;
    sector_t sector;
    std::vector<uint32_t> *crcs = NULL;

    x_info(uint32_t seq_, bool launch_, sector_t sector_,
	   std::vector<uint32_t>*crcs_) : seq(seq_), launch(launch_),
					  sector(sector_), crcs(crcs_) {}
#if 0
    ~x_info() { if (crcs) delete crcs; }
#endif
};

std::vector<x_info> all_info;
std::mutex aim;

void print_allinfo(void) {
    for (int i = 0; i < (int)all_info.size(); i++)
	if (all_info[i].launch)
	    printf("%d %d %d %d\n", i, all_info[i].seq, (int)all_info[i].sector,
		   (int)all_info[i].crcs->size());
}

void drain(std::queue<opinfo> &q, size_t window) {
    while (q.size() > window) {
	auto [c,ptr,sector,crcs] = q.front();
	q.pop();

	rbd_aio_wait_for_complete(c);

	std::unique_lock lk(aim);
	auto seq = op_sequence++;
	if (seq % 2000 == 1999)
	    printf("+"), fflush(stdout);
	all_info.emplace_back(seq, false, sector, crcs);
	rbd_aio_release(c);
	free(ptr);
    }
}

void stamp_seq(char *buf, int sectors, uint32_t seq) {
    for (auto p = (int*)buf; sectors > 0; sectors--, p += 512/4) 
	p[1] = seq;
}

void run_test(rbd_image_t img, struct cfg *cfg) {
    std::queue<opinfo> q;
    int start = op_sequence;
    while (op_sequence - start < 2U * cfg->run_len) {
	drain(q, cfg->window-1);
	rbd_completion_t c;
	rbd_aio_create_completion(NULL, NULL, &c);

	int n = n_sectors();
	int lba = gen_lba(cfg->image_sectors, n);
	auto ptr = (char*)aligned_alloc(512, n*512);
	
	get_random(ptr, lba, n);
	auto crcs = new std::vector<uint32_t>();
	auto seq = op_sequence++;
	if (seq - start > 2U * cfg->run_len)
	    break;

	stamp_seq(ptr, n, seq);
	calc_crcs(ptr, n*512, crcs);
	{
	    std::unique_lock lk(aim);
	    all_info.emplace_back(seq, true, lba, crcs);
	    q.push((opinfo){c, ptr, lba, crcs});
	}
	rbd_aio_write(img, 512L * lba, 512L * n, ptr, c);
	if (seq % 2000 == 1999)
	    printf("+"), fflush(stdout);
    }
    drain(q, 0);
}

struct lba_info {
    bool launch;
    uint32_t seq;
    uint32_t crc;
};

void test_img(rbd_image_t img, struct cfg *cfg) {
    auto tmp = __lsvd_dbg_be_delay;
    __lsvd_dbg_be_delay = false;

    std::vector<std::vector<lba_info>> per_lba_ops(cfg->image_sectors);
    int q = 0;
    for (auto [s,l,lba,crcs] : all_info) {
	for (auto c : *crcs) {
	    per_lba_ops[lba].push_back((lba_info){l,s,c});
	    lba++;
	}
	if (++q > (int) all_info.size() / 10) {
	    printf("."); fflush(stdout);
	    q = 0;
	}
    }

    std::vector<std::vector<uint32_t>> all_crcs(cfg->image_sectors);
    q = 0;
    for (int i = 0; i < cfg->image_sectors; i++) {
	auto op = &per_lba_ops[i];
	if (op->size() == 0) {
	    all_crcs[i].push_back(0xb2aa7578); // zero sector
	    continue;
	}
	int last = 0, count = 0;
	for (size_t j = 0; j < op->size()-1; j++) {
	    auto [launch, seq, crc] = (*op)[j];
	    (void) crc;
	    (void) seq;
	    if (launch) count++;
	    else count--;
	    if (count == 0)
		last = j;
	}
	for (size_t j = last; j < op->size(); j++) {
	    auto [launch, seq, crc] = (*op)[j];
	    (void)seq;
	    if (launch)
		all_crcs[i].push_back(crc);
	}
	if (++q > cfg->image_sectors / 10) {
	    printf(","); fflush(stdout);
	    q = 0;
	}
    }

    auto buf = (char*)aligned_alloc(512, 64*1024);
    q = 0;
    for (int i = 0; i < cfg->image_sectors; i += 128) {
	rbd_read(img, i * 512L, 64*1024, buf);
	for (int j = 0; j < 128; j++) {
	    bool found = false;
	    auto ptr = (unsigned char*)buf + j*512;
	    uint32_t crc = crc32(0, ptr, 512);
	    for (auto c : all_crcs[i+j])
		if (crc == c)
		    found = true;
	    assert(found);
	}
	if (++q > cfg->image_sectors / 128 / 10) {
	    printf("-"); fflush(stdout);
	    q = 0;
	}
    }
    printf("\n");
    free(buf);

    __lsvd_dbg_be_delay = tmp;
}

static char args_doc[] = "RUNS";

static struct argp_option options[] = {
    {"seed",     's', "S",    0, "use this seed (one run)"},
    {"len",      'l', "N",    0, "run length"},
    {"window",   'w', "W",    0, "write window"},
    {"size",     'z', "S",    0, "volume size (e.g. 1G, 100M)"},
    {"cache-dir",'d', "DIR",  0, "cache directory"},
    {"prefix",   'p', "PREFIX", 0, "object prefix"},
    {"keep",     'k', 0,      0, "keep data between tests"},
    {"verbose",  'v', 0,      0, "print LBAs and CRCs"},
    {"reverse",  'R', 0,      0, "reverse NVMe completion order"},    
    {"existing", 'x', 0,      0, "don't delete existing cache"},    
    {"threads",  't', "N",    0, "number of threads"},
    {"delay",    'D', 0,      0, "add random backend delays"},    
    {0},
};

struct cfg _cfg = {
    "/tmp",			// cache_dir
    "/tmp/bkt/obj",		// obj_prefix
    10000, 			// run_len
    16,				// window
    1024*1024*2,		// image_sectors,
    1,				// n_runs
    {},				// seeds
    true,			// restart
    false,			// verbose
    false,			// existing
    2};				// threads

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
    case 't':
	_cfg.threads = atoi(arg);
	break;
    case 'D':
	__lsvd_dbg_be_delay = true;
	break;
    case ARGP_KEY_END:
        break;
    }
    return 0;
}
static struct argp argp = { options, parse_opt, NULL, args_doc};

int main(int argc, char **argv) {
    argp_parse (&argp, argc, argv, 0, 0, 0);

    if (_cfg.seeds.size() == 0) {
	auto now = std::chrono::system_clock::now();
	auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
	auto value = now_ms.time_since_epoch();
	long seed = value.count();
	rng.seed(seed);
	for (int i = 0; i < _cfg.n_runs; i++)
	    _cfg.seeds.push_back(rng());
    }

    setenv("LSVD_CACHE_SIZE", "100M", 1);
    setenv("LSVD_BACKEND", "file", 1);
    setenv("LSVD_CACHE_DIR", _cfg.cache_dir, 1);
    __lsvd_dbg_rename = true;
    
    bool started = false;
    for (auto s : _cfg.seeds) {
	if (_cfg.restart || (!started && !_cfg.existing)) {
	    clean_cache(_cfg.cache_dir);
	    clean_image(_cfg.obj_prefix);
	    create_image(_cfg.obj_prefix, _cfg.image_sectors);
	    started = true;

	    for (auto [seq, launch, sector, crcs] : all_info) {
		(void) seq; (void) sector;
		if (launch)
		    delete crcs;
	    }
	    all_info.clear();
	    op_sequence.store(0);
	}
	    
	rng.seed(s);
	init_random();
	printf("seed: 0x%lx\n", s);

	rados_ioctx_t io = 0;
	rbd_image_t img;
	if (rbd_open(io, _cfg.obj_prefix, &img, NULL) < 0)
	    printf("failed: rbd_open\n"), exit(1);

	std::vector<std::thread> threads;
	for (int i = 0; i < _cfg.threads; i++)
	    threads.push_back(std::thread(run_test, img, &_cfg));

	while (threads.size() > 0) {
	    threads.back().join();
	    threads.pop_back();
	}

	test_img(img, &_cfg);
	rbd_close(img);
    }
}
    
