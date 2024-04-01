#include <unistd.h>
#include <uuid/uuid.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <thread>

#include "backend.h"
#include "config.h"
#include "extent.h"
#include "fake_rbd.h"
#include "journal.h"
#include "lsvd_types.h"
#include "misc_cache.h"
#include "nvme.h"
#include "objects.h"
#include "img_reader.h"
#include "request.h"
#include "smartiov.h"
#include "translate.h"
#include "write_cache.h"

// tuple :	used for retrieving maps
struct tuple {
    int base;
    int limit;
    int obj; // object map
    int offset;
    int plba; // write cache map
};

// getmap_s :	more helper structures
struct getmap_s {
    int i;
    int max;
    struct tuple *t;
};

char *logbuf, *p_log, *end_log;
#include <stdarg.h>
std::mutex m;
void do_log(const char *fmt, ...)
{
    std::unique_lock lk(m);
    if (!logbuf) {
        size_t sz = 64 * 1024;
        const char *env = getenv("LSVD_DEBUG_BUF");
        if (env)
            sz = atoi(env);
        p_log = logbuf = (char *)malloc(sz);
        end_log = p_log + sz;
    }
    va_list args;
    va_start(args, fmt);
    ssize_t max = end_log - p_log - 1;
    if (max > 256)
        p_log += vsnprintf(p_log, max, fmt, args);
}

FILE *_fp;
void fp_log(const char *fmt, ...)
{
    // std::unique_lock lk(m);
    if (_fp == NULL)
        _fp = fopen("/tmp/lsvd.log", "w");
    va_list args;
    va_start(args, fmt);
    vfprintf(_fp, fmt, args);
    fflush(_fp);
}

struct timelog {
    uint64_t loc : 8;
    uint64_t val : 48;
    uint64_t time;
};

struct timelog *tl;
std::atomic<int> tl_index;
int tl_max = 10000000;

#include <x86intrin.h>
void log_time(uint64_t loc, uint64_t value)
{
    return;
    if (tl == NULL)
        tl = (struct timelog *)malloc(tl_max * sizeof(struct timelog));
    auto t = __rdtsc();
    auto i = tl_index++;
    if (i < tl_max)
        tl[i] = {loc, value, t};
}

void save_log_time(void)
{
    return;
    FILE *fp = fopen("/tmp/timelog", "wb");
    size_t bytes = tl_index * sizeof(struct timelog);
    fwrite(tl, bytes, 1, fp);
    fclose(fp);
}

/* random run-time debugging stuff, not used at the moment...
 */
#if CRC_DEBUGGER
#include <zlib.h>
static std::map<int, uint32_t> sector_crc;
char zbuf[512];
static std::mutex zm;

void add_crc(sector_t sector, iovec *iov, int niovs)
{
    std::unique_lock lk(zm);
    for (int i = 0; i < niovs; i++) {
        for (size_t j = 0; j < iov[i].iov_len; j += 512) {
            const unsigned char *ptr = j + (unsigned char *)iov[i].iov_base;
            sector_crc[sector] = (uint32_t)crc32(0, ptr, 512);
            sector++;
        }
    }
}

void check_crc(sector_t sector, iovec *iov, int niovs, const char *msg)
{
    std::unique_lock lk(zm);
    for (int i = 0; i < niovs; i++) {
        for (size_t j = 0; j < iov[i].iov_len; j += 512) {
            const unsigned char *ptr = j + (unsigned char *)iov[i].iov_base;
            if (sector_crc.find(sector) == sector_crc.end()) {
                assert(memcmp(ptr, zbuf, 512) == 0);
            } else {
                unsigned crc1 = 0, crc2 = 0;
                assert((crc1 = sector_crc[sector]) ==
                       (crc2 = crc32(0, ptr, 512)));
            }
            sector++;
        }
    }
}

void list_crc(sector_t sector, int n)
{
    for (int i = 0; i < n; i++)
        printf("%ld %08x\n", sector + i, sector_crc[sector + i]);
}
#endif

#include <fcntl.h>
void getcpu(int pid, int tid, int &u, int &s)
{
    if (tid == 0)
        return;
    char procname[128];
    sprintf(procname, "/proc/%d/task/%d/stat", pid, tid);
    int fd = open(procname, O_RDONLY);
    char buf[512], *p = buf;
    read(fd, buf, sizeof(buf));
    close(fd);
    for (int i = 0; i < 13; i++)
        p = strchr(p, ' ') + 1;
    u = strtol(p, &p, 0);
    s = strtol(p, &p, 0);
}

int __dbg_wc_tid;
int __dbg_gc_tid;
int __dbg_fio_tid;
#include <sys/syscall.h>

int get_tid(void) { return syscall(SYS_gettid); }

int __dbg_write1;               // writes launched
int __dbg_write2;               // writes completed
int __dbg_write3, __dbg_write4; // objects written, completed

std::mutex *__dbg_wcache_m;
std::mutex *__dbg_xlate_m;
const char *__dbg_gc_state = "   -";
int __dbg_gc_reads;
int __dbg_gc_writes;
int __dbg_gc_deletes;
int __dbg_gc_n;

int __dbg_t_room;
int __dbg_w_room;

std::atomic<int> __dbg_in_lsvd;

bool read_lock(std::mutex *m)
{
    if (!m)
        return false;
    bool val = m->try_lock();
    if (val)
        m->unlock();
    return !val;
}
bool read_lock(std::shared_mutex *m)
{
    if (!m)
        return false;
    bool val = m->try_lock();
    if (val)
        m->unlock();
    return !val;
}

#include <sys/time.h>
struct timeval tv0;
double gettime(void)
{
    if (tv0.tv_sec == 0)
        gettimeofday(&tv0, NULL);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec - tv0.tv_sec + (tv.tv_usec - tv0.tv_usec) / 1.0e6;
}
