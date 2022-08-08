#ifndef REQUEST_H
#define REQUEST_H

class IORequest {
	bool notified;
	bool done;

public:

IORequest();
~IORequest();

void is_done();
void run();
void notify();

};


#endif
