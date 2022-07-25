#include <libaio.h>
#include <uuid/uuid.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <atomic>
#include <map>
#include <condition_variable>
#include <thread>
#include <stack>
#include <queue>
#include <cassert>
#include <shared_mutex>
#include "base_functions.h"

#include "journal2.h"
#include "smartiov.h"
#include "objects.h"
#include "extent.h"
#include "misc_cache.h"
#include "backend.h"
#include "io.h"
#include "disk.h"

void disk::e_io_pwrite_submit(io_context_t ioctx, int fd, void *buf, size_t len, size_t offset,
                      void (*cb)(void*), void *arg) {
	auto eio = new e_iocb;
	e_io_prep_pwrite(eio, fd, buf, len, offset, cb, arg);
	e_io_submit(ioctx, eio);
}
/*
ssize_t disk::writev(size_t offset, iovec *iov, int iovcnt, batch *current_batch, std::stack<batch*>  batches) {
        std::unique_lock<std::mutex> lk(m);
        size_t len = iov_sum(iov, iovcnt);

        while (puts_outstanding >= 32)
            cv.wait(lk);

        if (current_batch && current_batch->len + len > current_batch->max) {
            last_pushed = current_batch->seq;
            workers.put_locked(current_batch);
            current_batch = NULL;
        }
        if (current_batch == NULL) {
            if (batches.empty()) {
                current_batch = new batch(BATCH_SIZE);
            }
            else {
                current_batch = batches.top();
                batches.pop();
            }
            current_batch->reset();
            in_mem_objects[current_batch->seq] = current_batch->buf;
        }

        int64_t sector_offset = current_batch->len / 512,
            lba = offset/512, limit = (offset+len)/512;
        current_batch->append_iov(offset / 512, iov, iovcnt);

        std::vector<extmap::lba2obj> deleted;
        extmap::obj_offset oo = {current_batch->seq, sector_offset};
        std::unique_lock objlock(map->m);

        map->map.update(lba, limit, oo, &deleted);
        for (auto d : deleted) {
            auto [base, limit, ptr] = d.vals();
            object_info[ptr.obj].live -= (limit - base);
            total_live_sectors -= (limit - base);
        }

        return len;

}
*/
