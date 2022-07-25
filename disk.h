#ifndef DISK_H
#define DISK_H

class disk {
public:
	disk();
	~disk();
	void e_io_pwrite_submit(io_context_t ioctx, int fd, void *buf, size_t len, size_t offset,
                      void (*cb)(void*), void *arg);
//        ssize_t disk::writev(size_t offset, iovec *iov, int iovcnt, batch *current_batch, std::stack<batch*>  batches);



};

#endif
