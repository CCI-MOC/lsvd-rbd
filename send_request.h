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

	send_request(IORequest* r1, void* c_pad, IORequest* r2, void* c_data, page_t p, smartiov* wc_iovs, void* buffer) {
		r_pad = r1;
		r_data = r2;
		closure_pad = c_pad;
		closure_data = c_data;
		pad = p;
		iovs = wc_iovs;
		buf = buffer;
	}
	~send_request() {}

	void run(void* parent) {
		reqs++;
		if(pad) {
			reqs++;
			r_pad->run1(this);
		}
		r_data->run2(this);
	}

	void notify() {
		if(--reqs == 0) {
			if(pad) {
//				closure_pad();
			}
//			closure_data();
			delete this;
		}
	}
};

#endif
