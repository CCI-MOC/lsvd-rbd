/*
 * file:        thick-image.cc
 * description: create thick-provisioned LSVD image
 *
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2023 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <argp.h>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <queue>
#include <rados/librados.h>
#include <stdio.h>
#include <stdlib.h>

#include "backend.h"
#include "lsvd_types.h"
#include "objects.h"
#include "request.h"

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
    writer(char *buf_) { buf = buf_; }
    ~writer() { free(buf); }
    void notify(request *child)
    {
        if (child != NULL)
            child->release();
        std::unique_lock lk(m);
        done = true;
        cv.notify_all();
    }

    void wait_for(void)
    {
        std::unique_lock lk(m);
        while (!done)
            cv.wait(lk);
    }
};

static long parseint(const char *_s)
{
    char *s = (char *)_s;
    long val = strtol(s, &s, 0);
    if (toupper(*s) == 'G')
        val *= (1024 * 1024 * 1024);
    if (toupper(*s) == 'M')
        val *= (1024 * 1024);
    if (toupper(*s) == 'K')
        val *= 1024;
    return val;
}

long img_size = 0;
char *image_name;

static struct argp_option options[] = {
    {"size", 's', "SIZE", 0, "size in bytes (M/G=2^20,2^30)", 0},
    // {"pool", 'p', "POOL", 0, "pool for object"},
    {0, 0, 0, 0, 0, 0},
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
static struct argp argp = {options, parse_opt, NULL, args_doc, 0, 0, 0};

void create_thick(char *name, long size)
{
    uuid_t uu;
    uuid_generate(uu);

    auto be = make_rados_backend(NULL);

    int n_objs = size / (16384 * 512);

    /* create all the data objects, each with a single 8MB extent
     */
    std::queue<writer *> q;
    long sector = 0;
    int pct = 0;

    for (int i = 1; i <= n_objs; i++) {
        uint32_t data_sectors = 16 * 1024;
        size_t data_bytes = data_sectors * 512;
        auto hdr_buf = (char *)calloc(data_bytes + 4096, 1);

        auto h = (common_obj_hdr *)hdr_buf;
        *h = {LSVD_MAGIC, 1, {0}, OBJ_LOGDATA, 0, 8, data_sectors, 0};
        memcpy(h->vol_uuid, uu, sizeof(uu));

        auto dh = (obj_data_hdr *)(h + 1);
        auto map1 = (data_map *)(dh + 1);
        *dh = {0, 0, 0, sizeof(*h) + sizeof(*dh), sizeof(*map1), 0};

        char oname[128];
        sprintf(oname, "%s.%08x", name, i);
        h->seq = i;
        map1->lba = sector;
        map1->len = 16384;
        sector += 16384;

        auto req = be->aio_write(oname, hdr_buf, data_bytes + 4096);
        auto r = new writer(hdr_buf);
        q.push(r);
        req->run(r);
        while (q.size() > 32) {
            auto _r = q.front();
            q.pop();
            _r->wait_for();
            delete _r;
        }

        int _pct = i * 100 / n_objs;
        if (pct != _pct) {
            printf("Thick provisioning: %d%% complete...\r", pct = _pct);
            fflush(stdout);
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
    uint32_t objmap_bytes = n_objs * sizeof(ckpt_obj);
    uint32_t extmap_bytes = n_objs * sizeof(ckpt_mapentry);

    int ckpt_bytes = sizeof(common_obj_hdr) + sizeof(obj_ckpt_hdr) +
                     objmap_bytes + extmap_bytes;
    uint32_t ckpt_sectors = div_round_up(ckpt_bytes, 512);

    auto ckpt_buf = (char *)calloc(ckpt_sectors * 512, 1);
    auto ch = (common_obj_hdr *)ckpt_buf;
    *ch = {LSVD_MAGIC, 1, {0}, OBJ_CHECKPOINT, 0, ckpt_sectors, 0, 0};
    memcpy(ch->vol_uuid, uu, sizeof(uu));

    auto cph = (obj_ckpt_hdr *)(ch + 1);

    auto objmap = (ckpt_obj *)(cph + 1);
    uint32_t objmap_offset = (char *)objmap - ckpt_buf;

    uint32_t extmap_offset = objmap_offset + objmap_bytes;
    auto extmap = (ckpt_mapentry *)(ckpt_buf + extmap_offset);

    *(cph) = {
        0,           0, 0, objmap_offset, objmap_bytes, 0, 0, extmap_offset,
        extmap_bytes};

    for (int i = 1; i <= n_objs; i++) {
        objmap[i - 1].seq = i;
        objmap[i - 1].hdr_sectors = 1;
        objmap[i - 1].data_sectors = 16 * 1024;
        objmap[i - 1].live_sectors = 16 * 1024;
    }

    unsigned long lba = 0;
    for (int i = 1; i <= n_objs; i++) {
        extmap[i - 1].lba = lba;
        extmap[i - 1].len = 16 * 1024;
        extmap[i - 1].obj = i;
        extmap[i - 1].offset = 1;
        lba += 16 * 1024;
    }

    char oname[128];
    sprintf(oname, "%s.%08x", name, n_objs + 1);
    be->write(oname, ckpt_buf, ckpt_sectors * 512);

    /* now write the superblock, with a single checkpoint pointer
     */
    auto sb_data = (char *)calloc(4096, 1);
    auto h2 = (common_obj_hdr *)sb_data;
    auto sh = (super_hdr *)(h2 + 1);
    int ckpt_offset = sizeof(*h2) + sizeof(*sh);

    *h2 = {LSVD_MAGIC, 1, {0}, OBJ_SUPERBLOCK, 0, 8, 0, 0};
    memcpy(h2->vol_uuid, uu, sizeof(uu));

    sh->vol_size = img_size / 512;
    sh->ckpts_offset = ckpt_offset;
    sh->ckpts_len = sizeof(int);

    auto ckpt = (int *)(sb_data + ckpt_offset);
    *ckpt = n_objs + 1;

    be->write(name, sb_data, 4096);

    printf("Thick provisioning: 100%% complete...done\n");
}

int main(int argc, char **argv)
{
    argp_parse(&argp, argc, argv, 0, 0, 0);

    create_thick(image_name, img_size);
}
