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

#include <sys/uio.h>

#include <mutex>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>

#include "base_functions.h"

#include "journal2.h"
#include "smartiov.h"
#include "objects.h"
#include "extent.h"
#include "misc_cache.h"
#include "backend.h"
#include "io.h"
#include "request.h"
#include "nvme.h"
#include "send_request.h"
#include "translate.h"
#include "write_cache.h"

IORequest::IORequest(void* wc) {
        notified = false;
        done = false;
	write_ptr = wc;
}
IORequest::~IORequest() {}

void IORequest::is_done() {}

void IORequest::run1(void* sr) {
	send_request *sr_pnt = (send_request*) sr;
	write_cache *write_c = (write_cache*) write_ptr;
	auto eio = new e_iocb;
	e_io_prep_pwrite(eio, write_c->fd, sr_pnt->buf, 4096, sr_pnt->pad*4096L, call_wrapped, sr_pnt->closure_pad);
	e_io_submit(write_c->ioctx, eio);
	sr_pnt->notify();
}

void IORequest::run2(void* sr) {
        send_request *sr_pnt = (send_request*) sr;
        write_cache *write_c = (write_cache*) write_ptr;
	auto eio = new e_iocb;
	assert(write_c->blockno+sr_pnt->iovs->bytes()/4096L <= write_c->super->limit);
	e_io_prep_pwritev(eio, write_c->fd, sr_pnt->iovs->data(), sr_pnt->iovs->size(), write_c->blockno*4096L,
			  call_wrapped, sr_pnt->closure_data);
	e_io_submit(write_c->ioctx, eio);
	sr_pnt->notify();
}

void IORequest::notify() {}

