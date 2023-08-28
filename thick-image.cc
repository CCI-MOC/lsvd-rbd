/*
 * file:        thick-image.cc
 * description: create thick-provisioned LSVD image
 *
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2023 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <stdlib.h>
#include <rados/librados.h>
#include <cassert>
#include <stdio.h>
#include <fcntl.h>
#include <queue>
#include <mutex>
#include <condition_variable>

#include <argp.h>

#include "lsvd_types.h"

#include "smartiov.h"
#include "extent.h"
#include "request.h"
#include "backend.h"
#include "objname.h"
#include "objects.h"

/* we should be able to just make backend requests and then
 * wait on them, but the backend segfaults in the case where
 * there's no higher-level request :-(
 */
struct writer : public trivial_request {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    char *buf = NULL;
    
public:
    writer(char *buf_) {
	buf = buf_;
    }
    ~writer() {
	free(buf);
    }
    void notify(request *child) {
	if (child != NULL)
	    child->release();
	std::unique_lock lk(m);
	done = true;
	cv.notify_all();
    }

    void wait_for(void) {
	std::unique_lock lk(m);
	while (!done)
	    cv.wait(lk);
    }
};

static long parseint(const char *_s)
{
    char *s = (char*)_s;
    long val = strtol(s, &s, 0);
    if (toupper(*s) == 'G')
        val *= (1024*1024*1024);
    if (toupper(*s) == 'M')
        val *= (1024*1024);
    if (toupper(*s) == 'K')
        val *= 1024;
    return val;
}

long img_size = 0;
char *image_name;

static struct argp_option options[] = {
    {"size",     's', "SIZE", 0, "size in bytes (M/G=2^20,2^30)"},
    {0},
};

static char args_doc[] = "IMAGE";

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case ARGP_KEY_ARG:
        image_name = arg;
        break;
    case 's':
        img_size = parseint(arg);
        break;
    case ARGP_KEY_END:
        if (img_size == 0 || image_name == NULL)
            argp_usage(state);
        break;
    }
    return 0;
}
static struct argp argp = { options, parse_opt, NULL, args_doc};

void create_thick(char *name, long size) {
    uuid_t uu;
    uuid_generate(uu);

    auto be = make_rados_backend(NULL);

    int n_objs = size / (16384 * 512);
    int ckpt_seq = n_objs+1;

    /* create all the data objects, each with a single 8MB extent
     */
    std::queue<writer*> q;
    long sector = 0;

    for (int i = 1; i <= n_objs; i++) {
	sector_t data_sectors = 16 * 1024;
	size_t data_bytes = data_sectors * 512;
	auto hdr_buf = (char*)calloc(data_bytes+512, 1);

	auto h = (obj_hdr*)hdr_buf;
	*h = {LSVD_MAGIC, 1, {0}, LSVD_DATA, 0, 1, data_sectors, 0};
	memcpy(h->vol_uuid, uu, sizeof(uu));

	auto dh = (obj_data_hdr*)(h+1);
	auto map1 = (data_map*)(dh+1);
	*dh = {0, 0, 0, sizeof(*h) + sizeof(*dh), sizeof(*map1), 0};

	char oname[128];
	sprintf(oname, "%s.%08x", name, i);
	h->seq = i;
	map1->lba = sector;
	map1->len = 16384;
	sector += 16384;

	auto req = be->make_write_req(oname, hdr_buf, data_bytes+512);
	auto r = new writer(hdr_buf);
	q.push(r);
	req->run(r);
	while (q.size() > 16) {
	    auto _r = q.front();
	    q.pop();
	    _r->wait_for();
	    delete _r;
	}
    }

    while (q.size() > 0) {
	auto _r = q.front();
	q.pop();
	_r->wait_for();
	delete _r;
    }

    /* now create a checkpoint listing all the objects we created and
     * their extents
     */
    int objmap_bytes = n_objs * sizeof(ckpt_obj);
    int extmap_bytes = n_objs * sizeof(ckpt_mapentry);

    int ckpt_bytes = sizeof(obj_hdr) + sizeof(obj_ckpt_hdr) +
	objmap_bytes + extmap_bytes;
    int ckpt_sectors = div_round_up(ckpt_bytes, 512);

    auto ckpt_buf = (char*)calloc(ckpt_sectors*512, 1);
    auto ch = (obj_hdr*)ckpt_buf;
    *ch = {LSVD_MAGIC, 1, {0}, LSVD_CKPT, 0, ckpt_sectors, 0, 0};
    memcpy(ch->vol_uuid, uu, sizeof(uu));

    auto cph = (obj_ckpt_hdr*)(ch+1);

    auto objmap = (ckpt_obj*)(cph + 1);
    int objmap_offset = (char*)objmap - ckpt_buf;

    int extmap_offset = objmap_offset + objmap_bytes;
    auto extmap = (ckpt_mapentry*)(ckpt_buf + extmap_offset);

    *(cph) = {0, 0, 0, objmap_offset, objmap_bytes, 0, 0,
	      extmap_offset, extmap_bytes};
	
    for (int i = 1; i <= n_objs; i++) {
	objmap[i-1].seq = i;
	objmap[i-1].hdr_sectors = 1;
	objmap[i-1].data_sectors = 16*1024;
	objmap[i-1].live_sectors = 16*1024;
    }

    unsigned long lba = 0;
    for (int i = 1; i <= n_objs; i++) {
	extmap[i-1].lba = lba;
	extmap[i-1].len = 16*1024;
	extmap[i-1].obj = i;
	extmap[i-1].offset = 1;
    }

    char oname[128];
    sprintf(oname, "%s.%08x", name, n_objs+1);
    be->write_object(oname, ckpt_buf, ckpt_sectors*512);

    /* now write the superblock, with a single checkpoint pointer
     */
    auto sb_data = (char*) calloc(4096, 1);
    auto h2 = (obj_hdr*) sb_data;
    auto sh = (super_hdr*)(h2+1);
    int ckpt_offset = sizeof(*h2) + sizeof(*sh);
    
    *h2 = {LSVD_MAGIC, 1, {0}, LSVD_SUPER, 0, 8, 0, 0};
    memcpy(h2->vol_uuid, uu, sizeof(uu));

    sh->vol_size = img_size / 512;
    sh->ckpts_offset = ckpt_offset;
    sh->ckpts_len = sizeof(int);

    auto ckpt = (int*)(sb_data + ckpt_offset);
    *ckpt = n_objs+1;
    
    be->write_object(name, sb_data, 4096);
}

int main(int argc, char **argv) {
    argp_parse (&argp, argc, argv, 0, 0, 0);

    create_thick(image_name, img_size);
}

