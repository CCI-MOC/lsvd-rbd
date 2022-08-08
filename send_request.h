#ifndef SEND_REQUEST_H
#define SEND_REQUEST_H

class send_request {

	IORequest* r_pad;
	IORequest* r_data;
	void* 	closure_pad;
	void* 	closure_data;
	page_t	pad;
	std::atomic<int> reqs = 0;
public:

	send_request(IORequest* r1, void* c_pad, IORequest* r2, void* c_data, page_t p) {
		r_pad = r1;
		r_data = r2;
		closure_pad = c_pad;
		closure_data = c_data;
		pad = p;
	}
	~send_request() {}

	void run() {
		reqs++;
		if(pad) {
			reqs++;
			r_pad->run(this);
		}
		r_data->run(this);
	}

	void notify() {
		if(--reqs == 0) {
			if(pad) {
				closure_pad();
			}
			closure_data();
			delete this;
		}
	}
};

#endif
