/* This file contains all the rados_backend class functions. All functions used by LSVD
 * which focus on manipulating data to and from the rados backend
 */

#ifndef RADOS_BACKEND_H
#define RADOS_BACKEND_H

// split_string :	finds the first occurrence of the delim and returns a pair of one up to and one for after
//			the found delim.
std::pair<std::string,std::string> split_string(std::string s, std::string delim);

class rados_backend : public backend {
    std::mutex m;
    char *pool;
    char *prefix;
    rados_t cluster;
    rados_ioctx_t io_ctx;

public:
// rados_backend : constructor for the rados_backend class
    rados_backend(const char *_prefix) {
        int r;
        auto [_pool, _key] = split_string(std::string(_prefix), "/");
        if ((r = rados_create(&cluster, NULL)) < 0) // NULL = ".client"
            throw("rados create");
        if ((r = rados_conf_read_file(cluster, NULL)) < 0)
            throw("rados conf");
        if ((r = rados_connect(cluster)) < 0)
            throw("rados connect");
        if ((r = rados_ioctx_create(cluster, _pool.c_str(), &io_ctx)) < 0)
            throw("rados ioctx_create");
        prefix = strdup(_key.c_str());
    }
// write_object :	copies iov and iovcnt to smartiov and writes that to rados_backend and returns
//			the output of rados_write
    ssize_t write_object(const char *name, iovec *iov, int iovcnt);

// write_numbered_object :	Calls write_object based on a name with seq.
    ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt);

// delete_numbered_object :	Takes a name with the seq number and removes that rados object
    void delete_numbered_object(int seq);

// read_object :	Calls rados_read
    ssize_t read_object(const char *name, char *buf, size_t len, size_t offset);
// read_numbered_object :	calls read_object with a file name numbered with seq
    ssize_t read_numbered_object(int seq, char *buf, size_t len, size_t offset);
// read_numbered_objectv :	calls read_numbered_object after creating smartiov and using that for buf and len
    ssize_t read_numbered_objectv(int seq, iovec *iov, int iovcnt, size_t offset);

// rados_aio : structure which contains rado_completion, callback, and pointer
    struct rados_aio {
        void (*cb)(void*);
        void *ptr;
        rados_completion_t c;
    };
// aio_read_done :	creates a rados_aio object based on ptr and calls rados_aio_release
//			 and removes created object
    static void aio_read_done(rados_completion_t c, void *ptr);
// aio_read_num_object :	creates a rados_aio object, calls completion, and then rados_aio_read
    int aio_read_num_object(int seq, char *buf, size_t len, size_t offset,
                        void (*cb)(void*), void *ptr);
// aio_write_numbered_object :	calls write_object, callbacks to ptr, and returns write_object result
    int aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
                                  void (*cb)(void*), void *ptr);
// ~rados_backend :	Deconstructor for ~rados_backend
    ~rados_backend() {
        free((void*)prefix);
        rados_ioctx_destroy(io_ctx);
        rados_shutdown(cluster);
    }
// object_name :	 returns a file name based on seq
    std::string object_name(int seq);
};

#endif

