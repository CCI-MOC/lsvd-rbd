/*
 * file:        objects.h
 * description: back-end object format definitions and helper functions
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#ifndef OBJECTS_H
#define OBJECTS_H

#include <cstddef>
#include <stdint.h>
#include <stdlib.h>
#include <vector>
#include <tuple>
#include <cassert>
#include <uuid/uuid.h>

#if __BYTE_ORDER != __LITTLE_ENDIAN
#error "this code is little-endian only"
#endif

/* for now we'll use 32-bit object sequence numbers. So sue me...
 */

enum obj_type {
    LSVD_SUPER = 1,
    LSVD_DATA = 2,
    LSVD_CKPT = 3
};

// hdr :	header structure used to contain data for separate objects in translation and write cache layers
//		of the LSVD system
struct obj_hdr {
    uint32_t magic;
    uint32_t version;		// 1
    uuid_t   vol_uuid;
    uint32_t type;
    uint32_t seq;		// same as in name
    uint32_t hdr_sectors;
    uint32_t data_sectors;	// hdr_sectors + data_sectors = size
    uint32_t crc;
} __attribute__((packed));

// super_hdr :	super block style structure for the header structure, containing various metadata on the current
//		header structure it describes including offsets, checkpoint statistics, and size
/* variable-length fields are identified by offset/length pairs, where both
 * the offset and length are in units of bytes.
 */
struct super_hdr {
    uint64_t vol_size;
    uint64_t total_sectors;
    uint64_t live_sectors;
    uint32_t next_obj;    	// next sequence number
    uint32_t ckpts_offset;
    uint32_t ckpts_len;
    uint32_t clones_offset;	// array of struct clone
    uint32_t clones_len;
    uint32_t snaps_offset;
    uint32_t snaps_len;
} __attribute__((packed));

/* ckpts: list of active checkpoints: array of uint32_t */

/* variable-length structure */
struct clone_info {
    uuid_t   vol_uuid;
    uint32_t sequence;
    uint8_t  name_len;
    char     name[0];
} __attribute__((packed));

struct snap_info {
    uint32_t seq;
    uint8_t  name_len;
    char     name[0];
} __attribute__((packed));


struct obj_data_hdr {
    uint64_t cache_seq;
    uint32_t objs_cleaned_offset;
    uint32_t objs_cleaned_len;
    uint32_t data_map_offset;
    uint32_t data_map_len;
} __attribute__((packed));

struct obj_cleaned {
    uint32_t seq;
    uint32_t was_deleted;
} __attribute__((packed));

// we can omit the offset in a data header
//
struct data_map {
    uint64_t lba : 36;
    uint64_t len : 28;
} __attribute__((packed));

struct obj_ckpt_hdr {
    uint64_t cache_seq;         // from last data object
    uint32_t ckpts_offset;	// list includes self (TODO - not needed?)
    uint32_t ckpts_len;
    uint32_t objs_offset;	// ckpt_obj[]
    uint32_t objs_len;
    uint32_t deletes_offset;	// deferred_delete[]
    uint32_t deletes_len;
    uint32_t map_offset;        // ckpt_mapentry[]
    uint32_t map_len;
} __attribute__((packed));

struct ckpt_obj {
    uint32_t seq;
    uint32_t hdr_sectors;
    uint32_t data_sectors;
    uint32_t live_sectors;
} __attribute__((packed));

struct deferred_delete {
    uint32_t seq;		// object deleted
    uint32_t time;		// current write frontier when cleaned
} __attribute__((packed));

struct ckpt_mapentry {
    int64_t lba : 36;
    int64_t len : 28;
    int32_t obj;
    int32_t offset;
} __attribute__((packed));

class backend;

class object_reader {
    backend *objstore;

public:
    object_reader(backend *be) : objstore(be) {}

    char *read_object_hdr(const char *name, bool fast);

    std::pair<char*,ssize_t> read_super(const char *name,
					std::vector<uint32_t> &ckpts,
					std::vector<clone_info*> &clones,
					std::vector<snap_info*> &snaps,
					uuid_t &uuid);

    ssize_t read_data_hdr(const char *name, obj_hdr &h, obj_data_hdr &dh,
			  std::vector<obj_cleaned> &cleaned,
			  std::vector<data_map> &dmap);

    ssize_t read_checkpoint(const char *name, uint64_t &cache_seq,
			    std::vector<uint32_t> &ckpts,
			    std::vector<ckpt_obj> &objects, 
			    std::vector<deferred_delete> &deletes,
			    std::vector<ckpt_mapentry> &dmap);
};

extern size_t obj_hdr_len(int n_entries);

extern size_t make_data_hdr(char *hdr, size_t bytes, uint64_t cache_seq,
                            std::vector<data_map> *entries,
                            uint32_t seq, uuid_t *uuid);

#endif
