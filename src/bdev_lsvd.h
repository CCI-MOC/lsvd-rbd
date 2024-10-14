#pragma once

#include "backend.h"
#include "utils.h"

using str = std::string;
using ResUnit = Result<folly::Unit>;

ResUnit bdev_lsvd_create(str pool, str name, str cfg);
void bdev_lsvd_delete(str img_name, std::function<void(ResUnit)> cb);

ResUnit bdev_noop_create(str name, usize size);