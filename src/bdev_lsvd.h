#pragma once

#include <rados/librados.h>
#include <string>

int bdev_lsvd_create(std::string img_name, rados_ioctx_t io_ctx);

int bdev_lsvd_delete(std::string img_name);
