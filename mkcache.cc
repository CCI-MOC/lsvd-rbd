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
#include <uuid/uuid.h>

#include <string>

#include "lsvd_types.h"
#include "extent.h"
#include "journal.h"

/* translated from mkcache.py version 461b997f
 */
int make_cache(std::string name, uuid_t &uuid, uint32_t wblks, uint32_t rblks) {
    FILE *fp = fopen(name.c_str(), "wb");
    if (fp == NULL)
	return -1;

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    auto sup = (j_super*)buf;
    *sup = (j_super){LSVD_MAGIC,
		     LSVD_J_SUPER,
		     1,   // version
		     1,   // write superblock offset
		     2,   // read superblock offset
		     {*uuid}, // uuid
		     LSVD_BE_RADOS}; // doesn't matter
    fwrite(buf, 4096, 1, fp);

    uint32_t _map = div_round_up(wblks, 512);
    uint32_t _len = div_round_up(wblks, 1024);
    uint32_t mblks = 2 * (_map + _len);
    wblks -= mblks;
    
    memset(buf, 0, sizeof(buf));
    auto wsup = (j_write_super*)buf;
    *wsup = (j_write_super){LSVD_MAGIC,
			    LSVD_J_W_SUPER,
			    1,		   // version
			    1,		   // seq
			    3,		   // metadata_base
			    3+mblks,	   // metadata limit
			    3+mblks,	   // base
			    3+mblks+wblks, // limit
			    3+mblks,	   // next
			    3+mblks,	   // oldest
			    0,0,0,	   // map_start/blocks/entries
			    0,0,0};	   // len_start/blocks/entries
    fwrite(buf, 4096, 1, fp);

    int rbase = wblks+mblks+3;
    int units = rblks / 16;
    int map_blks = div_round_up(units*sizeof(extmap::obj_offset), 4096);

    memset(buf, 0, sizeof(buf));
    auto rsup = (j_read_super*)buf;
    *rsup = (j_read_super){LSVD_MAGIC,
			   LSVD_J_R_SUPER,
			   1,
			   128,	// unit size
			   rbase+map_blks, // base
			   units,	   // units
			   rbase,	   // map_start
			   map_blks,	   // map_blocks
			   0,0,0};	   // unused (eviction)
    fwrite(buf, 4096, 1, fp);

    memset(buf, 0, 4096);
    for (unsigned i = 0; i < 3 + mblks + wblks + map_blks + rblks; i++)
	fwrite(buf, 4096, 1, fp);
    fclose(fp);

    return 0;
}
