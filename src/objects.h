#pragma once

#include <cstdint>
#include <uuid/uuid.h>

#include "backend.h"
#include "lsvd_types.h"
#include "utils.h"

#if __BYTE_ORDER != __LITTLE_ENDIAN
#error "this code is little-endian only"
#endif

enum obj_type { OBJ_SUPERBLOCK = 1, OBJ_LOGDATA = 2, OBJ_CHECKPOINT = 3 };

/* hdr - standard header for all backend objects
 * total length is hdr_sectors + data_sectors, in 512-byte units
 * name is <prefix> for superblock, (<prefix>.%08x % seq) otherwise
 */
struct common_obj_hdr {
    uint32_t magic;
    uint32_t version; // 1
    uuid_t vol_uuid;
    uint32_t type;
    uint32_t seq; // same as in name
    uint32_t hdr_sectors;
    uint32_t data_sectors;
    uint32_t crc; // of header only
} __attribute__((packed));

/*  super_hdr - "superblock object", describing the entire volume
 *
 * variable-length fields are identified by offset/length pairs;
 * offset, length are both in units of bytes.
 *
 * ckpts : array of checkpoint sequence numbers (uint32). We keep the
 *         last 3, but we only need 1 unless we go to incremental ckpts
 * clones : zero or one struct clone_info, followed by a name and a zero
 *         byte to terminate the string. (chase the chain to find all)
 * snaps : TBD
 */
struct super_hdr {
    uint64_t vol_size; // in 512 byte sectors
    uint32_t ckpts_offset;
    uint32_t ckpts_len;
    uint32_t clones_offset; // array of struct clone
    uint32_t clones_len;
    uint32_t snaps_offset;
    uint32_t snaps_len;
} __attribute__((packed));

/* ckpts: list of active checkpoints: array of uint32_t */

/* variable-length structure
 * objects @last_seq and earlier are in volume @name or its predecessors
 */
struct clone_info {
    uuid_t vol_uuid;   // of volume @name
    uint32_t last_seq; // of linked volume
    uint8_t name_len;
    char name[0];
} __attribute__((packed));

struct snap_info {
    uint32_t seq;
    uint8_t name_len;
    char name[0];
} __attribute__((packed));

/* sub-header for a data object
 *  cache_seq: lowest write cache sequence number (see source code)
 *  objs_cleaned: TBD
 *  data_map_offset/len: lba/len pairs for data in body of object.
 */
struct obj_data_hdr {
    uint64_t cache_seq;
    uint32_t objs_cleaned_offset;
    uint32_t objs_cleaned_len;
    uint32_t data_map_offset;
    uint32_t data_map_len;
    uint32_t is_gc;
} __attribute__((packed));

struct obj_cleaned {
    uint32_t seq;
    uint32_t was_deleted;
} __attribute__((packed));

// we can omit the offset in a data header
//
struct data_map {
    uint64_t lba : 36; // 512-byte sectors
    uint64_t len : 28; // 512-byte sectors
} __attribute__((packed));

/* sub-header for a checkpoint object
 *  TODO: get rid of ckpts_offset/len
 *  objs_offset/len: list of live objects. For clones, does not include
 *                   objects in clone base
 *  deletes_offset/len: TBD
 *  map_offset/len: LBA-to-object map at time checkpoint was written
 *
 * TODO: incremental checkpointing, taking advantage of dirty-bit
 * feature in extent.h
 */
struct obj_ckpt_hdr {
    uint64_t cache_seq;    // from last data object
    uint32_t ckpts_offset; // TODO: remove this
    uint32_t ckpts_len;
    uint32_t objs_offset; // ckpt_obj[]
    uint32_t objs_len;
    uint32_t deletes_offset; // deferred_delete[]
    uint32_t deletes_len;
    uint32_t map_offset; // ckpt_mapentry[]
    uint32_t map_len;
} __attribute__((packed));

struct ckpt_obj {
    uint32_t seq;
    uint32_t hdr_sectors;
    uint32_t data_sectors;
    uint32_t live_sectors;
} __attribute__((packed));

struct deferred_delete {
    uint32_t seq;  // object deleted
    uint32_t time; // current write frontier when cleaned
} __attribute__((packed));

struct ckpt_mapentry {
    int64_t lba : 36;
    int64_t len : 28;
    int32_t obj;
    int32_t offset;
} __attribute__((packed));

/* ------ helper functions -------- */

struct parsed_superblock {
    vec<byte> superblock_buf; // buffer containing superblock
    usize vol_size;           // size in bytes
    vec<u32> ckpts;           // checkpoint sequence numbers
    vec<clone_info *> clones; // ptrs are into the buffer
    vec<snap_info *> snaps;   // ptrs are into the buffer
    uuid_t uuid;
};

struct parsed_data_hdr {
    vec<byte> buf;
    common_obj_hdr *hdr;
    obj_data_hdr *data_hdr;
    vec<obj_cleaned *> cleaned;
    vec<data_map *> data_map;
};

struct parsed_checkpoint {
    vec<byte> buf;
    common_obj_hdr *hdr;
    obj_ckpt_hdr *ckpt_hdr;
    vec<u32> ckpts;
    vec<ckpt_obj *> objects;
    vec<deferred_delete *> deletes;
    vec<ckpt_mapentry *> dmap;
};

class object_reader
{
    sptr<backend> objstore;

  public:
    object_reader(std::shared_ptr<backend> be) : objstore(be) {}

    Result<vec<byte>> fetch_object_header(str oname);
    Result<parsed_superblock> read_superblock(str oname);
    Result<parsed_data_hdr> read_data_hdr(str oname);
    Result<parsed_checkpoint> read_checkpoint(str oname);
};

// ----- common image types, temporary(T&Cs apply) workaround -----

struct clone_base {
    std::string name;
    seqnum_t last_seq;
    seqnum_t first_seq = 0;
};

struct data_obj_info {
    sector_t hdr;
    sector_t data;
    sector_t live;
};

void serialise_common_hdr(vec<byte> &buf, obj_type t, seqnum_t s, u32 hdr,
                          u32 data, uuid_t &uuid);

// Serialise a superblock object.
void serialise_superblock(vec<byte> &buf, vec<seqnum_t> &checkpoints,
                          vec<clone_base> &clones, uuid_t &uuid,
                          usize vol_size);
