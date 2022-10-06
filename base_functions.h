/*
 * file:	base_functions.h
 * description: General functions and minor structures for LSVD
 *			-rounding functions
 * author:	Peter Desnoyers, Northeastern University
 *              Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#ifndef BASE_FUNCTIONS_H
#define BASE_FUNCTIONS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>
#include <string>
#include <algorithm>
#include <functional>

enum {DBG_MAP = 1, DBG_HITS = 2, DBG_AIO = 4};

#define DBG(a) 

// https://stackoverflow.com/questions/5008804/generating-random-integer-from-a-range

typedef int64_t sector_t;
typedef int page_t;

/* make this atomic? */
const int BATCH_SIZE = 8 * 1024 * 1024;

// iov_sum :	Takes the sum of lengths of each element in iov
size_t iov_sum(const iovec *iov, int iovcnt);

// hex :	Converts uint32_t number to hex
std::string hex(uint32_t n);

// aligned :	returns if ptr is properly aligned with a-1
bool aligned(const void *ptr, int a);

#endif
