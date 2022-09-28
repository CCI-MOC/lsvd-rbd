#include <shared_mutex>
#include <condition_variable>
#include <queue>
#include <thread>

#include <uuid/uuid.h>
#include <sys/uio.h>

#include <vector>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>

#include "base_functions.h"
#include "smartiov.h"
#include "extent.h"
#include <mutex>

#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;

#include "misc_cache.h"

