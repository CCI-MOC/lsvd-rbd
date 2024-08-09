#pragma once

#include <rados/librados.h>
#include <functional>

#include "config.h"

int bdev_lsvd_create(str img_name, rados_ioctx_t io_ctx, lsvd_config cfg);
int bdev_lsvd_create(str pool_name, str image_name, str cfg_path);
void bdev_lsvd_delete(str img_name, std::function<void(int)> cb);
