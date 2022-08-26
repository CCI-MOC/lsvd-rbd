#ifndef NVME_H
#define NVME_H

class nvme_request;

class nvme {
  // FILE *fp;
public:
  int fp;
  bool e_io_running = false;
  std::thread e_io_th;
  io_context_t ioctx;


  nvme(int fd, const char* name);
  ~nvme();

  nvme_request* make_write_request(smartiov *iov, size_t offset);
  nvme_request* make_read_request(smartiov *iov, size_t offset);

};

#endif
