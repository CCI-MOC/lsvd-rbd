#include <queue>
#include <mutex>
#include <condition_variable>
#include <shared_mutex>
#include <string>
#include <unistd.h>
#include "base_functions.h"
#include <thread>
#include "smartiov.h"
#include "extent.h"
#include <atomic>
#include <stack>
#include <map>

#include "misc_cache.h"
#include "backend.h"
#include "objects.h"
#include "translate.h"

    void batch::reset(void) {
        len = 0;
        entries.resize(0);
        seq = batch_seq++;
    }

    void batch::append_iov(uint64_t lba, iovec *iov, int iovcnt) {
        char *ptr = buf + len;
        for (int i = 0; i < iovcnt; i++) {
            memcpy(ptr, iov[i].iov_base, iov[i].iov_len);
            entries.push_back((data_map){lba, iov[i].iov_len / 512});
            ptr += iov[i].iov_len;
            len += iov[i].iov_len;
            lba += iov[i].iov_len / 512;
        }
    }

    int batch::hdrlen(void) {
        return sizeof(hdr) + sizeof(data_hdr) + entries.size() * sizeof(data_map);
    }

    char *translate::read_object_hdr(const char *name, bool fast) {
	hdr *h = (hdr*)malloc(4096);
	if (translate::io->read_object(name, (char*)h, 4096, 0) < 0)
	    goto fail;
	if (fast)
	    return (char*)h;
	if (h->hdr_sectors > 8) {
	    h = (hdr*)realloc(h, h->hdr_sectors * 512);
	    if (translate::io->read_object(name, (char*)h, h->hdr_sectors*512, 0) < 0)
		goto fail;
	}
	return (char*)h;
    fail:
	free((char*)h);
	return NULL;
    }

    ssize_t translate::read_super(const char *name, std::vector<uint32_t> &ckpts,
		       std::vector<clone_p> &clones, std::vector<snap_info> &snaps) {
	super = translate::read_object_hdr(name, false);
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

    ssize_t translate::read_data_hdr(int seq, hdr &h, data_hdr &dh, std::vector<uint32_t> &ckpts,
		       std::vector<obj_cleaned> &cleaned, std::vector<data_map> &dmap) {
	auto name = translate::io->object_name(seq);
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

    ssize_t translate::read_checkpoint(int seq, std::vector<uint32_t> &ckpts, std::vector<ckpt_obj> &objects, 
			    std::vector<deferred_delete> &deletes, std::vector<ckpt_mapentry> &dmap) {
	auto name = translate::io->object_name(seq);
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

    void translate::do_completions(int32_t seq, void *closure) {
	std::unique_lock lk(m_c);
	if (seq == next_completion) {
	    next_completion++;
	    call_wrapped(closure);
	}
	else
	    completions.emplace(seq, closure);
	auto it = completions.begin();
	while (it != completions.end() && it->first == next_completion) {
	    call_wrapped(it->second);
	    it = completions.erase(it);
	    next_completion++;
	}
	lk.unlock();
	cv_c.notify_all();
    }

    int translate::write_checkpoint(int seq) {
	std::vector<ckpt_mapentry> entries;
	std::vector<ckpt_obj> objects;
	
	std::unique_lock lk(map->m);
	last_ckpt = seq;
	for (auto it = map->map.begin(); it != map->map.end(); it++) {
	    auto [base, limit, ptr] = it->vals();
	    entries.push_back((ckpt_mapentry){.lba = base, .len = limit-base,
			.obj = (int32_t)ptr.obj, .offset = (int32_t)ptr.offset});
	}
	lk.unlock();
	size_t map_bytes = entries.size() * sizeof(ckpt_mapentry);

	std::unique_lock lk2(m);
	for (auto it = object_info.begin(); it != object_info.end(); it++) {
	    auto obj_num = it->first;
	    auto [hdr, data, live, type] = it->second;
	    if (type == LSVD_DATA)
		objects.push_back((ckpt_obj){.seq = (uint32_t)obj_num, .hdr_sectors = hdr,
			    .data_sectors = data, .live_sectors = live});
	}

	size_t objs_bytes = objects.size() * sizeof(ckpt_obj);
	size_t hdr_bytes = sizeof(hdr) + sizeof(ckpt_hdr);
	uint32_t sectors = div_round_up(hdr_bytes + sizeof(seq) + map_bytes + objs_bytes, 512);
	object_info[seq] = (obj_info){.hdr = sectors, .data = 0, .live = 0, .type = LSVD_CKPT};
	lk2.unlock();

	char *buf = (char*)calloc(hdr_bytes, 1);
	hdr *h = (hdr*)buf;
	*h = (hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
		   .type = LSVD_CKPT, .seq = (uint32_t)seq, .hdr_sectors = sectors,
		   .data_sectors = 0};
	memcpy(h->vol_uuid, my_uuid, sizeof(uuid_t));
	ckpt_hdr *ch = (ckpt_hdr*)(h+1);

	uint32_t o1 = sizeof(hdr)+sizeof(ckpt_hdr), o2 = o1 + sizeof(seq), o3 = o2 + objs_bytes;
	*ch = (ckpt_hdr){.ckpts_offset = o1, .ckpts_len = sizeof(seq),
			 .objs_offset = o2, .objs_len = o3-o2,
			 .deletes_offset = 0, .deletes_len = 0,
			 .map_offset = o3, .map_len = (uint32_t)map_bytes};

	iovec iov[] = {{.iov_base = buf, .iov_len = hdr_bytes},
		       {.iov_base = (char*)&seq, .iov_len = sizeof(seq)},
		       {.iov_base = (char*)objects.data(), objs_bytes},
		       {.iov_base = (char*)entries.data(), map_bytes}};

	{
	    std::unique_lock lk_complete(m_c);
	    while (next_completion < seq)
		cv_c.wait(lk_complete);
	    next_completion++;
	}
	translate::io->write_numbered_object(seq, iov, 4);

	size_t offset = sizeof(*super_h) + sizeof(*super_sh);

	lk2.lock();
	super_sh->ckpts_offset = offset;
	super_sh->ckpts_len = sizeof(seq);
	*(int*)(super + offset) = seq;
	struct iovec iov2 = {super, 4096};
	translate::io->write_object(super_name, &iov2, 1);
	return seq;
    }

    int translate::make_hdr(char *buf, batch *b) {
	hdr *h = (hdr*)buf;
	data_hdr *dh = (data_hdr*)(h+1);
	uint32_t o1 = sizeof(*h) + sizeof(*dh), l1 = sizeof(uint32_t),
	    o2 = o1 + l1, l2 = b->entries.size() * sizeof(data_map),
	    hdr_bytes = o2 + l2;
	sector_t hdr_sectors = div_round_up(hdr_bytes, 512);
	
	*h = (hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
		   .type = LSVD_DATA, .seq = (uint32_t)b->seq,
		   .hdr_sectors = (uint32_t)hdr_sectors,
		   .data_sectors = (uint32_t)(b->len / 512)};
	memcpy(h->vol_uuid, my_uuid, sizeof(uuid_t));

	*dh = (data_hdr){.last_data_obj = (uint32_t)b->seq, .ckpts_offset = o1, .ckpts_len = l1,
			 .objs_cleaned_offset = 0, .objs_cleaned_len = 0,
			 .map_offset = o2, .map_len = l2};

	uint32_t *p_ckpt = (uint32_t*)(dh+1);
	*p_ckpt = last_ckpt;

	data_map *dm = (data_map*)(p_ckpt+1);
	for (auto e : b->entries)
	    *dm++ = e;

	return (char*)dm - (char*)buf;
    }

    sector_t translate::make_gc_hdr(char *buf, uint32_t seq, sector_t sectors,
			 data_map *extents, int n_extents) {
	hdr *h = (hdr*)buf;
	data_hdr *dh = (data_hdr*)(h+1);
	uint32_t o1 = sizeof(*h) + sizeof(*dh), l1 = sizeof(uint32_t),
	    o2 = o1 + l1, l2 = n_extents * sizeof(data_map),
	    hdr_bytes = o2 + l2;
	sector_t hdr_sectors = div_round_up(hdr_bytes, 512);

	*h = (hdr){.magic = LSVD_MAGIC, .version = 1, .vol_uuid = {0},
		   .type = LSVD_DATA, .seq = seq,
		   .hdr_sectors = (uint32_t)hdr_sectors,
		   .data_sectors = (uint32_t)sectors};
	memcpy(h->vol_uuid, my_uuid, sizeof(uuid_t));

	*dh = (data_hdr){.last_data_obj = seq, .ckpts_offset = o1, .ckpts_len = l1,
			 .objs_cleaned_offset = 0, .objs_cleaned_len = 0,
			 .map_offset = o2, .map_len = l2};

	uint32_t *p_ckpt = (uint32_t*)(dh+1);
	*p_ckpt = last_ckpt;

	data_map *dm = (data_map*)(p_ckpt+1);
	for (int i = 0; i < n_extents; i++)
	    *dm++ = extents[i];

	assert(hdr_bytes == ((char*)dm - buf));
	memset(buf + hdr_bytes, 0, 512*hdr_sectors - hdr_bytes); // valgrind

	return hdr_sectors;
    }

    void translate::do_gc(std::unique_lock<std::mutex> &lk) {
	//printf("\n** DO_GC **\n");
	gc_cycles++;
	int max_obj = batch_seq;

	std::set<std::tuple<double,int,int>> utilization;
	for (auto p : object_info)  {
	    if (in_mem_objects.find(p.first) != in_mem_objects.end())
		continue;
	    double rho = 1.0 * p.second.live / p.second.data;
	    sector_t sectors = p.second.hdr + p.second.data;
	    utilization.insert(std::make_tuple(rho, p.first, sectors));
	}

	const double threshold = 0.55;
	std::vector<std::pair<int,int>> objs_to_clean;
	for (auto [u, o, n] : utilization) {
	    if (u > threshold)
		break;
	    if (objs_to_clean.size() > 32)
		break;
	    objs_to_clean.push_back(std::make_pair(o, n));
	}
	if (objs_to_clean.size() == 0) 
	    return;
	
	/* find all live extents in objects listed in objs_to_clean
	 */
	std::vector<bool> bitmap(max_obj+1);
	for (auto [i,_xx] : objs_to_clean)
	    bitmap[i] = true;

	extmap::objmap live_extents;
	for (auto it = map->map.begin(); it != map->map.end(); it++) {
	    auto [base, limit, ptr] = it->vals();
	    if (bitmap[ptr.obj])
		live_extents.update(base, limit, ptr);
	}
	lk.unlock();

	if (live_extents.size() > 0) {
	    /* temporary file, delete on close. Should use mkstemp().
	     * need a proper config file so we can configure all this crap...
	     */
	    char temp[] = "/mnt/nvme/lsvd/gc.XXXXXX";
	    int fd = mkstemp(temp);
	    unlink(temp);

	    /* read all objects in completely. Someday we can check to see whether
	     * (a) data is already in cache, or (b) sparse reading would be quicker
	     */
	    extmap::cachemap file_map;
	    sector_t offset = 0;
	    //char *buf = (char*)malloc(10*1024*1024);
            char *buf = (char*)aligned_alloc(512, 10*1024*1024);

	    for (auto [i,sectors] : objs_to_clean) {
		//printf("\ngc read %d\n", i);
		translate::io->read_numbered_object(i, buf, sectors*512, 0);
		gc_sectors_read += sectors;
		extmap::obj_offset _base = {i, 0}, _limit = {i, sectors};
		file_map.update(_base, _limit, offset);
		write(fd, buf, sectors*512);
		offset += sectors;
	    }
	    free(buf);

	    struct _extent {
		int64_t base;
		int64_t limit;
		extmap::obj_offset ptr;
	    };
	    std::vector<_extent> all_extents;
	    for (auto it = live_extents.begin(); it != live_extents.end(); it++) {
		auto [base, limit, ptr] = it->vals();
		all_extents.push_back((_extent){base, limit, ptr});
	    }
	    //live_extents.reset();	// give up memory early
	    //printf("\ngc: to clean: %ld extents\n", all_extents.size());
	
	    while (all_extents.size() > 0) {
		sector_t sectors = 0, max = 16 * 1024; // 8MB
		char *hdr = (char*)malloc(1024*32);	// 8MB / 4KB = 2K extents = 16KB

		auto it = all_extents.begin();
		while (it != all_extents.end() && sectors < max) {
		    auto [base, limit, ptr] = *it++;
		    sectors += (limit - base);
		}
		std::vector<_extent> extents(std::make_move_iterator(all_extents.begin()),
					     std::make_move_iterator(it));
		all_extents.erase(all_extents.begin(), it);
	    
		/* lock the map while we read from the file. 
		 * TODO: read everything in, put it in a bufmap, and then go back and construct
		 * an iovec for the subset that's still valid.
		 */
		char *buf = (char*)aligned_alloc(512, sectors * 512);

		lk.lock();
		off_t byte_offset = 0;
		sector_t data_sectors = 0;
		auto obj_extents = new std::vector<data_map>();
	    
		for (auto [base, limit, ptr] : extents) {
		    for (auto it2 = map->map.lookup(base);
			 it2->base() < limit && it2 != map->map.end(); it2++) {
			auto [_base, _limit, _ptr] = it2->vals(base, limit);
			if (_ptr.obj != ptr.obj)
			    continue;
			sector_t _sectors = _limit - _base;
			size_t bytes = _sectors*512;
			auto it3 = file_map.lookup(_ptr);
			auto file_sector = it3->ptr();

			auto err = pread(fd, buf+byte_offset, bytes, file_sector*512);
			if (err != (ssize_t)bytes) {
			    printf("\n\n");
			    printf("%ld != %ld, obj=%ld end=%s\n", err, bytes, _ptr.obj,
				   it3 == file_map.end() ? "Y" : "n");
			    throw_fs_error("gc");
			}
			obj_extents->push_back((data_map){(uint64_t)_base, (uint64_t)_sectors});

			data_sectors += _sectors;
			byte_offset += bytes;
		    }
		}
		int32_t seq = batch_seq++;
		lk.unlock();

		gc_sectors_written += data_sectors;
	    
		int hdr_sectors = make_gc_hdr(hdr, seq, data_sectors,
					      obj_extents->data(), obj_extents->size());
		auto iovs = new smartiov;
		iovs->push_back((iovec){hdr, (size_t)hdr_sectors*512});
		iovs->push_back((iovec){buf, (size_t)byte_offset});
		//printf("\ngc write %d (%d)\n", seq, iovs->size() / 512);

		auto closure = wrap([this, hdr_sectors, obj_extents, buf, hdr, seq, iovs]{
			std::unique_lock lk(m);
			std::unique_lock objlock(map->m);
			auto offset = hdr_sectors;
			for (auto e : *obj_extents) {
			    extmap::obj_offset oo = {seq, offset};
			    map->map.update(e.lba, e.lba+e.len, oo, NULL);
			    offset += e.len;
			}
		    
			last_written = std::max(last_written, seq); // is this needed?
			cv.notify_all();

			delete obj_extents;
			delete iovs;
			free(buf);
			free(hdr);
			return true;
		    });
		auto closure2 = wrap([this, closure, seq]{
			do_completions(seq, closure);
			return true;
		    });
		translate::io->aio_write_numbered_object(seq, iovs->data(), iovs->size(),
					      call_wrapped, closure2);
	    }
	    close(fd);
	}
	
	for (auto [i, _xx] : objs_to_clean) {
	    translate::io->delete_numbered_object(i);
	    gc_deleted++;
	}
	lk.lock();
    }



    void translate::gc_thread(thread_pool<int> *p) {
	auto interval = std::chrono::milliseconds(100);
	sector_t trigger = 128 * 1024 * 2; // 128 MB
	const char *name = "gc";
	pthread_setname_np(pthread_self(), name);
	
	while (p->running) {
	    std::unique_lock lk(m);
	    p->cv.wait_for(lk, interval);
	    if (!p->running)
		return;
	    if (total_sectors - total_live_sectors < trigger)
		continue;
	    if (((double)total_live_sectors / total_sectors) > 0.6)
		continue;
	    do_gc(lk);
	}
    }
    
    void translate::worker_thread(thread_pool<batch*> *p) {
	while (p->running) {
	    std::unique_lock<std::mutex> lk(m);
	    batch *b;
	    if (!p->get_locked(lk, b))
		return;
	    uint32_t hdr_sectors = div_round_up(b->hdrlen(), 512);
	    object_info[b->seq] = (obj_info){.hdr = hdr_sectors, .data = (uint32_t)(b->len/512),
					     .live = (uint32_t)(b->len/512), .type = LSVD_DATA};
	    total_sectors += b->len/512;
	    total_live_sectors += b->len/512;
	    lk.unlock();

	    char *hdr = (char*)calloc(hdr_sectors*512, 1);
	    make_hdr(hdr, b);
	    auto iovs = new smartiov;
	    iovs->push_back((iovec){hdr, (size_t)(hdr_sectors*512)});
	    iovs->push_back((iovec){b->buf, b->len});

	    /* Note that we've already decremented object live counts when we copied the data
	     * into the batch buffer.
	     */
	    auto closure = wrap([this, hdr_sectors, hdr, iovs, b]{
		    std::unique_lock lk(m);
		    std::unique_lock objlock(map->m);

		    auto offset = hdr_sectors;
		    for (auto e : b->entries) {
			extmap::obj_offset oo = {b->seq, offset};
			map->map.update(e.lba, e.lba+e.len, oo, NULL);
			offset += e.len;
		    }

		    last_written = std::max(last_written, b->seq); // for flush()
		    cv.notify_all();
	    
		    in_mem_objects.erase(b->seq);
		    //printf("push\n");
		    batches.push(b);
		    objlock.unlock();
		    free(hdr);
		    delete iovs;
		    puts_outstanding--;
		    cv.notify_all();
		    return true;
		});
	    auto closure2 = wrap([this, closure, b]{
		    do_completions(b->seq, closure);
		    return true;
		});
	    puts_outstanding++;
	    translate::io->aio_write_numbered_object(b->seq, iovs->data(), iovs->size(),
					  call_wrapped, closure2);
	}
    }

    void translate::ckpt_thread(thread_pool<int> *p) {
	auto one_second = std::chrono::seconds(1);
	auto seq0 = batch_seq;
	const int ckpt_interval = 100;

	while (p->running) {
	    std::unique_lock<std::mutex> lk(m);
	    p->cv.wait_for(lk, one_second);
	    if (p->running && batch_seq - seq0 > ckpt_interval) {
		seq0 = batch_seq;
		lk.unlock();
		checkpoint();
	    }
	}
    }

    void translate::flush_thread(thread_pool<int> *p) {
	auto wait_time = std::chrono::milliseconds(500);
	auto timeout = std::chrono::seconds(2);
	auto t0 = std::chrono::system_clock::now();
	auto seq0 = batch_seq;

	while (p->running) {
	    std::unique_lock<std::mutex> lk(*p->m);
	    p->cv.wait_for(lk, wait_time);
	    if (p->running && current_batch && seq0 == batch_seq && current_batch->len > 0) {
		if (std::chrono::system_clock::now() - t0 > timeout) {
		    lk.unlock();
		    flush();
		}
	    }
	    else {
		seq0 = batch_seq;
		t0 = std::chrono::system_clock::now();
	    }
	}
    }

    int translate::checkpoint(void) {
	std::unique_lock<std::mutex> lk(m);
	if (current_batch && current_batch->len > 0) {
	    last_pushed = current_batch->seq;
	    workers.put_locked(current_batch);
	    current_batch = NULL;
	}
	if (map->map.size() == 0)
	    return 0;
	int seq = batch_seq++;
	lk.unlock();
	return write_checkpoint(seq);
    }

    int translate::flush(void) {
	std::unique_lock<std::mutex> lk(m);
	int val = 0;
	if (current_batch && current_batch->len > 0) {
	    val = current_batch->seq;
	    last_pushed = val;
	    workers.put_locked(current_batch);
	    current_batch = NULL;
	}
	while (last_written < last_pushed)
	    cv.wait(lk);
	return val;
    }

    ssize_t translate::init(const char *name, int nthreads, bool timedflush) {
	std::vector<uint32_t>  ckpts;
	std::vector<clone_p>   clones;
	std::vector<snap_info> snaps;

	super_name = strdup(name);
	ssize_t bytes = read_super(name, ckpts, clones, snaps);
	if (bytes < 0)
	  return bytes;
	batch_seq = super_sh->next_obj;

	int _ckpt = 1;
	for (auto ck : ckpts) {
	    ckpts.resize(0);
	    std::vector<ckpt_obj> objects;
	    std::vector<deferred_delete> deletes;
	    std::vector<ckpt_mapentry> entries;
	    if (read_checkpoint(ck, ckpts, objects, deletes, entries) < 0)
		return -1;
	    for (auto o : objects) {
		object_info[o.seq] = (obj_info){.hdr = o.hdr_sectors, .data = o.data_sectors,
					.live = o.live_sectors, .type = LSVD_DATA};
		total_sectors += o.data_sectors;
		total_live_sectors += o.live_sectors;
	    }
	    for (auto m : entries) {
		map->map.update(m.lba, m.lba + m.len,
				(extmap::obj_offset){.obj = m.obj, .offset = m.offset});
	    }
	    _ckpt = ck;
	}

	for (int i = _ckpt; ; i++) {
	    std::vector<uint32_t>    ckpts;
	    std::vector<obj_cleaned> cleaned;
	    std::vector<data_map>    entries;
	    hdr h; data_hdr dh;
	    batch_seq = i;
	    if (read_data_hdr(i, h, dh, ckpts, cleaned, entries) < 0)
		break;
	    object_info[i] = (obj_info){.hdr = h.hdr_sectors, .data = h.data_sectors,
					.live = h.data_sectors, .type = LSVD_DATA};
	    total_sectors += h.data_sectors;
	    total_live_sectors += h.data_sectors;
	    int offset = 0, hdr_len = h.hdr_sectors;
	    for (auto m : entries) {
		std::vector<extmap::lba2obj> deleted;
		extmap::obj_offset oo = {i, offset + hdr_len};
		map->map.update(m.lba, m.lba + m.len, oo, &deleted);
		offset += m.len;
		for (auto d : deleted) {
		    auto [base, limit, ptr] = d.vals();
		    object_info[ptr.obj].live -= (limit - base);
		    total_live_sectors -= (limit - base);
		}
	    }
	}
	next_completion = batch_seq;

	for (int i = 0; i < nthreads; i++) 
	    workers.pool.push(std::thread(&translate::worker_thread, this, &workers));
	misc_threads.pool.push(std::thread(&translate::ckpt_thread, this, &misc_threads));
	if (timedflush)
	    misc_threads.pool.push(std::thread(&translate::flush_thread, this, &misc_threads));
	misc_threads.pool.push(std::thread(&translate::gc_thread, this, &misc_threads));
	
	return bytes;
    }

    void translate::shutdown(void) {
    }

    ssize_t translate::writev(size_t offset, iovec *iov, int iovcnt) {
	std::unique_lock<std::mutex> lk(m);
	size_t len = iov_sum(iov, iovcnt);

	while (puts_outstanding >= 32)
	    cv.wait(lk);
	
	if (current_batch && current_batch->len + len > current_batch->max) {
	    last_pushed = current_batch->seq;
	    workers.put_locked(current_batch);
	    current_batch = NULL;
	}
	if (current_batch == NULL) {
	    if (batches.empty()) {
		current_batch = new batch(BATCH_SIZE);
	    }
	    else {
		current_batch = batches.top();
		batches.pop();
	    }
	    current_batch->reset();
	    in_mem_objects[current_batch->seq] = current_batch->buf;
	}

	int64_t sector_offset = current_batch->len / 512,
	    lba = offset/512, limit = (offset+len)/512;
	current_batch->append_iov(offset / 512, iov, iovcnt);
	
	std::vector<extmap::lba2obj> deleted;
	extmap::obj_offset oo = {current_batch->seq, sector_offset};
	std::unique_lock objlock(map->m);

	map->map.update(lba, limit, oo, &deleted);
	for (auto d : deleted) {
	    auto [base, limit, ptr] = d.vals();
	    object_info[ptr.obj].live -= (limit - base);
	    total_live_sectors -= (limit - base);
	}
	
	return len;
    }

    ssize_t translate::readv(size_t offset, iovec *iov, int iovcnt) {
	smartiov iovs(iov, iovcnt);
	size_t len = iovs.bytes();
	int64_t base = offset / 512;
	int64_t sectors = len / 512, limit = base + sectors;
	
	if (map->map.size() == 0) {
	    iovs.zero();
	    return len;
	}

	/* object number, offset (bytes), length (bytes) */
	std::vector<std::tuple<int, size_t, size_t>> regions;

	std::unique_lock lk(m);
	std::shared_lock slk(map->m);
	
	auto prev = base;
	int iov_offset = 0;

	for (auto it = map->map.lookup(base); it != map->map.end() && it->base() < limit; it++) {
	    auto [_base, _limit, oo] = it->vals(base, limit);
	    if (_base > prev) {	// unmapped
		size_t _len = (_base - prev)*512;
		regions.push_back(std::tuple(-1, 0, _len));
		iov_offset += _len;
	    }
	    size_t _len = (_limit - _base) * 512,
		_offset = oo.offset * 512;
	    int obj = oo.obj;
	    if (in_mem_objects.find(obj) != in_mem_objects.end()) {
		iovs.slice(iov_offset, iov_offset+_len).copy_in(
		    in_mem_objects[obj]+_offset);
		obj = -2;
	    }
	    regions.push_back(std::tuple(obj, _offset, _len));
	    iov_offset += _len;
	    prev = _limit;
	}
	slk.unlock();
	lk.unlock();

	iov_offset = 0;
	for (auto [obj, _offset, _len] : regions) {
	    auto slice = iovs.slice(iov_offset, iov_offset + _len);
	    if (obj == -1)
		slice.zero();
	    else if (obj == -2)
		/* skip */;
	    else
		io->read_numbered_objectv(obj, slice.data(), slice.size(), _offset);
	    iov_offset += _len;
	}

	return iov_offset;
    }

    // debug methods
    void translate::getmap(int base, int limit, int (*cb)(void *ptr,int,int,int,int), void *ptr) {
	for (auto it = map->map.lookup(base); it != map->map.end() && it->base() < limit; it++) {
	    auto [_base, _limit, oo] = it->vals(base, limit);
	    if (!cb(ptr, (int)_base, (int)_limit, (int)oo.obj, (int)oo.offset))
		break;
	}
    }
    int translate::mapsize(void) {
	return map->map.size();
    }
    void translate::reset(void) {
	map->map.reset();
    }
    int translate::frontier(void) {
	if (translate::current_batch)
	    return current_batch->len / 512;
	return 0;
    }

