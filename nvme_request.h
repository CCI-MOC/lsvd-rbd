// file: 	nvme_request.h
// description:	nvme level request structure using the request interface
#ifndef NVME_REQUEST_H
#define NVME_REQUEST_H

class send_write_request;

void call_send_request_notify(void *ptr);

#define WRITE_REQ 1
//#define WRITEV_REQ 2
#define READ_REQ 3
//#define READV_REQ 4
/*
class nvme_request : public request {

  e_iocb* eio;

public:
  nvme_request(void *buf, size_t len, size_t offset, int type);
	bool is_done(void);
	void run(void *parent);
	void notify(void);
	~nvme_request();3

};
*/
class nvme_request : public request {

  e_iocb* eio;
  smartiov* iovs;
  size_t ofs;
  int t;
  nvme* nvme_ptr;
  send_write_request* sr;
public:
  nvme_request(smartiov *iov, size_t offset, int type, nvme* nvme_w);
	bool is_done(void);
	void run(void *parent);
	void notify(void);
	~nvme_request();
};

#endif
