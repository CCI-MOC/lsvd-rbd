#ifndef NVME_H
#define NVME_H

class nvme {
	FILE *fp;
	void *wc;
public:
	nvme(char* filename,void* write_c);
	~nvme();

	nvme_request* make_write_request(bool pad);
};

#endif
