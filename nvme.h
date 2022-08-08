#ifndef NVME_H
#define NVME_H

class nvme {
	FILE *fp;
public:
	nvme(char* filename) {
		fp = fopen(filename, "w");
	}
	~nvme() {
		fclose(fp);
	};

	IORequest* make_write_request(/*int offset, iovec iovecs*/void) {
        	IORequest *wr = new IORequest;
        	return wr;
	}


//IORequest* make_write_request(/*int offset, iovec iovecs*/void);
/*
void notify(IORequest *R);
void wait();
void is_done();
*/



};

#endif
