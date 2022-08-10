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
#include "request.h"
#include "send_request.h"


        send_request::send_request(IORequest* r1, void* c_pad, IORequest* r2, void* c_data, page_t p, smartiov* wc_iovs, void* buffer) {
                r_pad = r1;
                r_data = r2;
                closure_pad = c_pad;
                closure_data = c_data;
                pad = p;
                iovs = wc_iovs;
                buf = buffer;
        }
        send_request::~send_request() {}

        void send_request::run(void* parent) {
                reqs++;
                if(pad) {
                        reqs++;
                        r_pad->run1(this);
                }
                r_data->run2(this);
        }

        void send_request::notify() {
                if(--reqs == 0) {
                        if(pad) {
//                              closure_pad();
                        }
//                      closure_data();
                        delete this;
                }
        }

