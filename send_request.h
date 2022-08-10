#ifndef SEND_REQUEST_H
#define SEND_REQUEST_H

class send_request {
public:
	IORequest* r_pad;
	IORequest* r_data;
	void* 	closure_pad;
	void* 	closure_data;
	page_t	pad;
	smartiov* iovs;
	void* 	buf;
	std::atomic<int> reqs = 0;

	send_request(IORequest* r1, void* c_pad, IORequest* r2, void* c_data, page_t p, smartiov* wc_iovs, void* buffer);
	~send_request();

	void run(void* parent);

	void notify();
};

#endif
