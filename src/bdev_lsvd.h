#pragma once

#include "utils.h"

CEXTERN int bdev_lsvd_create(const char *pool_name, const char *img_name);

CEXTERN int bdev_lsvd_delete(const char *img_name);
