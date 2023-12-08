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
                               1 + w_meta, 1 + w_meta,
                               1 + w_meta + w_pages, 1 + w_meta,
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