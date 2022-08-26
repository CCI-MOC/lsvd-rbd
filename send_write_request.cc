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


        send_write_request::send_write_request(nvme_request* r1, void* c_pad, nvme_request* r2, void* c_data, page_t p) {
                r_pad = r1;
                r_data = r2;
                closure_pad = c_pad;
                closure_data = c_data;
		pad = p;
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

