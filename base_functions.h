/*
base_functions.h : A lot of base generic functions and minor structures used
by many portions of the lsvd system:
	-debug initializations
	-_log structure
	-rounding up functions
	-wrapper functions
*/

#ifndef BASE_FUNCTIONS_H
#define BASE_FUNCTIONS_H

#include <uuid/uuid.h>
#include <sys/uio.h>

#include <vector>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>

std::mutex printf_m;
bool _debug_init_done;
int  _debug_mask;
enum {DBG_MAP = 1, DBG_HITS = 2, DBG_AIO = 4};

void debug_init(void)
{
    if (getenv("DBG_AIO"))
	_debug_mask |= DBG_AIO;
    if (getenv("DBG_HITS"))
	_debug_mask |= DBG_HITS;
    if (getenv("DBG_MAP"))
	_debug_mask |= DBG_MAP;
}
    
#define xprintf(mask, ...) do { \
    if (!_debug_init_done) debug_init(); \
    if (mask & _debug_mask) { \
	std::unique_lock lk(printf_m); \
	fprintf(stderr, __VA_ARGS__); \
    }} while (0)

struct _log {
    int l;
    pthread_t th;
    long arg;
} *logbuf, *logptr;

void dbg(int l, long arg)
{
    logptr->l = l;
    logptr->th = pthread_self();
    logptr->arg = arg;
    logptr++;
}
//#define DBG(a) dbg(__LINE__, a)
#define DBG(a) 

void printlog(FILE *fp)
{
    while (logbuf != logptr) {
	fprintf(fp, "%d %lx %lx\n", logbuf->l, logbuf->th, logbuf->arg);
	logbuf++;
    }
}

// https://stackoverflow.com/questions/5008804/generating-random-integer-from-a-range
std::random_device rd;     // only used once to initialise (seed) engine
//std::mt19937 rng(rd());  // random-number engine used (Mersenne-Twister in this case)
std::mt19937 rng(17);      // for deterministic testing

typedef int64_t sector_t;
typedef int page_t;

/* make this atomic? */
int batch_seq;
int last_ckpt;
const int BATCH_SIZE = 8 * 1024 * 1024;
uuid_t my_uuid;

int div_round_up(int n, int m)
{
    return (n + m - 1) / m;
}
int round_up(int n, int m)
{
    return m * div_round_up(n, m);
}

size_t iov_sum(const iovec *iov, int iovcnt)
{
    size_t sum = 0;
    for (int i = 0; i < iovcnt; i++)
	sum += iov[i].iov_len;
    return sum;
}

std::string hex(uint32_t n)
{
    std::stringstream stream;
    stream << std::setfill ('0') << std::setw(8) << std::hex << n;
    return stream.str();
}

/* ------- */

/* simple hack so we can pass lambdas through C callback mechanisms
 */
struct wrapper {
    std::function<bool()> f;
    wrapper(std::function<bool()> _f) : f(_f) {}
};

/* invoked function returns boolean - if true, delete the wrapper; otherwise
 * keep it around for another invocation
 */
void *wrap(std::function<bool()> _f)
{
    auto s = new wrapper(_f);
    return (void*)s;
}

void call_wrapped(void *ptr)
{
    auto s = (wrapper*)ptr;
    if (std::invoke(s->f))
	delete s;
}

void delete_wrapped(void *ptr)
{
    auto s = (wrapper*)ptr;
    delete s;
}

/* ----- */

#endif
