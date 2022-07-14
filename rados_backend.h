#ifndef RADOS_BACKEND_H
#define RADOS_BACKEND_H

std::pair<std::string,std::string> split_string(std::string s, std::string delim);

class rados_backend : public backend {
    std::mutex m;
    char *pool;
    char *prefix;
    rados_t cluster;
    rados_ioctx_t io_ctx;

public:
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
    ssize_t write_object(const char *name, iovec *iov, int iovcnt);
    ssize_t write_numbered_object(int seq, iovec *iov, int iovcnt);
    void delete_numbered_object(int seq);
    ssize_t read_object(const char *name, char *buf, size_t len, size_t offset);
    ssize_t read_numbered_object(int seq, char *buf, size_t len, size_t offset);
    ssize_t read_numbered_objectv(int seq, iovec *iov, int iovcnt, size_t offset);

    struct rados_aio {
        void (*cb)(void*);
        void *ptr;
        rados_completion_t c;
    };

    static void aio_read_done(rados_completion_t c, void *ptr);
    int aio_read_num_object(int seq, char *buf, size_t len, size_t offset,
                        void (*cb)(void*), void *ptr);
    int aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
                                  void (*cb)(void*), void *ptr);

    ~rados_backend() {
        free((void*)prefix);
        rados_ioctx_destroy(io_ctx);
        rados_shutdown(cluster);
    }
    std::string object_name(int seq);
};

#endif

