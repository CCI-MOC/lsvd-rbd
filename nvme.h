#ifndef NVME_H
#define NVME_H

class nvme {
  // FILE *fp;
	int fp;
  bool e_io_running = false;
  std::thread e_io_th;
  io_context_t ioctx;
public:

  nvme(char* filename);
  ~nvme();

  nvme_request* make_write_request(smartiov *iov, size_t offset);
  nvme_request* make_read_request(smartiov *iov, size_t offset);

};

#endif
