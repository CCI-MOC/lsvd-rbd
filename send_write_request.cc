/*
 * file:        send_write_request.cc
 * description: write logic for write_cache 
 * 
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <unistd.h>
#include <sys/uio.h>

#include <uuid/uuid.h>

#include <atomic>
#include <condition_variable>
#include <thread>
#include <shared_mutex>
#include <mutex>

#include <vector>
#include <map>
#include <stack>
#include <queue>
#include <cassert>

#include <algorithm>

#include "base_functions.h"

#include "journal.h"
#include "smartiov.h"
#include "extent.h"
#include "misc_cache.h"
#include "backend.h"
#include "io.h"
#include "translate.h"
#include "request.h"
#include "nvme.h"
#include "write_cache.h"
#include "write_cache_impl.h"
#include "send_write_request.h"

extern uuid_t my_uuid;






bool      send_write_request::is_done() {return true;}
void      send_write_request::wait() {}
sector_t  send_write_request::lba() {return 0;}
smartiov *send_write_request::iovs() {return NULL;}
void      send_write_request::release() {}


