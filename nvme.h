#ifndef NVME_H
#define NVME_H

class nvme {
	FILE *fp;
	void *wc;
public:
	nvme(char* filename,void* write_c) {
		fp = fopen(filename, "w");
		wc = write_c;
	}
	~nvme() {
		fclose(fp);
	};

	IORequest* make_write_request(/*int offset, iovec iovecs*/void) {
        	IORequest *wr = new IORequest(wc);
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
