#ifndef NVME_H
#define NVME_H

class nvme {
	FILE *fp;
public:
	nvme(char* filename) {
		fopen(filename, "w");
	}
	~nvme() {
		fclose(fp);
	};

IORequest make_write_request(int offset, iovec iovecs);
/*
void notify(IORequest *R);
void wait();
void is_done();
*/



};

#endif
