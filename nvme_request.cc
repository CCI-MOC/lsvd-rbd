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
#include "nvme_request.h"
#include "translate.h"
#include "write_cache.h"
#include "send_write_request.h"


void call_send_request_notify(void *parent)
{
    nvme_request *r = (nvme_request*) parent;
    r->notify();
}

nvme_request::nvme_request(smartiov *iov, size_t offset, int type, nvme* nvme_w) {
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
  sr = (send_write_request*) parent;
  if(t == WRITE_REQ) {
    //assert(ofs+iovs->bytes()/4096L <= write_c->super->limit);
    e_io_prep_pwritev(eio, nvme_ptr->fp, iovs->data(), iovs->size(), ofs, call_send_request_notify, this);
    e_io_submit(nvme_ptr->ioctx, eio);

  } else if(t == READ_REQ) {
    e_io_prep_preadv(eio, nvme_ptr->fp, iovs->data(), iovs->size(), ofs, call_send_request_notify, this);
    e_io_submit(nvme_ptr->ioctx, eio);
  } else {}
}
void nvme_request::notify() {
	sr->notify();
	delete this;
}

nvme_request::~nvme_request() {

}
