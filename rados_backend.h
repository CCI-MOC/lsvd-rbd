// file:	rados_backend.h
// description: This file contains all the rados_backend class functions. All functions used by LSVD
//		which focus on manipulating data to and from the rados backend
// author:      Peter Desnoyers, Northeastern University
//              Copyright 2021, 2022 Peter Desnoyers
// license:     GNU LGPL v2.1 or newer
//              LGPL-2.1-or-later

#ifndef RADOS_BACKEND_H
#define RADOS_BACKEND_H

#include "backend.h"

extern backend *make_rados_backend();

#endif

