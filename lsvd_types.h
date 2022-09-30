/*
 * file:        lsvd_types.h
 * description: basic types
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#ifndef __LSVD_TYPES_H__
#define __LSVD_TYPES_H__

#include <stdint.h>

typedef int64_t sector_t;
typedef int page_t;

enum lsvd_op {
    OP_READ = 2,
    OP_WRITE = 4
};

enum { LSVD_MAGIC = 0x4456534c };

#endif

