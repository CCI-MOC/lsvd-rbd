#include <sys/uio.h>
#include <string.h>
#include <vector>

/* this makes readv / writev a lot easier...
 */
class smartiovec {
    std::vector<iovec> iovs;
public:
    smartiovec() {}
    smartiovec(const iovec *iov, int iovcnt) {
	for (int i = 0; i < iovcnt; i++)
	    iovs.push_back(iov[i]);
    }
    void push_back(const iovec &iov) {
	iovs.push_back(iov);
    }
    iovec *data(void) {
	return iovs.data();
    }
    iovec& operator[](int i) {
	return iovs[i];
    }
    int size(void) {
	return iovs.size();
    }
    size_t bytes(void) {
	size_t sum = 0;
	for (auto i : iovs)
	    sum += i.iov_len;
	return sum;
    }
    smartiovec slice(size_t off, size_t limit) {
	smartiovec other;
	size_t len = limit - off;
	for (auto it = iovs.begin(); it != iovs.end() && len > 0; it++) {
	    if (it->iov_len < off)
		off -= it->iov_len;
	    else {
		auto _len = std::min(len, it->iov_len - off);
		other.push_back((iovec){(char*)it->iov_base + off, _len});
		len -= _len;
		off = 0;
	    }
	}
	return other;
    }
    void zero(void) {
	for (auto i : iovs)
	    memset(i.iov_base, 0, i.iov_len);
    }
    void copy_in(char *buf) {
	for (auto i : iovs) {
	    memcpy((void*)i.iov_base, (void*)buf, (size_t)i.iov_len);
	    buf += i.iov_len;
	}
    }
    void copy_out(char *buf) {
	for (auto i : iovs) {
	    memcpy((void*)buf, (void*)i.iov_base, (size_t)i.iov_len);
	    buf += i.iov_len;
	}
    }
    bool aligned(int n) {
	for (auto i : iovs)
	    if (((long)i.iov_base & (n-1)) != 0)
		return false;
	return true;
    }
};

#ifdef TEST

#include <cassert>
#include <cstdlib>

void test1(void)
{
    char *buf1 = (char*)calloc(1001, 1);
    char *buf2 = (char*)calloc(1001, 1);
    memset(buf1, 'A', 1000);
    iovec iov1[] = {{buf1, 117}, {buf1+117, 204}, {buf1+117+204, 412},
		    {buf1+733, 1000-733}};
    auto s = smartiovec(iov1, 4);
    s.copy_out(buf2);
    assert(strlen(buf2) == 1000);

    auto z = s.slice(200,500);  // 0, 83+121, 0+179
    assert(z.bytes() == 300);
    assert(z.size() == 2);
    assert(z.iov()[0].iov_base == buf1+200);
    
    s.slice(200,500).zero();
    assert(strlen(buf1) == 200);
    assert(memchr(buf1+200, 'A', 800) == buf1+500);

    memset(buf2, 0, 1001);
    s.slice(400,700).copy_out(buf2);
    assert(memchr(buf2, 'A', 300) == buf2 + 100);
    assert(strlen(buf2+100) == 200);
}

int main(int argc, char **argv)
{
    test1();
}
#endif
