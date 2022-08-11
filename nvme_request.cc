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
#include "nvme_request.h"
#include "nvme.h"
#include "send_request.h"
#include "translate.h"
#include "write_cache.h"

void call_send_request_notify(void *parent)
{
    send_request *sr = (send_request*) parent;
    sr->notify();
}

nvme_request::nvme_request(void *wc, bool pad) {
	write_cash = wc;
	is_pad = pad;
	eio = new e_iocb;
}

bool nvme_request::is_done() {
	return true;
}
void nvme_request::run(void* parent) {
	write_cache* write_c = (write_cache*) write_cash;
	send_request *sr = (send_request*) parent;
	if(is_pad) {
		e_io_prep_pwrite(eio, write_c->fd, sr->buf, 4096, sr->pad*4096L, call_send_request_notify, parent);
        	e_io_submit(write_c->ioctx, eio);
	} else {
	        assert(write_c->blockno+sr->iovs->bytes()/4096L <= write_c->super->limit);
	        e_io_prep_pwritev(eio, write_c->fd, sr->iovs->data(), sr->iovs->size(), write_c->blockno*4096L,
                          call_send_request_notify, parent);
        	e_io_submit(write_c->ioctx, eio);

	}
}
void nvme_request::notify() {
}

nvme_request::~nvme_request() {

}
