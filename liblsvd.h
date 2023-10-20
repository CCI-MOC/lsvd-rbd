#pragma once

#include "backend.h"
#include "config.h"
#include "fake_rbd.h"

std::unique_ptr<backend> get_backend(lsvd_config *cfg, rados_ioctx_t io,
                                     const char *name);
