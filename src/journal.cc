#include "folly/FileUtil.h"

#include "journal.h"
#include "representation.h"
#include "utils.h"

Journal::Journal(s32 fd, sptr<FileIo> journ_io, usize journ_size)
    : fd(fd), journ_io(journ_io), journ_size(journ_size)
{
}

Journal::~Journal() { folly::closeNoInt(fd); }

Result<uptr<Journal>> Journal::open(fspath path, usize size)
{
    XLOGF(INFO, "Initialising journal at {} with size {}", path.string(), size);
    int fd = folly::openNoInt(path.c_str(), O_RDWR | O_CREAT);
    if (fd < 0)
        return absl::ErrnoToStatus(-errno, "Failed to open journal file");

    int tr = folly::ftruncateNoInt(fd, size);
    if (tr < 0)
        return absl::ErrnoToStatus(-errno, "Failed to truncate journal file");

    auto journ_io = FileIo::make_file_io(fd);
    if (!journ_io)
        return absl::InternalError("Failed to create journal file io");

    return uptr<Journal>(new Journal(fd, std::move(journ_io), size));
}

enum class journ_entry_type : u64 {
    WRITE = 0,
    TRIM = 1,
};

struct journ_entry {
    u64 type : 2;
    u64 offset : 62;
    u64 len;
    S3Ext ext;
};

// TODO: this currently just writes the data to a journal file linearly, with
// no mechanism to sync, wait for writes, etc. It's good enough to emulate
// the performance characteristics of a real journal, but provides no durability

TaskUnit Journal::record_write(off_t offset, iovec iov, S3Ext ext)
{
    ENSURE(offset >= 0);
    usize cur_off, new_off;

    do {
        cur_off = cur_offset.load();
        new_off = cur_off + sizeof(journ_entry) + iov.iov_len;
        if (new_off > journ_size)
            new_off = 0;
    } while (!cur_offset.compare_exchange_weak(cur_off, new_off));

    auto entry = journ_entry{
        .type = WRITE,
        .offset = (u64)(offset),
        .len = iov.iov_len,
        .ext = ext,
    };

    iovec entry_iov = {
        .iov_base = (void *)&entry,
        .iov_len = sizeof(entry),
    };

    auto jiov = smartiov::from_iovecs(entry_iov, iov);
    auto write_res = co_await journ_io->pwritev(cur_off, jiov);
    CRET_IF_NOTOK(write_res);

    // TODO handle partial write
    co_return folly::Unit();
}

TaskUnit Journal::record_trim(off_t offset, usize len, S3Ext ext)
{
    ENSURE(offset >= 0);
    usize cur_off, new_off;

    do {
        cur_off = cur_offset.load();
        new_off = cur_off + sizeof(journ_entry);
        if (new_off > journ_size)
            new_off = 0;
    } while (!cur_offset.compare_exchange_weak(cur_off, new_off));

    auto entry = journ_entry{
        .type = WRITE,
        .offset = (u64)(offset),
        .len = len,
        .ext = ext,
    };

    iovec entry_iov = {
        .iov_base = (void *)&entry,
        .iov_len = sizeof(entry),
    };

    auto jiov = smartiov::from_iovecs(entry_iov);
    auto write_res = co_await journ_io->pwritev(cur_off, jiov);
    CRET_IF_NOTOK(write_res);

    // TODO handle partial write
    co_return folly::Unit();
}
