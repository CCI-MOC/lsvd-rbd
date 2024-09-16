#include <folly/executors/CPUThreadPoolExecutor.h>
#include <uuid.h>

#include "backend.h"
#include "config.h"
#include "extmap.h"
#include "journal.h"
#include "read_cache.h"
#include "smartiov.h"
#include "utils.h"
#include "writelog.h"

template <typename T> using FutRes = folly::Future<Result<T>>;

class LsvdImage
{
  public:
    const str name;
    const uuid_t uuid;
    const usize size_bytes;
    const usize block_size = 4096;

  private:
    folly::Executor::KeepAlive<> &executor;
    LsvdConfig cfg;
    ExtMap extmap;
    sptr<Backend> s3;
    sptr<ReadCache> cache;
    sptr<Journal> journal;
    sptr<LsvdLog> wlog;

  public:
    static Result<uptr<LsvdImage>> mount(sptr<Backend> s3, str name,
                                         str config);

    FutRes<void> read(off_t offset, smartiov iovs);
    FutRes<void> write(off_t offset, smartiov iovs);
    FutRes<void> trim(off_t offset, usize len);
    FutRes<void> flush();

    static FutRes<void> create(sptr<Backend> s3, str name);
    static FutRes<void> remove(sptr<Backend> s3, str name);
    static FutRes<void> clone(sptr<Backend> s3, str src, str dst);

  private:
    ResTask<void> read_task(off_t offset, smartiov iovs);
    ResTask<void> write_task(off_t offset, smartiov iovs);
    ResTask<void> trim_task(off_t offset, usize len);
    ResTask<void> flush_task();
};