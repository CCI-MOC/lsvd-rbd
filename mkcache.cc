/*
 * file:        mkcache.cc
 * description: create file containing write+read caches
 *
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include <string>

#include "extent.h"
#include "journal.h"
#include "lsvd_types.h"

/* translated from mkcache.py version 2a8d3060
 */
int make_cache(int fd, uuid_t &uuid, int n_pages)
{
    page_t r_units = 0.66 * n_pages / 16;
    page_t r_pages = r_units * 16;
    page_t r_meta = div_round_up(r_units * sizeof(extmap::obj_offset), 4096);
    page_t w_pages = n_pages - r_pages - r_meta - 3;

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    auto sup = (j_super *)buf;
    *sup = (j_super){LSVD_MAGIC, LSVD_J_SUPER,
                     1,    // version
                     1,    // write superblock offset
                     2,    // read superblock offset
                     {0}}; // uuid
    memcpy(sup->vol_uuid, uuid, sizeof(uuid_t));
    write(fd, buf, 4096);

    page_t _map = div_round_up(w_pages, 256);
    page_t _len = div_round_up(w_pages, 512);
    page_t w_meta = 2 * (_map + _len); // TODO: no more 2x?
    w_pages -= w_meta;

    memset(buf, 0, sizeof(buf));
    auto w_super = (j_write_super *)buf;
    *w_super = (j_write_super){LSVD_MAGIC,
                               LSVD_J_W_SUPER,
                               1,                    // version
                               1,                    // clean
                               1,                    // seq
                               3,                    // meta_base
                               3 + w_meta,           // meta_limit
                               3 + w_meta,           // base
                               3 + w_meta + w_pages, // limit
                               3 + w_meta,           // next
                               0,
                               0,
                               0, // map_start/blocks/entries
                               0,
                               0,
                               0}; // len_start/blocks/entries
    write(fd, buf, 4096);

    page_t r_base = 3 + w_pages + w_meta;

    memset(buf, 0, sizeof(buf));
    auto r_super = (j_read_super *)buf;
    *r_super = (j_read_super){LSVD_MAGIC,
                              LSVD_J_R_SUPER,
                              1,               // version
                              128,             // unit_size
                              r_base + r_meta, // base
                              r_units,         // units
                              r_base,          // map_start
                              r_meta};         // map_blocks
    write(fd, buf, 4096);

    memset(buf, 0, 4096);
    for (int i = 3; i < 3 + w_pages + w_meta + r_pages + r_meta; i++)
        write(fd, buf, 4096);

    return 0;
}

int init_rcache(int fd, uuid_t &uuid, int n_pages)
{
    page_t r_units = n_pages / 16;
    page_t r_pages = n_pages;
    page_t r_meta = div_round_up(r_units * sizeof(extmap::obj_offset), 4096);
    page_t r_base = 1;

    char buf[4096];

    memset(buf, 0, sizeof(buf));
    auto r_super = (j_read_super *)buf;
    *r_super = (j_read_super){LSVD_MAGIC,      LSVD_J_R_SUPER, 1,      128,
                              r_base + r_meta, r_units,        r_base, r_meta, {0}};
    memcpy(r_super->vol_uuid, uuid, sizeof(uuid_t));
    if (!write(fd, buf, 4096)) {
        perror("read cache write");
        return -1;
    }

    memset(buf, 0, 4096);
    for (int i = 1; i < 1 + r_pages + r_meta; i++) {
        if (!write(fd, buf, 4096)) {
            perror("read cache write");
            return -1;
        }
    }

    return 0;
}

int init_wcache(int fd, uuid_t &uuid, int n_pages)
{
    page_t w_pages = n_pages - 1;
    page_t _map = div_round_up(w_pages, 256);
    page_t _len = div_round_up(w_pages, 512);
    page_t w_meta = 2 * (_map + _len);
    char buf[4096];

    w_pages -= w_meta;

    memset(buf, 0, sizeof(buf));
    auto w_super = (j_write_super *)buf;
    *w_super = (j_write_super){LSVD_MAGIC, LSVD_J_W_SUPER,
                               1,          1,
                               1,          1,
                               1 + w_meta, 1 + w_meta + w_pages,
                               1 + w_meta, 0,
                               0,          0,
                               0,          0,
                               0,          0, {0}};
    memcpy(w_super->vol_uuid, uuid, sizeof(uuid_t));
    if (!write(fd, buf, 4096)) {
        perror("write cache write");
        return -1;
    }

    memset(buf, 0, 4096);
    for (int i = 1; i < 1 + w_pages + w_meta; i++) {
        if (!write(fd, buf, 4096)) {
            perror("write cache write");
            return -1;
        }
    }

    return 0;
}