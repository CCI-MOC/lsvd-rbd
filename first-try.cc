/*
 * file:        first-try.cc
 * description: first pass at a userspace block-on-object layer
 */

#include "extent.cc"
#include "objects.cc"

#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <stack>
#include <map>
#include <thread>
#include <ios>
#include <sstream>
#include <iomanip>

#include <unistd.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <string.h>

/* make this atomic? */
int batch_seq = 1;
const int BATCH_SIZE = 8 * 1024 * 1024;
uuid_t my_uuid;

static int div_round_up(int n, int m)
{
    return (n + m - 1) / m;
}

static int round_up(int n, int m)
{
    return div_round_up(n, m) * m;
}

	
struct batch {
    char  *buf;
    size_t max;
    size_t len;
    int    seq;
    std::vector<data_map> entries;
public:
    batch(size_t _max) {
	buf = (char*)malloc(_max);
	max = _max;
    }
    void reset(void) {
	len = 0;
	entries.resize(0);
	seq = batch_seq++;
    }
    int hdrlen(void) {
	return sizeof(hdr) + sizeof(data_hdr) + entries.size() * sizeof(data_map);
    }
};

// merge buffer and free list are protected by merge_lock
//
std::mutex              m;
std::condition_variable cv;
std::queue<batch*>      work_queue;

batch              *current_batch;
std::stack<batch*>  batches;
extmap::objmap      object_map;
std::map<int,int>   object_size;  // in sectors
std::map<int,int>   object_hdrsz; // in sectors
std::map<int,char*> in_mem_objects;

void make_hdr(char *buf, batch *b)
{
    hdr *h = (hdr*)buf;
    h->magic = LSVD_MAGIC;
    h->version = 1;
    memcpy(h->vol_uuid, my_uuid, sizeof(uuid_t));
    h->type = LSVD_DATA;
    h->seq = b->seq;
    h->hdr_sectors = 8;
    h->data_sectors = b->len / 512;

    data_hdr *dh = (data_hdr*)(h+1);
    dh->last_data_obj = b->seq;
    dh->map_offset = sizeof(*h) + sizeof(*dh);
    dh->map_len = b->entries.size() * sizeof(data_map);

    data_map *dm = (data_map*)(dh+1);
    for (auto e : b->entries)
	*dm++ = e;
}

char      *prefix;
char      *super;
hdr       *super_h;
super_hdr *super_sh;
size_t     super_len;

std::string hex(uint32_t n)
{
    std::stringstream stream;
    stream << std::setfill ('0') << std::setw(8) << std::hex << n;
    return stream.str();
}

ssize_t read_super(char *name)
{
    prefix = strdup(name);
    int fd = open(name, O_RDONLY);
    if (fd < 0)
	return -1;

    size_t len = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    super = (char*)calloc(len, 1);
    read(fd, super, len);
    super_len = len;

    super_h = (hdr*)super;
    if (super_h->magic != LSVD_MAGIC || super_h->version != 1 ||
	super_h->type != LSVD_SUPER)
	return -1;
    memcpy(my_uuid, super_h->vol_uuid, sizeof(uuid_t));

    super_sh = (super_hdr*)(super_h+1);
    batch_seq = super_sh->next_obj;
    return super_sh->vol_size * 512;
}

void write_object(int seq, iovec *iov, int iovcnt)
{
    auto name = std::string(prefix) + "." + hex(seq);
    int fd = open(name.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0777);
    if (fd < 0)
	perror("create"), exit(1);
    writev(fd, iov, iovcnt);
    close(fd);
}

size_t read_object(int seq, char *buf, size_t offset, size_t len)
{
    auto name = std::string(prefix) + "." + hex(seq);
    int fd = open(name.c_str(), O_RDONLY);
    if (fd < 0)
	return -1;
    auto val = pread(fd, buf, len, offset);
    close(fd);
    return val;
}

/* read data map from data object header
 * for now ignores checkpoints, objects_cleaned
 */
void read_data_map(char *buf, size_t len)
{
    hdr *h = (hdr*)buf;
    assert(h->type == LSVD_DATA);
    assert(h->hdr_sectors*512 <= len);
    
    data_hdr *dh = (data_hdr*)(h+1);
    data_map *m = (data_map*)(buf + dh->map_offset),
	*map_end = (data_map*)(buf + dh->map_offset + dh->map_len);
    uint64_t offset = 0;
    
    for (; m < map_end; m++) {
	object_map.update(m->lba, m->lba+m->len,
			  (extmap::obj_offset){.obj = (uint64_t) h->seq,
			      .offset = offset});
	offset += m->len;
    }
}

/* ignores checkpoint list, object/size list, deferred deletes
 * just loads the map for now
 */
void read_checkpoint(char *buf, size_t len)
{
    hdr *h = (hdr*)buf;
    assert(h->type == LSVD_CKPT);
    assert(h->hdr_sectors*512 <= len);

    ckpt_hdr *ch = (ckpt_hdr*)(h+1);
    ckpt_mapentry *m = (ckpt_mapentry*)(buf + ch->map_offset),
	*map_end = (ckpt_mapentry*)(buf + ch->map_offset + ch->map_len);

    for (; m < map_end; m++)
	object_map.update(m->lba, m->lba+m->len,
			  (extmap::obj_offset){.obj = (uint64_t) m->obj,
			      .offset = (uint64_t)m->offset});
}

/* TODO: object list
 */
void write_checkpoint(void)
{
    std::vector<ckpt_mapentry> entries;

    std::unique_lock<std::mutex> lk(m);
    for (auto it = object_map.begin(); it != object_map.end(); it++) {
	auto [base, limit, ptr] = it->vals();
	entries.push_back((ckpt_mapentry){.lba = base, .len = limit-base,
		    .obj = (uint32_t)ptr.obj, .offset = (uint32_t)ptr.offset});
    }
    uint32_t seq = batch_seq++;
    size_t map_bytes = entries.size() * sizeof(ckpt_mapentry);
    size_t hdr_bytes = sizeof(hdr) + sizeof(ckpt_hdr);
    uint32_t sectors = div_round_up(hdr_bytes + sizeof(seq) + map_bytes, 512);
    object_hdrsz[seq] = sectors;
    lk.unlock();

    char *buf = (char*)calloc(hdr_bytes, 1);

    hdr *h = (hdr*)buf;
    *h = (hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
	       .type = LSVD_CKPT, .seq = seq, .hdr_sectors = sectors,
	       .data_sectors = 0};
    memcpy(h->vol_uuid, my_uuid, sizeof(uuid_t));
    ckpt_hdr *ch = (ckpt_hdr*)(h+1);
    *ch = (ckpt_hdr){.ckpts_offset = sizeof(hdr)+sizeof(ckpt_hdr),
		     .ckpts_len = sizeof(seq),
		     .objs_offset = 0, .objs_len = 0,
		     .deletes_offset = 0, .deletes_len = 0,
		     .map_offset = sizeof(hdr)+sizeof(ckpt_hdr)+sizeof(seq),
		     .map_len = (uint32_t)map_bytes};

    iovec iov[] = {{.iov_base = buf, .iov_len = hdr_bytes},
		   {.iov_base = (char*)&seq, .iov_len = sizeof(seq)},
		   {.iov_base = (char*)entries.data(), map_bytes}};
    write_object(seq, iov, 3);
}

// returns 1 beyond the last object found
//
uint32_t find_most_recent(uint32_t seq)
{
    char buf[4096];
    for (uint32_t i = seq; ; i++) {
	auto name = std::string(prefix) + "." + hex(i);
	if (read_object(i, buf, 0, sizeof(buf)) <= 0)
	    return i;
	hdr *h = (hdr*)buf;
	if (h->magic != LSVD_MAGIC || h->version != 1 || h->seq != i)
	    return i;
	object_hdrsz[i] = h->hdr_sectors;
    }
    return seq;
}
    
// returns next sequence number
uint32_t roll_forward(uint32_t seq)
{
    char buf[64*1024];
    for (uint32_t i = seq; ; i++) {
	auto name = std::string(prefix) + "." + hex(i);
	if (read_object(i, buf, 0, 64*1024) <= 0)
	    return i;
	hdr *h = (hdr*)buf;
	if (h->magic != LSVD_MAGIC || h->version != 1 || h->seq != i)
	    return i;
	object_hdrsz[i] = h->hdr_sectors;
	
	if (h->type == LSVD_DATA)
	    read_data_map(buf, sizeof(buf));
	else if (h->type == LSVD_CKPT)
	    read_checkpoint(buf, sizeof(buf));
	else
	    return i;
    }
    return 0;
}

std::queue<std::thread> pool;
static bool running;

void worker_thread(void)
{
    while (true) {
	batch *b;
	std::unique_lock<std::mutex> lock(m);

	while (work_queue.empty() && running)
	    cv.wait(lock);
	if (!running)
	    return;
	b = work_queue.front();
	work_queue.pop();

	int sectors = div_round_up(b->hdrlen(), 512);
	object_hdrsz[b->seq] = sectors;
	object_size[b->seq] = b->len / 512;
	lock.unlock();

	char *hdr = (char*)calloc(sectors*512, 1);
	make_hdr(hdr, b);
	iovec iov[2] = {{hdr, (size_t)(sectors*512)}, {b->buf, b->len}};
	write_object(b->seq, iov, 2);
	free(hdr);
	
	std::unique_lock<std::mutex> lock2(m);
	in_mem_objects.erase(b->seq);
	batches.push(b);
	lock2.unlock();
    }
}


ssize_t init(char *name, int nthreads)
{
    ssize_t bytes = read_super(name);
    if (bytes < 0)
      return bytes;
    // todo: ignore checkpoints for now
    batch_seq = roll_forward(batch_seq);
    running = true;
    
    for (int i = 0; i < nthreads; i++) 
      pool.push(std::thread(worker_thread));
    return bytes;
}

void lsvd_shutdown(void)
{
    running = false;
    std::unique_lock<std::mutex> lk(m);
    cv.notify_all();
    lk.unlock();

    while (!pool.empty()) {
	pool.front().join();
	pool.pop();
    }
}

ssize_t lsvd_write(size_t offset, size_t len, const char *buf)
{
    const std::unique_lock<std::mutex> lock(m);
    if (current_batch && current_batch->len + len > current_batch->max) {
	work_queue.push(current_batch);
	current_batch = NULL;
	cv.notify_one();
    }
    if (current_batch == NULL) {
	if (batches.empty())
	    current_batch = new batch(BATCH_SIZE);
	else {
	    current_batch = batches.top();
	    batches.pop();	// f-ing C++ stacks
	}
	current_batch->reset();
	in_mem_objects[current_batch->seq] = current_batch->buf;
    }

    uint64_t sector_offset = current_batch->len / 512,
	lba = offset/512, limit = (offset+len)/512;
    object_map.update(lba, limit,
		      (extmap::obj_offset){.obj = (uint64_t) current_batch->seq, .offset = sector_offset});
    char *ptr = current_batch->buf + current_batch->len;
    current_batch->len += len;
    current_batch->entries.push_back((data_map){.lba = offset/512, .len = len/512});
    // should I unlock here???
    
    memcpy(ptr, buf, len);

    return len;
}

int lsvd_flush(void)
{
    const std::unique_lock<std::mutex> lock(m);
    int val = 0;
    if (current_batch && current_batch->len > 0) {
	val = current_batch->seq;
	work_queue.push(current_batch);
	current_batch = NULL;
	cv.notify_one();
	if (batches.empty())
	    current_batch = new batch(BATCH_SIZE);
	else {
	    current_batch = batches.top();
	    batches.pop();	// f-ing C++ stacks
	}
	current_batch->reset();
	in_mem_objects[current_batch->seq] = current_batch->buf;
    }
    return val;
}

ssize_t lsvd_read(size_t offset, size_t len, char *buf)
{
    uint64_t base = offset / 512;
    uint64_t sectors = len / 512, limit = base + sectors;

    if (object_map.size() == 0) {
	memset(buf, 0, len);
	return len;
    }
    
    /* object number, offset (bytes), length (bytes) */
    std::vector<std::tuple<int, size_t, size_t>> regions;
    std::unique_lock<std::mutex> lock(m);

    auto prev = base;
    char *ptr = buf;
    for (auto it = object_map.lookup(base); it != object_map.end() && it->base() < limit; it++) {
        auto [_base, _limit, oo] = it->vals(base, limit);
	if (_base > prev) {	// unmapped
	    size_t _len = (_base - prev)*512;
	    regions.push_back(std::tuple(-1, 0, _len));
	    ptr += _len;
	}
	size_t _len = (_limit - _base) * 512,
	    _offset = oo.offset * 512;
	int obj = oo.obj;
	if (in_mem_objects.find(obj) != in_mem_objects.end()) {
	    memcpy((void*)ptr, in_mem_objects[obj]+_offset, _len);
	    obj = -2;
	}
	regions.push_back(std::tuple(obj, _offset, _len));
	ptr += _len;
	prev = _limit;
    }
    lock.unlock();

    ptr = buf;
    for (auto [obj, _offset, _len] : regions) {
	if (obj == -1)
	    memset(ptr, 0, _len);
	else if (obj == -2)
	    /* skip */;
	else
	    read_object(obj, ptr, _offset + object_hdrsz[obj]*512, _len);
	ptr += _len;
    }
    
    return ptr - buf;
}

extern "C" int c_read(char*, uint64_t, uint32_t, struct bdus_ctx*);
extern "C" int c_write(const char*, uint64_t, uint32_t, struct bdus_ctx*);
extern "C" int c_flush(struct bdus_ctx*);
extern "C" ssize_t c_init(char*, int);
extern "C" int c_size(void);
extern "C" void c_shutdown(void);

void c_shutdown(void)
{
    lsvd_shutdown();
}

int c_flush(struct bdus_ctx* ctx)
{
    return lsvd_flush();
}

ssize_t c_init(char *name, int n)
{
    return init(name, n);
}

int c_size(void)
{
    return object_map.size();
}

int c_read(char *buffer, uint64_t offset, uint32_t size, struct bdus_ctx *ctx)
{
    size_t val = lsvd_read(offset, size, buffer);
    return val < 0 ? -1 : 0;
}

int c_write(const char *buffer, uint64_t offset, uint32_t size, struct bdus_ctx *ctx)
{
    size_t val = lsvd_write(offset, size, buffer);
    return val < 0 ? -1 : 0;
}

extern "C" int dbg_inmem(int max, int *list);

int dbg_inmem(int max, int *list)
{
    int i = 0;
    for (auto it = in_mem_objects.begin(); i < max && it != in_mem_objects.end(); it++)
	list[i++] = it->first;
    return i;
}

struct tuple {
    int base;
    int limit;
    int obj;
    int offset;
};

extern "C" int dbg_getmap(int, int, int, struct tuple*);
int dbg_getmap(int base, int limit, int max, struct tuple *t)
{
    int i = 0;
    for (auto it = object_map.lookup(base);
	 i < max && it != object_map.end() && it->base() < (uint64_t)limit; it++) {
        auto [_base, _limit, oo] = it->vals(base, limit);
	t[i++] = (struct tuple){.base = (int)_base, .limit = (int)_limit,
				.obj = (int)oo.obj, .offset = (int)oo.offset};
    }
    return i;
}

extern "C" void dbg_checkpoint(void);
void dbg_checkpoint(void)
{
    write_checkpoint();
}

