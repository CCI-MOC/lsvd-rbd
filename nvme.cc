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
#include "send_request.h"
#include "write_cache.h"

        nvme::nvme(char* filename,void* write_c) {
                fp = fopen(filename, "w");
                wc = write_c;
        }
        nvme::~nvme() {
                fclose(fp);
        };

        nvme_request* nvme::make_write_request(bool pad) {
                nvme_request *wr = new nvme_request(wc, pad);
                return wr;
        }




