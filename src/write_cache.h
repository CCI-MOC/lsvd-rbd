#pragma once

#include "lsvd_types.h"
#include "translate.h"
#include "utils.h"

/* all addresses are in units of 4KB blocks
 */
class write_cache
{
  public:
    virtual void get_room(sector_t sectors) = 0;
    virtual void release_room(sector_t sectors) = 0;
    virtual void flush(void) = 0;

    virtual ~write_cache() {}

    virtual request *writev(sector_t lba, smartiov *iov) = 0;

    virtual void do_write_checkpoint(void) = 0;
};

uptr<write_cache> make_write_cache(uint32_t blkno, int fd, translate *be,
                                   lsvd_config *cfg);
