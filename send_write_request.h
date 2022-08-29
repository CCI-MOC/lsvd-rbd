#ifndef SEND_WRITE_REQUEST_H
#define SEND_WRITE_REQUEST_H

class write_cache;

class send_write_request : public request {
public:
	nvme_request* r_pad;
	nvme_request* r_data;
	void* 	closure_pad;
	void* 	closure_data;
	page_t pad;

	std::atomic<int> reqs = 0;

  send_write_request(std::vector<cache_work*> *w, page_t blocks, page_t blockno, page_t p, write_cache* wcache);
	bool is_done();
	void run(void* parent);
	void notify();
	~send_write_request();

};

#endif
