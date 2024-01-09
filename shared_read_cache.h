#pragma once

#include <array>
#include <atomic>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/framework/extractor.hpp>
#include <boost/accumulators/statistics.hpp>
#include <boost/accumulators/statistics/rolling_count.hpp>
#include <boost/accumulators/statistics/rolling_sum.hpp>
#include <boost/bimap.hpp>
#include <boost/container_hash/hash.hpp>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include "backend.h"
#include "extent.h"
#include "nvme.h"
#include "objname.h"
#include "request.h"
#include "shared_read_cache.h"
#include "smartiov.h"
#include "utils.h"

const size_t CACHE_CHUNK_SIZE = 64 * 1024;

class read_cache
{
  public:
    read_cache() {}
    virtual ~read_cache() {}

    virtual bool should_bypass_cache(std::string img_prefix, uint64_t seqnum,
                                     size_t offset) = 0;
    virtual void served_bypass_request(std::string img_prefix, uint64_t seqnum,
                                       size_t offset, size_t bytes) = 0;
    virtual request *make_read_req(std::string img_prefix, uint64_t seqnum,
                                   size_t offset, size_t adjust,
                                   smartiov &dest) = 0;
    virtual void insert_object(std::string img_prefix, uint64_t seqnum,
                               size_t obj_size, void *obj_data) = 0;
};

/**
 * The shared read cache should be *shared*, so we use a singleton
 * The passed in params are only used on 1st call to construct the cache;
 * it's ignored on all subsequent calls
 */
sptr<read_cache> get_read_cache_instance(std::string cache_dir,
                                         size_t cache_bytes,
                                         sptr<backend> obj_backend);
