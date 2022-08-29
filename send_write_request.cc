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
#include "send_write_request.h"


send_write_request::send_write_request(nvme_request* r1, void* c_pad, nvme_request* r2, void* c_data,
				       page_t p) {
  r_pad = r1;
  r_data = r2;
  closure_pad = c_pad;
  closure_data = c_data;
  pad = p;
  
  pad_hdr = (char*)aligned_alloc(512, 4096);
  auto one_iovs = new smartiov();
  one_iovs->push_back((iovec){pad_hdr, 4096});
  nvme_request* r_pad = wcache->nvme_w->make_write_request(one_iovs, pad*4096L);
  auto closure = wrap([pad_hdr, one_iovs]{
			free(pad_hdr);
			delete one_iovs;
			return true;
		      });
  
  std::vector<j_extent> extents;
  for (auto _w : *w)
    extents.push_back((j_extent){_w->lba, (uint64_t)_w->sectors});
  
  char *hdr = (char*)aligned_alloc(512, 4096);
  j_hdr *j = mk_header(hdr, LSVD_J_DATA, wcache->my_uuid, 1+blocks);

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
  nvme_request * r_data = nvme_w->make_write_request(iovs, blockno*4096L);
  // closure for data is declared
  auto closure_data = wrap([this, hdr, plba, iovs, w] {
			     /* first update the maps */
			     std::vector<extmap::lba2lba> garbage; 
			     std::unique_lock lk(writew->m);
			     auto _plba = plba;
			     for (auto _w : *w) {
			       writew->map.update(_w->lba, _w->lba + _w->sectors, _plba, &garbage);
			       writew->rmap.update(plba, _plba+_w->sectors, _w->lba);
			       _plba += _w->sectors;
			       writew->map_dirty = true;
			     }
			     for (auto it = garbage.begin(); it != garbage.end(); it++) 
			       writew->rmap.trim(it->s.base, it->s.base+it->s.len);
			     lk.unlock();
			     for (auto _w : *w) {
			       writew->be->writev(_w->lba*512, _w->iovs.data(), _w->iovs.size());
			       _w->callback(_w->ptr);
			       delete _w;
			     }
			     
			     /* and finally clean everything up */
			     free(hdr);
			     delete iovs;
			     delete w;
			     
			     lk.lock();
			     --(writew->writes_outstanding);
			     if (writew->work.size() > 0) {
			       writew->lk.unlock();
			       writew->send_writes();
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

