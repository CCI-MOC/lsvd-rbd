#ifndef NVME_H
#define NVME_H

class nvme {
	FILE *fp;
	void *wc;
public:
	nvme(char* filename,void* write_c);
	~nvme();

	IORequest* make_write_request(/*int offset, iovec iovecs*/void);
};

#endif
