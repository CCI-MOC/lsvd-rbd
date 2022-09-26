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
#include "translate.h"
#include "request.h"
#include "nvme.h"
#include "nvme_request.h"
#include "write_cache.h"
#include "send_write_request.h"

extern uuid_t my_uuid;

send_write_request::send_write_request(std::vector<cache_work*> *w,
				       page_t blocks, page_t blockno,
				       page_t pad_pgs, write_cache* wcache)  {

  pad = pad_pgs;
  char * pad_hdr = NULL;

  if (pad > 0) {
      pad_hdr = (char*)aligned_alloc(512, 4096);
      memset(pad_hdr, 0, 4096);
  
      auto one_iovs = new smartiov();
      one_iovs->push_back((iovec){pad_hdr, 4096});
      r_pad = wcache->nvme_w->make_write_request(one_iovs, pad*4096L);
      closure_pad = wrap([pad_hdr, one_iovs]{
	      free(pad_hdr);
	      delete one_iovs;
	      return true;
	  });
  }
  
  std::vector<j_extent> extents;
  for (auto _w : *w)
    extents.push_back((j_extent){_w->lba, (uint64_t)_w->sectors});
  
  char *hdr = (char*)aligned_alloc(512, 4096);
  j_hdr *j = wcache->mk_header(hdr, LSVD_J_DATA, my_uuid, 1+blocks);

  j->extent_offset = sizeof(*j);
  size_t e_bytes = extents.size() * sizeof(j_extent);
  j->extent_len = e_bytes;
  memcpy((void*)(hdr + sizeof(*j)), (void*)extents.data(), e_bytes);

  sector_t plba = (blockno+1) * 8;
  auto iovs = new smartiov();
  iovs->push_back((iovec){hdr, 4096});
  for (auto _w : *w) {
    auto [iov, iovcnt] = _w->iovs.c_iov();
    iovs->ingest(iov, iovcnt);
  }
  r_data = wcache->nvme_w->make_write_request(iovs, blockno*4096L);
  // closure for data is declared
  closure_data = wrap([this, wcache, hdr, plba, iovs, w] {
			     /* first update the maps */
			     std::vector<extmap::lba2lba> garbage; 
			     std::unique_lock lk(wcache->m);
			     auto _plba = plba;
			     for (auto _w : *w) {
			       wcache->map.update(_w->lba, _w->lba + _w->sectors, _plba, &garbage);
			       wcache->rmap.update(plba, _plba+_w->sectors, _w->lba);
			       _plba += _w->sectors;
			       wcache->map_dirty = true;
			     }
			     for (auto it = garbage.begin(); it != garbage.end(); it++) 
			       wcache->rmap.trim(it->s.base, it->s.base+it->s.len);
			     lk.unlock();
			     for (auto _w : *w) {
			       wcache->be->writev(_w->lba*512, _w->iovs.data(), _w->iovs.size());
			       _w->callback(_w->ptr);
			       delete _w;
			     }
			     
			     /* and finally clean everything up */
			     free(hdr);
			     delete iovs;
			     delete w;
			     
			     lk.lock();
			     --(wcache->writes_outstanding);
			     if (wcache->work.size() > 0) {
			       lk.unlock();
			       wcache->send_writes();
			     }
			     return true;
		      });
}
bool send_write_request::is_done() {return true;}
void send_write_request::run(void* parent) {
  reqs++;
  if(pad) {
    reqs++;
    r_pad->run(this);
  }
  r_data->run(this);
}

void send_write_request::notify() {
  if(--reqs == 0) {
    if(pad) {
      call_wrapped(closure_pad);
    }
    call_wrapped(closure_data);
    delete this;
  }
}

send_write_request::~send_write_request() {}

