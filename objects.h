//
// file:        objects.cc
// description: header, checkpoint etc. objects
//
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
    LSVD_CKPT = 3,
    LSVD_MAGIC = 0x4456534c
};

// hdr :	header structure used to contain data for separate objects in translation and write cache layers
//		of the LSVD system
struct hdr {
    uint32_t magic;
    uint32_t version;		// 1
    uuid_t   vol_uuid;
    uint32_t type;
    uint32_t seq;		// same as in name
    uint32_t hdr_sectors;
    uint32_t data_sectors;	// hdr_sectors + data_sectors = size
};

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
};

/* ckpts: list of active checkpoints: array of uint32_t */

/* variable-length structure */
struct clone_info {
    uuid_t   vol_uuid;
    uint32_t sequence;
    uint8_t  name_len;
    char     name[0];
} __attribute__((packed));

struct snap_info {
    uuid_t   snap_uuid;
    uint32_t seq;
};


struct data_hdr {
    uint32_t last_data_obj;
    uint32_t ckpts_offset;
    uint32_t ckpts_len;
    uint32_t objs_cleaned_offset;
    uint32_t objs_cleaned_len;
    uint32_t map_offset;
    uint32_t map_len;
};

struct obj_cleaned {
    uint32_t seq;
    uint32_t was_deleted;
};

// we can omit the offset in a data header
//
struct data_map {
    uint64_t lba : 36;
    uint64_t len : 28;
};

struct ckpt_hdr {
    uint32_t ckpts_offset;	// list includes self
    uint32_t ckpts_len;
    uint32_t objs_offset;	// objects and sizes
    uint32_t objs_len;
    uint32_t deletes_offset;	// deferred deletes
    uint32_t deletes_len;
    uint32_t map_offset;
    uint32_t map_len;
};

struct ckpt_obj {
    uint32_t seq;
    uint32_t hdr_sectors;
    uint32_t data_sectors;
    uint32_t live_sectors;
};

struct deferred_delete {
    uint32_t seq;		// object deleted
    uint32_t time;		// current write frontier when cleaned
};

struct ckpt_mapentry {
    int64_t lba : 36;
    int64_t len : 28;
    int32_t obj;
    int32_t offset;
};

#endif
