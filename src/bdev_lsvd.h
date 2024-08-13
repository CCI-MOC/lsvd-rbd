#pragma once

#include <functional>
#include <rados/librados.h>

#include "config.h"
#include "utils.h"

result<void> bdev_lsvd_create(str img_name, rados_ioctx_t io, lsvd_config cfg);
result<void> bdev_lsvd_create(str pool_name, str image_name, str cfg_path);
void bdev_lsvd_delete(str img_name, std::function<void(result<void>)> cb);
