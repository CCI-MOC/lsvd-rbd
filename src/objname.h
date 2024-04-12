/*
 * file:        objname.h
 * description: string-like structure for handling object names
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#pragma once

#include <fmt/format.h>

class objname
{
    std::string name;

  public:
    objname(std::string prefix, uint32_t seq)
    {
        name = fmt::format("{}.{:08x}", prefix, seq);
    }

    std::string str() { return name; }
    const char *c_str() { return name.c_str(); }
};
