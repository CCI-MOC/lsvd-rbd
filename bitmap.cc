#include <stdint.h>
#include <cassert>
#include <string.h>

// Bitmap allocator
//

// for forward linking we need to know what the *next* allocation 
// will be. We start searching at 'next', and guarantee that location
// is always free. Which also means we always need at least one free 
// location.

class bitmap {
    const int SHIFT = 6;
    const int MASK = 63;
    
    uint64_t *map;
    size_t    size;
    size_t    nfree;
    size_t    next;

    bool get(size_t i) {
	assert(i < size);
	return (map[i >> SHIFT] & (1 << (i & MASK))) != 0;
    }

    void set(size_t i) {
	assert(i < size);
	map[i >> SHIFT] |= (1 << (i & MASK));
    }

    void clear(size_t i) {
	assert(i < size);
	map[i >> SHIFT] &= ~((1 << (i % MASK)));
    }    

    size_t count(void) {
	size_t sum = 0;
	for (size_t i = 0; i < size; i++)
	    sum += (get(i) ? 1 : 0);
	return sum;
    }
	
 public:

    bitmap(size_t _size) {
	size_t bytes = (_size + MASK) >> SHIFT; 
	map = (uint64_t*) calloc(bytes, 1);
	size = nfree = size;
	next = 0;
    }
    
    bitmap(size_t _size, void *_map, size_t _next) {
	map = (uint64_t*) _map;
	size = _size;
	next = _next;
	nfree = count();
	assert(!get(_next));
    }

    size_t bytes(void) {
	return (size + MASK) >> SHIFT;
    }

    // sizeof(buf) >= bytes()
    //
    void copy(void *buf) {
	memcpy(buf, (void*)map, bytes());
    }
    
    void utilization(size_t &total, size_t &free) {
	total = size;
	free = nfree;
    }

    // returns value of 'next' for forward linking
    // TODO - 
    //
    size_t allocate(size_t n, size_t *blks) {
	assert(nfree > n);
	assert(!get(next));

	for (int i = 0; i < n; i++) {
	    blks[i] = next;
	    set(next);
	    nfree -= 1;
	    do {
		next = (next + 1) % size;
	    } while (get(next));
	}
	return next;
    }

    void free(struct bitmap_alloc *b, size_t i) {
	clear(i);
	nfree += 1;
    }

    // used during recovery - mark a block as allocated, update
    // next pointer. kind of a kludge...
    //
    void mark(size_t i) {
	if (!get(i)) {
	    set(i);
	    nfree -= 1;
	}
    }
    void set_next(size_t _next) {
	next = _next;
    }
};

