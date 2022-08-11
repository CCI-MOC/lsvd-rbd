#ifndef NVME_REQUEST_H
#define NVME_REQUEST_H

void call_send_request_notify(void *ptr);

class nvme_request : public request {

	e_iocb* eio;
	bool is_pad;
	void* write_cash;
public:
	nvme_request(void* wc, bool pad);
	bool is_done(void);
	void run(void *parent);
	void notify(void);
	~nvme_request();

};

#endif
