#include <stdint.h>
#include <vector>

//enum { LSVD_MAGIC = 0x4456534c };

struct j_extent {
    uint64_t blk : 36;		// SSD block (LBA/8)
    uint64_t len : 28;
    uint64_t lba;		// volume LBA (in sectors)
} __attribute__((packed));

enum {LSVD_J_DATA,
      LSVD_J_CKPT,
      LSVD_J_PAD,
      LSVD_J_SUPER,
      LSVD_J_W_SUPER,
      LSVD_J_R_SUPER};

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

    /* all in 4KB block units. FIFO range is [base,limit), 
     *  valid range accounting for wraparound is [oldest,next)
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
    uint32_t map_start;
    uint32_t map_blocks;
    uint32_t map_entries;
};

/* probably in the third 4KB block, never gets overwritten (overwrite map in place)
 * uses a fixed map with 1 entry per 64KB block, stored as 32-bit value (LBA/128)
 * to update atomically:
 * - reclaim batch of blocks, then write map. (free entries = UINT_MAX)
 * - allocate blocks, then write map
 * - recover free list to memory on startup
 */
struct j_read_super {
    uint32_t magic;
    uint32_t type;		// LSVD_J_R_SUPER
    uint32_t version;		// 1
    uuid_t   vol_uuid;

    uint32_t map_start;
    uint32_t map_blocks;
    uint32_t map_entries;	// entry is uint32_t = LBA/128

    /* need to persist status of cache replacement algorithm
     * for now it's just in memory. 
     * Note that with 64KB blocks, LFUDA would take 128MB per TB of SSD
     */
};

/* this goes in the first 4KB block in the cache partition, and never
 * gets modified
 */
struct j_super {
    uint32_t magic;
    uint32_t type;		// LSVD_J_SUPER
    uint32_t version;		// 1
    uuid_t   vol_uuid;

    /* both are single blocks, so we only need a block number
     */
    uint32_t write_super;
    uint32_t read_super;
};

/* TODO: 
 * - superblock
 * - move volume UUID to superblock??? maybe have it everywhere.
 */
