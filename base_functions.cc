/*
 * file:	base_functions.cc
 * description: General functions and minor structures for LSVD
 *			-wrapper functions
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
int batch_seq;
int last_ckpt;
uuid_t my_uuid;

int div_round_up(int n, int m)
{
    return (n + m - 1) / m;
}

int round_up(int n, int m)
{
    return m * div_round_up(n, m);
}

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

void *wrap(std::function<bool()> _f)
{
    auto s = new wrapper(_f);
    return (void*)s;
}

void call_wrapped(void *ptr)
{
    auto s = (wrapper*)ptr;
    if (std::invoke(s->f))
        delete s;
}

void delete_wrapped(void *ptr)
{
    auto s = (wrapper*)ptr;
    delete s;
}

bool aligned(const void *ptr, int a) {
    return 0 == ((long)ptr & (a-1));
}

