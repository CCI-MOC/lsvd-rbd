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

#include <unistd.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <string.h>

/* make this atomic? */
int batch_seq = 1;
const int BATCH_SIZE = 8 * 1024 * 1024;


static int div_round_up(int n, int m)
{
    return (n + m - 1) / m;
}

static int round_up(int n, int m)
{
#if 0
    n += (m - 1);
    n -= (n % m);
    return n;
#else
    return div_round_up(n, m) * m;
#endif
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
    // h->vol_uuid
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

char *prefix = (char*)"/tmp/bkt/obj";

void write_object(int seq, iovec *iov, int iovcnt)
{
    char name[128];
    sprintf(name, "%s.%05x", prefix, seq);
    int fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0777);
    if (fd < 0)
	perror("create"), exit(1);
    writev(fd, iov, iovcnt);
    close(fd);
}

size_t read_object(int seq, char *buf, size_t offset, size_t len)
{
    char name[128];
    sprintf(name, "%s.%05x", prefix, seq);
    int fd = open(name, O_RDONLY);
    if (fd < 0)
	perror("open_read"), exit(1);
    return pread(fd, buf, len, offset + object_hdrsz[seq]*512);
}

void *worker_thread(void* tmp)
{
    while (true) {
	batch *b;
	std::unique_lock<std::mutex> lock(m);

	while (work_queue.empty())
	    cv.wait(lock);
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
    return NULL;
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
	    read_object(obj, ptr, _offset, _len);
	ptr += _len;
    }
    
    return ptr - buf;
}

extern "C" int c_read(char*, uint64_t, uint32_t, struct bdus_ctx*);
extern "C" int c_write(const char*, uint64_t, uint32_t, struct bdus_ctx*);

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

extern "C" int dbg_inmem(int *list);

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

int c_getmap(int base, int limit, int max, struct tuple *t)
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
