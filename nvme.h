#ifndef NVME_H
#define NVME_H

class nvme {
public:
    nvme() {};
    virtual ~nvme() {};
    
    virtual request* make_write_request(smartiov *iov, size_t offset) = 0;
    virtual request* make_read_request(smartiov *iov, size_t offset) = 0;
};

enum {
    WRITE_REQ = 1,
    READ_REQ = 3
};

nvme *make_nvme(int fd, const char* name);

#endif
