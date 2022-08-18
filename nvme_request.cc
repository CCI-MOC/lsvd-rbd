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

nvme_request::nvme_request(smartiov *iov, size_t offset, int type, void* nvme_w) {
	eio = new e_iocb;
	iovs = iov;
	ofs = offset;
	t = type;
	nvme_ptr = nvme_w;
}

bool nvme_request::is_done() {
	return true;
}
void nvme_request::run(void* parent) {
  send_request *sr = (send_request*) parent;
  nvme* disk = (nvme*) nvme_ptr;
  if(t == WRITE_REQ) {/*
		e_io_prep_pwrite(eio, write_c->fd, sr->buf, 4096, sr->pad*4096L, call_send_request_notify, parent);
        	e_io_submit(write_c->ioctx, eio);
		} else {*/
    //assert(ofs+iovs->bytes()/4096L <= write_c->super->limit);
    e_io_prep_pwritev(eio, disk->fp, iovs->data(), iovs->size(), ofs, call_send_request_notify, parent);
    e_io_submit(disk->ioctx, eio);

  } else if(t == READ_REQ) {
    e_io_prep_preadv(eio, disk->fp, iovs->data(), iovs->size(), ofs, call_send_request_notify, parent);
    e_io_submit(disk->ioctx, eio);
  } else { return NULL;}
}
void nvme_request::notify() {
}

nvme_request::~nvme_request() {

}
