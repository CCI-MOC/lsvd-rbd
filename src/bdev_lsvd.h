#pragma once

#include "backend.h"
#include "utils.h"

using str = std::string;

Result<void> bdev_lsvd_create(str pool, str name, str cfg);
void bdev_lsvd_delete(str img_name, std::function<void(Result<void>)> cb);
