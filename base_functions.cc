/*
 * file:	base_functions.cc
 * description: General functions and minor structures for LSVD
 *			-rounding functions
 * author:	Peter Desnoyers, Northeastern University
 *              Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <uuid/uuid.h>
#include <sys/uio.h>

#include <vector>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>
#include "base_functions.h"

std::mt19937 rng(17);
int last_ckpt;
uuid_t my_uuid;

size_t iov_sum(const iovec *iov, int iovcnt)
{
    size_t sum = 0;
    for (int i = 0; i < iovcnt; i++)
        sum += iov[i].iov_len;
    return sum;
}

std::string hex(uint32_t n)
{
    std::stringstream stream;
    stream << std::setfill ('0') << std::setw(8) << std::hex << n;
    return stream.str();
}

bool aligned(const void *ptr, int a) {
    return 0 == ((long)ptr & (a-1));
}

