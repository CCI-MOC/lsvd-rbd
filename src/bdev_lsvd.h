#pragma once

#include <functional>
#include <rados/librados.h>

#include "config.h"
#include "utils.h"

Result<void> bdev_lsvd_create(str img_name, rados_ioctx_t io, lsvd_config cfg);
Result<void> bdev_lsvd_create(str pool_name, str image_name, str cfg_path);
void bdev_lsvd_delete(str img_name, std::function<void(Result<void>)> cb);
