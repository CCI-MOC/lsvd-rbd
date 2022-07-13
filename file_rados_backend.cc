#include <fcntl.h>
#include "file_rados_backend.h"

    int file_backend::get_cached_fd(int seq) {
        std::unique_lock lk(m);
        auto it = cached_fds.find(seq);
        if (it != cached_fds.end())
            return cached_fds[seq];

        if (cached_nums.size() >= fd_cache_size) {
            auto num = cached_nums.front();
            close(cached_fds[num]);
            cached_fds.erase(num);
            cached_nums.pop();
        }
        auto name = std::string(prefix) + "." + hex(seq);
        auto fd = open(name.c_str(), O_RDONLY);
        if (fd < 0)
            throw_fs_error("read_obj_open");
        cached_fds[seq] = fd;
        cached_nums.push(seq);
        return fd;
    }

    ssize_t file_backend::write_object(const char *name, iovec *iov, int iovcnt) {
        int fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0777);
        if (fd < 0)
            return -1;
        auto val = writev(fd, iov, iovcnt);
        close(fd);
        return val;
    }
    ssize_t file_backend::write_numbered_object(int seq, iovec *iov, int iovcnt) {
        auto name = std::string(prefix) + "." + hex(seq);
        return write_object(name.c_str(), iov, iovcnt);
    }
    void file_backend::delete_numbered_object(int seq) {
        auto name = std::string(prefix) + "." + hex(seq);
        unlink(name.c_str());
    }
    ssize_t file_backend::read_object(const char *name, char *buf, size_t len, size_t offset) {
        int fd = open(name, O_RDONLY);
        if (fd < 0)
            return -1;
        auto val = pread(fd, buf, len, offset);
        close(fd);
        if (val < 0)
            throw_fs_error("read_obj");
        return val;
    }
    ssize_t file_backend::read_numbered_object(int seq, char *buf, size_t len, size_t offset) {
        iovec iov = {buf, len};
        return read_numbered_objectv(seq, &iov, 1, offset);
    }
    
    ssize_t file_backend::read_numbered_objectv(int seq, iovec *iov, int iovcnt, size_t offset) {
        auto fd = get_cached_fd(seq);
        auto val = preadv(fd, iov, iovcnt, offset);
        if (val < 0)
            throw_fs_error("read_obj");
        return val;
    }

    int file_backend::aio_read_num_object(int seq, char *buf, size_t len,
                            size_t offset, void (*cb)(void*), void *ptr) {
        int fd = get_cached_fd(seq);
        auto eio = new e_iocb;
        e_io_prep_pread(eio, fd, buf, len, offset, cb, ptr);
        e_io_submit(ioctx, eio);
        return 0;
    }
    
    int file_backend::aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
                                  void (*cb)(void*), void *ptr) {
        auto name = std::string(prefix) + "." + hex(seq);
        int fd = open(name.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0777);
        if (fd < 0)
            return -1;

        auto closure = wrap([fd, cb, ptr]{
                close(fd);
                cb(ptr);
                return true;
            });
        auto eio = new e_iocb;
        size_t offset = 0;
        e_io_prep_pwritev(eio, fd, iov, iovcnt, offset, call_wrapped, closure);
        e_io_submit(ioctx, eio);
        return 0;
    }

    std::string file_backend::object_name(int seq) {
        return std::string(prefix) + "." + hex(seq);
    }

    std::pair<std::string,std::string> split_string(std::string s, std::string delim)
    {
        auto i = s.find(delim);
        return std::pair(s.substr(0,i), s.substr(i+delim.length()));
    }

    ssize_t rados_backend::write_object(const char *name, iovec *iov, int iovcnt) {
        smartiov iovs(iov, iovcnt);
        char *buf = (char*)malloc(iovs.bytes());
        iovs.copy_out(buf);
        int r = rados_write(io_ctx, name, buf, iovs.bytes(), 0);
        free(buf);
        return r;
    }
    ssize_t rados_backend::write_numbered_object(int seq, iovec *iov, int iovcnt) {
        char name[128];
        sprintf(name, "%s.%08x", prefix, seq);
        //auto name = std::string(prefix) + "." + hex(seq);
        return write_object(name, iov, iovcnt);
    }
    void rados_backend::delete_numbered_object(int seq) {
        char name[128];
        sprintf(name, "%s.%08x", prefix, seq);
        rados_remove(io_ctx, name);
    }
    ssize_t rados_backend::read_object(const char *name, char *buf, size_t len, size_t offset) {
        return rados_read(io_ctx, name, buf, len, offset);
    }
    ssize_t rados_backend::read_numbered_object(int seq, char *buf, size_t len, size_t offset) {
        //auto name = std::string(prefix) + "." + hex(seq);
        char name[128];
        sprintf(name, "%s.%08x", prefix, seq);
        //printf("p: %s +: %s\n", name2, name.c_str());
        return read_object(name, buf, len, offset);
    }
    
    ssize_t rados_backend::read_numbered_objectv(int seq, iovec *iov, int iovcnt, size_t offset) {
        smartiov iovs(iov, iovcnt);
        char *buf = (char*)malloc(iovs.bytes());
        int r = read_numbered_object(seq, buf, iovs.bytes(), offset);
        iovs.copy_in(buf);
        free(buf);
        return r;
    }

    void rados_backend::aio_read_done(rados_completion_t c, void *ptr) {
        auto aio = (rados_aio*)ptr;
        aio->cb(aio->ptr);
        rados_aio_release(aio->c);
        delete aio;
    }
    int rados_backend::aio_read_num_object(int seq, char *buf, size_t len, size_t offset,
                        void (*cb)(void*), void *ptr)
    {
        auto name = std::string(prefix) + "." + hex(seq);
        rados_aio *aio = new rados_aio;
        aio->cb = cb;
        aio->ptr = ptr;
        assert(buf != NULL);
        rados_aio_create_completion((void*)aio, aio_read_done, NULL, &aio->c);
        return rados_aio_read(io_ctx, name.c_str(), aio->c, buf, len, offset);
    }
    int rados_backend::aio_write_numbered_object(int seq, iovec *iov, int iovcnt,
                                  void (*cb)(void*), void *ptr) {
        auto name = std::string(prefix) + "." + hex(seq);
        auto rv = write_object(name.c_str(), iov, iovcnt);
        cb(ptr);
        return rv;
    }

    std::string rados_backend::object_name(int seq) {
        return std::string(prefix) + "." + hex(seq);
    }

