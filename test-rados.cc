/*
 * file:        test-rados.cc
 * description: simple performance test - create N 8MB objects in parallel
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include <rados/librados.h>

#include <stack>
#include <mutex>
#include <condition_variable>

rados_t cluster;
rados_ioctx_t io_ctx;

double timestamp(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1.0e-6;
}

std::stack<void*> buffers;
std::mutex m;
std::condition_variable cv;
int outstanding;

/* pass buffer as private data
 */
void aio_write_done(rados_completion_t c, void *ptr) {
    //printf("done %p\n", ptr);
    std::unique_lock lk(m);
    buffers.push(ptr);
    cv.notify_one();
    rados_aio_release(c);
}

/* usage: test-rados N T POOL PREFIX
 * N : window size
 * t : runtime (seconds)
 */
int main(int argc, char **argv) {
    int N = atoi(argv[1]);
    double t = atof(argv[2]);
    char *pool = argv[3];
    char *prefix = argv[4];
    
    if (rados_create(&cluster, NULL) < 0)
        printf("error: create\n"), exit(1);
    if (rados_conf_read_file(cluster, NULL) < 0)
        printf("error: conf\n"), exit(1);
    if (rados_connect(cluster) < 0)
        printf("error: connect\n"), exit(1);
    if (rados_ioctx_create(cluster, pool, &io_ctx) < 0)
        printf("error: ioctx\n"), exit(1);

    size_t bytes = 8*1024*1024;
    for (int i = 0; i < N; i++) {
	auto buf = malloc(bytes);
	//printf("push %p\n", buf);
	buffers.push(buf);
    }

    double t0 = timestamp();
    int rnd = (t0 - (int)t0) * 1000;
    
    double t1;
    int seq = 0;

    while ((t1 = timestamp()) - t0 < t) {
	void *buf;
	{
	    std::unique_lock lk(m);
	    while (buffers.size() == 0)
		cv.wait(lk);
	    buf = buffers.top();
	    buffers.pop();
	}
	rados_completion_t c;
	rados_aio_create_completion(buf, aio_write_done, NULL, &c);
	char name[128];
	sprintf(name, "%s.%d.%d", prefix, rnd, seq);
	printf(".");
	fflush(stdout);
	if (rados_aio_write(io_ctx, name, c, (char*)buf, bytes, 0) < 0)
	    printf("aio_write\n"), exit(1);
	seq++;
    }
    {
	std::unique_lock lk(m);
	while (buffers.size() < N)
	    cv.wait(lk);
    }
    double t2 = timestamp(), t3 = t2-t0;
    double decimal = 1024*1024/1.0e6;
    
    printf("\n %d objects %.3f sec %.2f MB/s\n",
	   seq, t3, seq*8/(t3*decimal));
}
