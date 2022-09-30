/*
 * file:        rados_backend.h
 * description: backend interface using RADOS objects
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#ifndef RADOS_BACKEND_H
#define RADOS_BACKEND_H

#include "backend.h"

extern backend *make_rados_backend();

#endif

