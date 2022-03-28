typedef void (*aio_cb_t)(long val, void *ptr1, void *ptr2);

enum lsvd_op {
    LSVD_OP_READ = 1,
    LSVD_OP_WRITE = 2
};

/* offset_base, len, buf_offset */
//typedef std::tuple<size_t,size_t,size_t> cache_miss;
struct cache_miss {
    size_t offset;		// volume offset, in bytes
    size_t len;			// in bytes
    size_t buf_offset;		// in bytes
    cache_miss(size_t _off, size_t _len, size_t _buf_offset) :
	offset(_off), len(_len), buf_offset(_buf_offset) {}
};

class lsvd_aio {
public:
    std::mutex m;
    lba_t      lba;
    smartiov   iovs;
    char      *aligned_buf = NULL;
    std::atomic<int> refs = 0;
    std::atomic<int> ops = 0;
    aio_cb_t   cb;
    long       val;
    void      *ptr1 = NULL;
    void      *ptr2 = NULL;

    lsvd_aio(lba_t _lba, const iovec *iov, int iovcnt) :
	iovs(iov, iovcnt) {
	lba = _lba;
    }
    void start(void) {
	ops++;
    }
    void finish(long _val) {
	if (--ops == 0) {
	    val = _val;
	    cb(val, ptr1, ptr2);
	}
    }
    void put(void) {
	refs++;
    }
    void get(void) {
	if (--refs == 0)
	    delete this;
    }
};

class lsvd_read_aio : lsvd_aio {
public:
    lsvd_read_aio(lba_t _lba, const iovec *iov, int iovcnt) :
	lsvd_aio(_lba, iov, iovcnt) {
    }
};

class lsvd_write_aio : lsvd_aio {
public:
    lsvd_write_aio(lba_t _lba, const iovec *iov, int iovcnt) :
	lsvd_aio(_lba, iov, iovcnt) {
    }
};
