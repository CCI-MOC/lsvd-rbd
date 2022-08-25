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
#include "nvme_request.h"
#include "nvme.h"
#include "send_write_request.h"
#include "write_cache.h"

        nvme::nvme(int fd, const char* name) {
	        fp = fd;
                e_io_running = true;
		io_queue_init(64, &ioctx);
		e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name);
	
        }
        nvme::~nvme() {
	  e_io_running = false;
	  e_io_th.join();
	  io_queue_release(ioctx);

	 
        };

        nvme_request* nvme::make_write_request(smartiov *iov, size_t offset) {
                nvme_request *wr = new nvme_request(iov, offset, WRITE_REQ, this);
                return wr;
        }

        nvme_request* nvme::make_read_request(smartiov *iov, size_t offset) {
                nvme_request *wr = new nvme_request(iov, offset, READ_REQ, this);
                return wr;
        }




