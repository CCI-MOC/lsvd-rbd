#ifndef SEND_WRITE_REQUEST_H
#define SEND_WRITE_REQUEST_H

class send_write_request : public request {
public:
	nvme_request* r_pad;
	nvme_request* r_data;
	void* 	closure_pad;
	void* 	closure_data;
	page_t pad;

	std::atomic<int> reqs = 0;

	send_write_request(nvme_request* r1, void* c_pad, nvme_request* r2, void* c_data, page_t p);
	bool is_done();
	void run(void* parent);
	void notify();
	~send_write_request();

};

#endif
