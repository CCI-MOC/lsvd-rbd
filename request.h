#ifndef REQUEST_H
#define REQUEST_H


class IORequest {
	bool notified;
	bool done;
	void *write_ptr;
public:

IORequest(void* wc);
~IORequest();

void is_done();
void run1(void* sr);
void run2(void* sr);
void notify();

};


#endif
