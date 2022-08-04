#ifndef NVME_H
#define NVME_H

class nvme {
public:
	nvme(char* filename);
	~nvme();

IORequest make_write_request(int offset, iovec iovecs);
/*
void notify(IORequest *R);
void wait();
void is_done();
*/



};

#endif
