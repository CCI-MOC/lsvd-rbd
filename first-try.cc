/*
 * file:        first-try.cc
 * description: first pass at a userspace block-on-object layer
 */

#include "extent.cc"
#include "objects.cc"
#include "journal2.cc"

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
#include <stdexcept>

#include <unistd.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <string.h>

/* make this atomic? */
int batch_seq;
int last_ckpt;
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
    ~batch(){
	free((void*)buf);
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

std::string hex(uint32_t n)
{
    std::stringstream stream;
    stream << std::setfill ('0') << std::setw(8) << std::hex << n;
    return stream.str();
}

class backend {
public:
    virtual ssize_t write_object(const char *name, iovec *iov, int iovcnt) = 0;
    virtual ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt) = 0;
    virtual ssize_t read_object(const char *name, char *buf, size_t offset, size_t len) = 0;
    virtual ssize_t read_numbered_object(int seq, char *buf, size_t offset, size_t len) = 0;
    virtual std::string object_name(int seq) = 0;
};

backend *io;

/* simple backend that uses files in a directory. 
 * good for debugging and testing
 */
class file_backend : public backend {
    char *prefix;
public:
    file_backend(char *_prefix) {
	prefix = strdup(_prefix);
    }
    ~file_backend() {
	free((void*)prefix);
    }
    ssize_t write_object(const char *name, iovec *iov, int iovcnt) {
	int fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0777);
	if (fd < 0)
	    return -1;
	auto val = writev(fd, iov, iovcnt);
	close(fd);
	return val;
    }
    ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt) {
	auto name = std::string(prefix) + "." + hex(seq);
	return write_object(name.c_str(), iov, iovcnt);
    }
    ssize_t read_object(const char *name, char *buf, size_t offset, size_t len) {
	int fd = open(name, O_RDONLY);
	if (fd < 0)
	    return -1;
	auto val = pread(fd, buf, len, offset);
	close(fd);
	return val;
    }
    ssize_t read_numbered_object(int seq, char *buf, size_t offset, size_t len) {
	auto name = std::string(prefix) + "." + hex(seq);
	return read_object(name.c_str(), buf, offset, len);
    }
    std::string object_name(int seq) {
	return std::string(prefix) + "." + hex(seq);
    }
};    

char *read_object_hdr(const char *name, bool fast) {
    hdr *h = (hdr*)malloc(4096);
    if (io->read_object(name, (char*)h, 0, 4096) < 0)
	goto fail;
    if (fast)
	return (char*)h;
    if (h->hdr_sectors > 8) {
	h = (hdr*)realloc(h, h->hdr_sectors * 512);
	if (io->read_object(name, (char*)h, 0, h->hdr_sectors*512) < 0)
	    goto fail;
    }
    return (char*)h;
fail:
    free((char*)h);
    return NULL;
}


// merge buffer and free list are protected by merge_lock
//
std::mutex              m;
std::condition_variable cv;
std::queue<batch*>      work_queue;

batch              *current_batch;
std::stack<batch*>  batches;
extmap::objmap      object_map;
std::map<int,char*> in_mem_objects;

/* info on all live objects - all sizes in sectors
 */
struct obj_info {
    uint32_t hdr;
    uint32_t data;
    uint32_t live;
    int      type;
};
std::map<int,obj_info> object_info;

char      *super;
hdr       *super_h;
super_hdr *super_sh;
size_t     super_len;

/* these all should probably be combined with the stuff in objects.cc to create
 * object classes that serialize and de-serialize themselves. Sometime, maybe.
 */

template <class T>
void decode_offset_len(char *buf, size_t offset, size_t len, std::vector<T> &vals)
{
    T *p = (T*)(buf + offset), *end = (T*)(buf + offset + len);
    for (; p < end; p++)
	vals.push_back(*p);
}

/* clone_info is variable-length, so we need to pass back pointers 
 * rather than values. That's OK because we allocate superblock permanently
 */
typedef clone_info *clone_p;
ssize_t read_super(char *name, std::vector<uint32_t> &ckpts,
		   std::vector<clone_p> &clones, std::vector<snap_info> &snaps)
{
    super = read_object_hdr(name, false);
    super_h = (hdr*)super;
    super_len = super_h->hdr_sectors * 512;

    if (super_h->magic != LSVD_MAGIC || super_h->version != 1 ||
	super_h->type != LSVD_SUPER)
	return -1;
    memcpy(my_uuid, super_h->vol_uuid, sizeof(uuid_t));

    super_sh = (super_hdr*)(super_h+1);

    decode_offset_len<uint32_t>(super, super_sh->ckpts_offset, super_sh->ckpts_len, ckpts);
    decode_offset_len<snap_info>(super, super_sh->snaps_offset, super_sh->snaps_len, snaps);

    // this one stores pointers, not values...
    clone_info *p_clone = (clone_info*)(super + super_sh->clones_offset),
	*end_clone = (clone_info*)(super + super_sh->clones_offset + super_sh->clones_len);
    for (; p_clone < end_clone; p_clone++)
	clones.push_back(p_clone);
    
    return super_sh->vol_size * 512;
}

ssize_t read_data_hdr(int seq, hdr &h, data_hdr &dh, std::vector<uint32_t> &ckpts,
		   std::vector<obj_cleaned> &cleaned, std::vector<data_map> &dmap)
{
    auto name = io->object_name(seq);
    char *buf = read_object_hdr(name.c_str(), false);
    if (buf == NULL)
	return -1;
    hdr      *tmp_h = (hdr*)buf;
    data_hdr *tmp_dh = (data_hdr*)(tmp_h+1);
    if (tmp_h->type != LSVD_DATA) {
	free(buf);
	return -1;
    }
    
    h = *tmp_h;
    dh = *tmp_dh;

    decode_offset_len<uint32_t>(buf, tmp_dh->ckpts_offset, tmp_dh->ckpts_len, ckpts);
    decode_offset_len<obj_cleaned>(buf, tmp_dh->objs_cleaned_offset, tmp_dh->objs_cleaned_len, cleaned);
    decode_offset_len<data_map>(buf, tmp_dh->map_offset, tmp_dh->map_len, dmap);

    free(buf);
    return 0;
}

ssize_t read_checkpoint(int seq, std::vector<uint32_t> &ckpts, std::vector<ckpt_obj> &objects, 
			std::vector<deferred_delete> &deletes, std::vector<ckpt_mapentry> &dmap)
{
    auto name = io->object_name(seq);
    char *buf = read_object_hdr(name.c_str(), false);
    if (buf == NULL)
	return -1;
    hdr      *h = (hdr*)buf;
    ckpt_hdr *ch = (ckpt_hdr*)(h+1);
    if (h->type != LSVD_CKPT) {
	free(buf);
	return -1;
    }

    decode_offset_len<uint32_t>(buf, ch->ckpts_offset, ch->ckpts_len, ckpts);
    decode_offset_len<ckpt_obj>(buf, ch->objs_offset, ch->objs_len, objects);
    decode_offset_len<deferred_delete>(buf, ch->deletes_offset, ch->deletes_len, deletes);
    decode_offset_len<ckpt_mapentry>(buf, ch->map_offset, ch->map_len, dmap);

    free(buf);
    return 0;
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
int write_checkpoint(int seq)
{
    std::vector<ckpt_mapentry> entries;
    std::unique_lock<std::mutex> lk(m);
    last_ckpt = seq;
    for (auto it = object_map.begin(); it != object_map.end(); it++) {
	auto [base, limit, ptr] = it->vals();
	entries.push_back((ckpt_mapentry){.lba = base, .len = limit-base,
		    .obj = (uint32_t)ptr.obj, .offset = (uint32_t)ptr.offset});
    }
    size_t map_bytes = entries.size() * sizeof(ckpt_mapentry);
    size_t hdr_bytes = sizeof(hdr) + sizeof(ckpt_hdr);
    uint32_t sectors = div_round_up(hdr_bytes + sizeof(seq) + map_bytes, 512);
    object_info[seq] = (obj_info){.hdr = sectors, .data = 0, .live = 0, .type = LSVD_CKPT};
    lk.unlock();

    char *buf = (char*)calloc(hdr_bytes, 1);

    hdr *h = (hdr*)buf;
    *h = (hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
	       .type = LSVD_CKPT, .seq = (uint32_t)seq, .hdr_sectors = sectors,
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
    io->write_numbered_object(seq, iov, 3);
    return seq;
}


std::queue<std::thread> pool;
static bool running;

int make_hdr(char *buf, batch *b)
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
    dh->ckpts_offset = sizeof(*h) + sizeof(*dh);
    dh->ckpts_len = sizeof(uint32_t);
    dh->map_offset = sizeof(*h) + sizeof(*dh) + sizeof(uint32_t);
    dh->map_len = b->entries.size() * sizeof(data_map);

    uint32_t *p_ckpt = (uint32_t*)(dh+1);
    *p_ckpt = last_ckpt;
    
    data_map *dm = (data_map*)(p_ckpt+1);
    for (auto e : b->entries)
	*dm++ = e;

    return (char*)dm - (char*)buf;
}

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

	uint32_t sectors = div_round_up(b->hdrlen(), 512);
	object_info[b->seq] = (obj_info){.hdr = sectors, .data = (uint32_t)(b->len / 512),
					 .live = (uint32_t)(b->len / 512), .type = LSVD_DATA};
	lock.unlock();

	char *hdr = (char*)calloc(sectors*512, 1);
	make_hdr(hdr, b);
	iovec iov[2] = {{hdr, (size_t)(sectors*512)}, {b->buf, b->len}};
	io->write_numbered_object(b->seq, iov, 2);
	free(hdr);
	
	std::unique_lock<std::mutex> lock2(m);
	in_mem_objects.erase(b->seq);
	batches.push(b);
	lock2.unlock();
    }
}

// returns 1 beyond the last object found
//
uint32_t find_most_recent(uint32_t seq)
{
    char buf[4096];
    for (uint32_t i = seq; ; i++) {
	if (io->read_numbered_object(i, buf, 0, sizeof(buf)) <= 0)
	    return i;
	hdr *h = (hdr*)buf;
	if (h->magic != LSVD_MAGIC || h->version != 1 || h->seq != i)
	    return i;
	object_info[i] = (struct obj_info){.hdr = h->hdr_sectors, .data = h->data_sectors,
					   .live = h->data_sectors, .type = (int)h->type};
    }
    return seq;
}
    
// returns next sequence number
int roll_forward(uint32_t seq)
{
    char buf[64*1024];
    for (uint32_t i = seq; ; i++) {
	if (io->read_numbered_object(i, buf, 0, 64*1024) <= 0)
	    return i;
	hdr *h = (hdr*)buf;
	if (h->magic != LSVD_MAGIC || h->version != 1 || h->seq != i)
	    return i;
	object_info[i] = (obj_info){.hdr = h->hdr_sectors, .data = h->data_sectors,
				    .live = h->data_sectors, .type = (int)h->type};

	if (h->type == LSVD_DATA)
	    read_data_map(buf, sizeof(buf));
	else if (h->type == LSVD_CKPT)
	    read_checkpoint(buf, sizeof(buf));
	else
	    return i;
    }
    return 0;
}


ssize_t init(char *name, int nthreads)
{
    io = new file_backend(name);
    
    /* For testing we want to get the in-memory state back to the beginning.
     * TODO: these should be class fields, not globals
     */
    batch_seq = 1;
    while (!batches.empty()) {
	delete batches.top();
	batches.pop();
    }
    delete current_batch;
    current_batch = NULL;
    object_map.reset();
    in_mem_objects.erase(in_mem_objects.begin(), in_mem_objects.end());
    object_info.erase(object_info.begin(), object_info.end());

    std::vector<uint32_t>  ckpts;
    std::vector<clone_p>   clones;
    std::vector<snap_info> snaps;
    ssize_t bytes = read_super(name, ckpts, clones, snaps);
    if (bytes < 0)
      return bytes;
    batch_seq = super_sh->next_obj;
    
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

/* returns sequence number of ckpt
 */
int lsvd_checkpoint(void)
{
    std::unique_lock<std::mutex> lk(m);
    if (current_batch && current_batch->len > 0) {
	work_queue.push(current_batch);
	current_batch = NULL;
	cv.notify_one();
    }
    int seq = batch_seq++;
    lk.unlock();
    return write_checkpoint(seq);
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
	    io->read_numbered_object(obj, ptr, _offset + object_info[obj].hdr*512, _len);
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

extern "C" int dbg_checkpoint(void);
int dbg_checkpoint(void)
{
    return lsvd_checkpoint();
}

