/*
 * file:	base_functions.h
 * description: General functions and minor structures for LSVD
 *			-wrapper functions
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

enum {DBG_MAP = 1, DBG_HITS = 2, DBG_AIO = 4};

#define DBG(a) 

// https://stackoverflow.com/questions/5008804/generating-random-integer-from-a-range

typedef int64_t sector_t;
typedef int page_t;

/* make this atomic? */
const int BATCH_SIZE = 8 * 1024 * 1024;

// div_round_up :	This function simply take two numbers and divides them rounding up.
int div_round_up(int n, int m);

// round_up :	This function rounds up a number n to the nearest multiple of m
int round_up(int n, int m);

// iov_sum :	Takes the sum of lengths of each element in iov
size_t iov_sum(const iovec *iov, int iovcnt);

// hex :	Converts uint32_t number to hex
std::string hex(uint32_t n);

/* simple hack so we can pass lambdas through C callback mechanisms
 */
struct wrapper {
    std::function<bool()> f;
    wrapper(std::function<bool()> _f) : f(_f) {}
};

/* invoked function returns boolean - if true, delete the wrapper; otherwise
 * keep it around for another invocation
 */

// wrap:	creates a new wrapper for the inputted function
void *wrap(std::function<bool()> _f);

// call_wrapped:	Invokes a wrapped function stored in location pointed to by pointer and if
//			the function returns a value of TRUE, the wrapped function is deleted
void call_wrapped(void *ptr);

// delete_wrapped:	Wrapped function pointed to by ptr is simply deleted
void delete_wrapped(void *ptr);

// aligned :	returns if ptr is properly aligned with a-1
bool aligned(const void *ptr, int a);

#endif
