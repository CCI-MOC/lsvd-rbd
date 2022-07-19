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
#include "translate.h"
#include "io.h"

#include "disk.h"

std::pair<size_t,size_t> read_cache::async_read(size_t offset, char *buf, size_t len, void (*cb)(void*), void *ptr) {
        sector_t base = offset/512, sectors = len/512, limit = base+sectors;
        size_t skip_len = 0, read_len = 0;
        extmap::obj_offset oo = {0, 0};






}
