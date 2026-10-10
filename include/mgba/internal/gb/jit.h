/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_JIT_H
#define GB_JIT_H

#include <mgba-util/common.h>

CXX_GUARD_START

struct GB;
struct GBJit;
struct SM83Core;

// Code running from cartridge ROM, compiled to x86-64; only blocks no hardware event can interrupt run natively
struct GBJit* GBJitCreate(struct GB* gb);
void GBJitDestroy(struct GBJit* jit);
void GBJitFlush(struct GBJit* jit);
bool GBJitRun(struct SM83Core* cpu);

CXX_GUARD_END

#endif
