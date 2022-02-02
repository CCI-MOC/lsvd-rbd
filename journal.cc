#include <stdint.h>
#include <vector>

// block size 4KB - i.e. block numbers are LBA/8

enum { LSVD_MAGIC = 0x4456534c };

struct j_extent {
    uint64_t lba : 36;
    uint64_t len : 28;
    uint64_t LBA;
} __attribute__((packed));

struct journal_hdr {
    uint32_t magic;
    uint64_t sequence;		// TODO - FIX REST OF CODE
    uint32_t this_blk;
    uint32_t next_blk;
    uint32_t block_count;
    uint32_t block_offset;	// in bytes
    uint32_t extent_count;
    uint32_t extent_offset;
};

class journal_entry {
public:

    uint32_t seq;
    uint32_t this_blk;
    uint32_t next_blk;
    std::vector<uint32_t> *blocks;
    std::vector<j_extent> *extents;

    journal_entry() {
	seq = this_blk = next_blk = 0;
	blocks = nullptr;
	extents = nullptr;
    }
    
    journal_entry(uint32_t _seq, uint32_t _this, uint32_t _next,
		  std::vector<uint32_t> *_blocks, std::vector<j_extent> *_extents) {
	seq = _seq;
	this_blk = _this;
	next_blk = _next;
	blocks = _blocks;
	extents = _extents;
    }

    ~journal_entry() {
	if (blocks)
	    delete blocks;
	if (extents)
	    delete extents;
    }

    size_t bytes(void) {
	return sizeof(journal_hdr) +
	    blocks->size() * sizeof(uint32_t) +
	    extents->size() * sizeof(j_extent);
    }

    size_t serialize(char *buf, size_t len) {
	if (bytes() < len)
	    return 0;
	size_t blk_bytes = blocks->size() * sizeof(uint32_t);
	size_t extent_count = extents ? extents->size() : 0;
	
	journal_hdr *j = (journal_hdr*) buf;
	*j = (journal_hdr){ .magic = LSVD_MAGIC, .sequence = seq, .this_blk = this_blk,
			    .next_blk = next_blk, .block_count = (uint32_t)blocks->size(),
			    .block_offset = sizeof(*j), .extent_count = (uint32_t)extent_count,
			    .extent_offset = (uint32_t)(sizeof(*j) + blk_bytes)};
	uint32_t *b_ptr = (uint32_t*) (j+1);
	for (auto it = blocks->begin(); it != blocks->end(); it++)
	    *b_ptr++ = *it;

	j_extent *j_ptr = (j_extent*) b_ptr;
	for (auto it = extents->begin(); it != extents->end(); it++)
	    *j_ptr++ = *it;

	return (char*)j_ptr - buf;
    }
    
    bool deserialize(char *buf, size_t len) {
	journal_hdr *j = (journal_hdr*) buf;
	if (len < sizeof(*j))
	    return false;
	if (j->magic != LSVD_MAGIC)
	    return false;
	if (len < sizeof(*j) + j->block_count * sizeof(uint32_t) +
	    j->extent_count * sizeof(j_extent))
	    return false;

	seq = j->sequence;
	this_blk = j->this_blk;
	next_blk = j->next_blk;

	blocks = new std::vector<uint32_t>;
	uint32_t *b_ptr = (uint32_t*) (buf + j->block_offset);
	for (int i = 0; i < j->block_count; i++)
	    blocks->push_back(*b_ptr++);

	extents = new std::vector<j_extent>;
	j_extent *j_ptr = (j_extent*) (buf + j->extent_offset);
	for (int i = 0; i < j->extent_count; i++)
	    extents->push_back(*j_ptr++);
	
	return true;
    }
};
