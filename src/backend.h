#pragma once

#include <folly/File.h>
#include <rados/librados.h>
#include <sys/uio.h>

#include "representation.h"
#include "smartiov.h"
#include "utils.h"

class ObjStore
{
  public:
    virtual ~ObjStore() {}

    virtual TaskRes<usize> get_size(strc name) = 0;

    virtual TaskRes<vec<byte>> read_all(strc name) = 0;
    virtual TaskRes<u32> read(strc name, off_t offset, smartiov &iov) = 0;
    virtual TaskRes<u32> read(strc name, off_t offset, iovec iov) = 0;
    virtual TaskRes<u32> write(strc name, smartiov &iov) = 0;
    virtual TaskRes<u32> write(strc name, iovec iov) = 0;
    virtual TaskUnit remove(strc name) = 0;

    static Result<sptr<ObjStore>> connect_to_pool(fstr pool_name);
};

class FileIo
{
  public:
    virtual ~FileIo() = 0;
    virtual TaskRes<u32> preadv(off_t offset, smartiov iov) = 0;
    virtual TaskRes<u32> pwritev(off_t offset, smartiov iov) = 0;

    // Does NOT take ownership of the fd
    static uptr<FileIo> make_file_io(s32 fd);
};
