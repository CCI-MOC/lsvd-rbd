/* 
This file, journal2.h, features structures which are used for the each of the
logging systems used in the lsvd system, including structures of superblocks for
both the write and read caches, and the connecting structures between them and 
the how they fit together with rados and s3
*/
#ifndef JOURNAL2_H
#define JOURNAL2_H

#include <stdint.h>
#include <vector>

//enum { LSVD_MAGIC = 0x4456534c };

struct j_extent {
    uint64_t lba : 40;		// volume LBA (in sectors)
    uint64_t len : 24;		// length (sectors)
} __attribute__((packed));

// could do a horrible hack to make this 14 bytes, but not worth it
struct j_map_extent {
    uint64_t lba : 40;		// volume LBA (in sectors)
    uint64_t len : 24;		// length (sectors)
    uint64_t plba;		// on-SSD LBA
};

struct j_length {
    int32_t page;		// in pages
    int32_t len;		// in pages
};

enum {LSVD_J_DATA    = 10,
      LSVD_J_CKPT    = 11,
      LSVD_J_PAD     = 12,
      LSVD_J_SUPER   = 13,
      LSVD_J_W_SUPER = 14,
      LSVD_J_R_SUPER = 15};

/* for now we'll assume that all entries are contiguous
 */
struct j_hdr {
    uint32_t magic;
    uint32_t type;		// LSVD_J_DATA
    uint32_t version;		// 1
    uuid_t   vol_uuid;		// must match backend volume
    uint64_t seq;
    uint32_t len;		// in 4KB blocks, including header
    uint32_t crc32;		// TODO: implement this
    uint32_t extent_offset;	// in bytes
    uint32_t extent_len;	// in bytes
};

/* probably in the second 4KB block of the parition
 * this gets overwritten every time we re-write the map. We assume the 4KB write is
 * atomic, and write out the new map before updating the superblock.
 */
struct j_write_super {
    uint32_t magic;
    uint32_t type;		// LSVD_J_W_SUPER
    uint32_t version;		// 1
    uuid_t   vol_uuid;

    uint64_t seq;		// next write sequence

    /* all values are in 4KB block units. */
    
    /* Map and length checkpoints live in this region. Allocation within this
     * range is arbitrary, just set {map/len}_{start/blocks/entries} properly
     */
    uint32_t meta_base;
    uint32_t meta_limit;
    
    /* FIFO range is [base,limit), 
     *  valid range accounting for wraparound is [oldest,next)
     *  'wrapped' 
     */
    uint32_t base;
    uint32_t limit;
    uint32_t next;
    uint32_t oldest;
    
    /* to checkpoint the map:
     * - allocate enough blocks at the write frontiers
     * - write LSVD_J_CKPT header + map entries
     * - overwrite the write superblock
     */
    uint32_t map_start;		// type: j_map_extent
    uint32_t map_blocks;
    uint32_t map_entries;

    uint32_t len_start;	// type: j_length
    uint32_t len_blocks;
    uint32_t len_entries;
};

/* probably in the third 4KB block, never gets overwritten (overwrite map in place)
 * uses a fixed map with 1 entry per 64KB block
 * to update atomically:
 * - reclaim batch of blocks, then write map. (free entries: obj=0)
 * - allocate blocks, then write map
 * - recover free list to memory on startup
 */
struct j_read_super {
    uint32_t magic;
    uint32_t type;		// LSVD_J_R_SUPER
    uint32_t version;		// 1
    uuid_t   vol_uuid;

    int32_t unit_size;		// cache unit size, in sectors

    /* note that the cache is not necessarily unit-aligned
     */
    int32_t base;		// the cache itself
    int32_t units;		// length, in @unit_size segments

    /* each has @cache_segments entries
     */
    int32_t map_start;		// extmap::obj_offset
    int32_t map_blocks;

    int32_t evict_type;		// eviction algorithm - TBD
    int32_t evict_start;	// eviction state - TBD
    int32_t evict_blocks;
};

      
/* this goes in the first 4KB block in the cache partition, and never
 * gets modified
 */
struct j_super {
    uint32_t magic;
    uint32_t type;		// LSVD_J_SUPER
    uint32_t version;		// 1

    /* both are single blocks, so we only need a block number
     */
    uint32_t write_super;
    uint32_t read_super;

    uuid_t   vol_uuid;
    uint32_t backend_type;
};

/* backend follows superblock
 */
enum {LSVD_BE_FILE  = 20,
      LSVD_BE_S3    = 21,
      LSVD_BE_RADOS = 22};

struct j_be_file {
    uint16_t len;
    char    prefix[0];
};

struct offset_len {
    uint16_t offset;
    uint16_t len;
};

struct j_be_s3 {
    uint16_t use_https;
    struct offset_len access_key;
    struct offset_len secret_key;
    struct offset_len hostname;
    struct offset_len bucket;
    struct offset_len prefix;
};

/* based on C example at https://docs.ceph.com/en/latest/rados/api/librados-intro/
 */
struct j_be_rados {
    struct offset_len cluster_name;
    struct offset_len user_name;
    struct offset_len config_file;
};


/* TODO: 
 * - superblock
 * - move volume UUID to superblock??? maybe have it everywhere.
 */

#endif
