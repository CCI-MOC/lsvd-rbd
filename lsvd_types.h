/*
 * file:        lsvd_types.h
 * description: basic types
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

#endif

