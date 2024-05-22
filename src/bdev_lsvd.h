#pragma once

#include <rados/librados.h>

#include "config.h"

int bdev_lsvd_create(str img_name, rados_ioctx_t io_ctx, lsvd_config cfg);
int bdev_lsvd_delete(str img_name);
