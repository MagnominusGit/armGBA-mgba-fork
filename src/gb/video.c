/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/video.h>

#include <mgba/core/sync.h>
#include <mgba/core/cache-set.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/io.h>
#include <mgba/internal/gb/renderers/cache-set.h>
#include <mgba/internal/gb/serialize.h>
#include <mgba/internal/sm83/sm83.h>

#include <mgba-util/memory.h>

mLOG_DEFINE_CATEGORY(GB_VIDEO, "GB Video", "gb.video");

static void GBVideoDummyRendererInit(struct GBVideoRenderer* renderer, enum GBModel model, bool borders);
static void GBVideoDummyRendererDeinit(struct GBVideoRenderer* renderer);
static uint8_t GBVideoDummyRendererWriteVideoRegister(struct GBVideoRenderer* renderer, uint16_t address, uint8_t value);
static void GBVideoDummyRendererWriteSGBPacket(struct GBVideoRenderer* renderer, uint8_t* data);
static void GBVideoDummyRendererWritePalette(struct GBVideoRenderer* renderer, int index, uint16_t value);
static void GBVideoDummyRendererWriteVRAM(struct GBVideoRenderer* renderer, uint16_t address);
static void GBVideoDummyRendererWriteOAM(struct GBVideoRenderer* renderer, uint16_t oam);
static void GBVideoDummyRendererDrawRange(struct GBVideoRenderer* renderer, int startX, int endX, int y);
static void GBVideoDummyRendererFinishScanline(struct GBVideoRenderer* renderer, int y);
static void GBVideoDummyRendererFinishFrame(struct GBVideoRenderer* renderer);
static void GBVideoDummyRendererEnableSGBBorder(struct GBVideoRenderer* renderer, bool enable);
static void GBVideoDummyRendererGetPixels(struct GBVideoRenderer* renderer, size_t* stride, const void** pixels);
static void GBVideoDummyRendererPutPixels(struct GBVideoRenderer* renderer, size_t stride, const void* pixels);

static void _cleanOAM(struct GBVideo* video, int y);

static void _ppuStep(struct mTiming* timing, void* context, uint32_t cyclesLate);
static void _ppuIRQ(struct mTiming* timing, void* context, uint32_t cyclesLate);
static void _updateFrameCount(struct mTiming* timing, void* context, uint32_t cyclesLate);

static const uint16_t _defaultBorderPalette[16] = {
	0x0000,
	0x7FDE,
	0x7FFF,
	0x739A,
	0x2929,
	0x24E7,
	0x1CC6,
	0x0400,
	0x514A,
	0x3907,
	0x28C5,
};

static const uint8_t _defaultBorderTilemap[] = {
	0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10,
	0x12, 0x10, 0x14, 0x10, 0x16, 0x10, 0x17, 0x10, 0x17, 0x10, 0x17, 0x10, 0x17, 0x10, 0x17, 0x10,
	0x17, 0x10, 0x17, 0x10, 0x17, 0x10, 0x17, 0x10, 0x17, 0x10, 0x16, 0x50, 0x14, 0x10, 0x12, 0x50,
	0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10,
	0x02, 0x10, 0x06, 0x10, 0x0A, 0x10, 0x0D, 0x10, 0x0E, 0x10, 0x0F, 0x10, 0x10, 0x10, 0x11, 0x10,
	0x13, 0x10, 0x15, 0x10, 0x15, 0x10, 0x18, 0x10, 0x1B, 0x10, 0x1B, 0x10, 0x1B, 0x10, 0x1B, 0x10,
	0x1B, 0x10, 0x1B, 0x10, 0x1B, 0x10, 0x1B, 0x10, 0x18, 0x50, 0x15, 0x10, 0x15, 0x10, 0x13, 0x10,
	0x11, 0x10, 0x10, 0x50, 0x0F, 0x50, 0x0E, 0x50, 0x0D, 0x50, 0x0A, 0x50, 0x06, 0x50, 0x02, 0x50,
	0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10,
	0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10,
	0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10,
	0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10,
	0x04, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10,
	0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10,
	0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10,
	0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x07, 0x10, 0x04, 0x50,
	0x05, 0x10, 0x08, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10,
	0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10,
	0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10,
	0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x0B, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
	0x00, 0x10, 0x00, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x05, 0x10, 0x09, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10,
	0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10,
	0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10,
	0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x0C, 0x10, 0x05, 0x50,
	0x04, 0x90, 0x07, 0x90, 0x07, 0x90, 0x07, 0x90, 0x07, 0x90, 0x07, 0x90, 0x07, 0x90, 0x07, 0x90,
	0x07, 0x90, 0x07, 0x90, 0x07, 0x90, 0x07, 0x90, 0x07, 0x90, 0x1F, 0x10, 0x23, 0x10, 0x23, 0x10,
	0x23, 0x10, 0x23, 0x10, 0x23, 0x10, 0x23, 0x10, 0x34, 0x10, 0x07, 0x90, 0x07, 0x90, 0x07, 0x90,
	0x07, 0x90, 0x07, 0x90, 0x07, 0x90, 0x07, 0x90, 0x07, 0x90, 0x07, 0x90, 0x07, 0x90, 0x04, 0xD0,
	0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10,
	0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x19, 0x10, 0x1C, 0x10, 0x20, 0x10, 0x24, 0x10, 0x26, 0x10,
	0x29, 0x10, 0x2B, 0x10, 0x2E, 0x10, 0x31, 0x10, 0x35, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10,
	0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10,
	0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10,
	0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x1A, 0x10, 0x1D, 0x10, 0x21, 0x10, 0x25, 0x10, 0x27, 0x10,
	0x2A, 0x10, 0x2C, 0x10, 0x2F, 0x10, 0x32, 0x10, 0x1A, 0x50, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10,
	0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10,
	0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10,
	0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x19, 0x90, 0x1E, 0x10, 0x22, 0x10, 0x1C, 0x90, 0x28, 0x10,
	0x1C, 0x90, 0x2D, 0x10, 0x30, 0x10, 0x33, 0x10, 0x19, 0xD0, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10,
	0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10, 0x03, 0x10,
};

static const uint8_t _defaultBorderChardata[] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x03, 0xFC, 0x00, 0x00, 0xF0, 0x0F, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x00, 0xFF, 0x00, 0xFF, 0x03, 0xFC, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x01, 0xFF, 0x03, 0xFF,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x03, 0xFF, 0x03, 0xFF, 0x03, 0xFF, 0x03, 0xFF, 0x03, 0xFF, 0x03, 0xFF, 0x03, 0xFF, 0x03, 0xFF,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x00, 0x00, 0x3F, 0x00, 0xF0, 0x00, 0x0F, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x00, 0xFF, 0x00, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0xFF, 0xF0, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xF0, 0xFF, 0xF0, 0xFF, 0xF0, 0xFF, 0xF0, 0xFF, 0xF0, 0xFF, 0xF0, 0xFF, 0xF0, 0xFF, 0xF0, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xC0, 0x3F, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x00, 0xFF, 0x00, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xE0, 0x1F, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x00, 0xFF, 0x7F, 0x80, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x1F, 0x00, 0x7E, 0x80, 0x01, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x00, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0xFC, 0x03, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x00, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x00, 0xFF, 0x00, 0x00, 0xF8, 0x07, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x07, 0xF8, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
	0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00,
	0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x3F,
	0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,
	0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x00, 0x00, 0xC0, 0x3F, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x07, 0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0xFE,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFE, 0x00,
	0x0F, 0xFE, 0x0F, 0xFE, 0x0F, 0xFE, 0x0F, 0xFE, 0x0F, 0xFE, 0x0F, 0xFE, 0x0F, 0xFE, 0x0F, 0xFE,
	0xFE, 0x00, 0xFE, 0x00, 0xFE, 0x00, 0xFE, 0x00, 0xFE, 0x00, 0xFE, 0x00, 0xFE, 0x00, 0xFE, 0x00,
	0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00,
	0xFF, 0x00, 0xFF, 0x19, 0xFF, 0x19, 0xFF, 0x19, 0xFF, 0x19, 0xFF, 0x19, 0xFF, 0x19, 0xFF, 0x19,
	0x00, 0x00, 0x19, 0x00, 0x19, 0x00, 0x19, 0x00, 0x19, 0x00, 0x19, 0x00, 0x19, 0x00, 0x19, 0x00,
	0xFF, 0x19, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x19, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x0F, 0xFF,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0x1F, 0xFF, 0x1F, 0xFF, 0x1F, 0xFC, 0x1F, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0x1C,
	0xFF, 0x00, 0xFF, 0x00, 0xFC, 0x00, 0xFC, 0x00, 0xFC, 0x00, 0xFC, 0x00, 0xFC, 0x00, 0x1C, 0x00,
	0xFF, 0x1C, 0xFF, 0x9C, 0xFF, 0x9C, 0xFF, 0x9C, 0xFF, 0x9C, 0xFF, 0x9C, 0xFF, 0x9C, 0xFF, 0x9C,
	0x1C, 0x00, 0x9C, 0x00, 0x9C, 0x00, 0x9C, 0x00, 0x9C, 0x00, 0x9C, 0x00, 0x9C, 0x00, 0x9C, 0x00,
	0xFF, 0x9C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x9C, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xFF, 0xFF,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F,
	0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x0F, 0x00, 0x0F, 0x00,
	0xFF, 0x0E, 0xFF, 0x0E, 0xFF, 0x0E, 0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x00, 0xFF, 0x00,
	0x0E, 0x00, 0x0E, 0x00, 0x0E, 0x00, 0x0F, 0x00, 0x0F, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0xFF, 0x07, 0xFF, 0x07, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0x00, 0xFF, 0x00, 0x07, 0x00, 0x07, 0x00, 0x07, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0xFF, 0x07, 0xFF, 0x07, 0xFF, 0x07, 0xFF, 0xC7, 0xFF, 0xC7, 0xFF, 0xC7, 0xFF, 0x07, 0xFF, 0x07,
	0x07, 0x00, 0x07, 0x00, 0x07, 0x00, 0xC7, 0x00, 0xC7, 0x00, 0xC7, 0x00, 0x07, 0x00, 0x07, 0x00,
	0xFF, 0x07, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x07, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x03, 0xFF, 0x03, 0xFF, 0x03,
	0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x03, 0x00,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x03, 0xFF, 0x03, 0xFF, 0x03, 0xFF, 0x00, 0xFF, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0xFF, 0x01, 0xFF, 0x01, 0xFF, 0xF1, 0xFF, 0xF1, 0xFF, 0xF1,
	0xFF, 0x00, 0xFF, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0xF1, 0x00, 0xF1, 0x00, 0xF1, 0x00,
	0xFF, 0x03, 0xFF, 0x07, 0xFF, 0x03, 0xFF, 0xF1, 0xFF, 0xF1, 0xFF, 0xF1, 0xFF, 0x01, 0xFF, 0x01,
	0x03, 0x00, 0x07, 0x00, 0x03, 0x00, 0xF1, 0x00, 0xF1, 0x00, 0xF1, 0x00, 0x01, 0x00, 0x01, 0x00,
	0xFF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0x01, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0,
	0xFF, 0x00, 0xFF, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
	0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF, 0xC0,
	0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00, 0xC0, 0x00,
	0xFF, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0xC0, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC,
	0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0x00, 0xFC, 0x00, 0xFC, 0x00,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC, 0xFF, 0xFC,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0x00, 0xFC, 0x00, 0xFC, 0x00, 0xFC, 0x00, 0xFC, 0x00,
	0xFF, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
	0xFC, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xE0, 0xFF,
	0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
	0xF0, 0xFF, 0xF0, 0xFF, 0xF0, 0x7F, 0xF0, 0x7F, 0xF0, 0x7F, 0xF0, 0x7F, 0xF0, 0x7F, 0xF0, 0x7F,
	0xFF, 0x00, 0xFF, 0x00, 0x7F, 0x00, 0x7F, 0x00, 0x7F, 0x00, 0x7F, 0x00, 0x7F, 0x00, 0x7F, 0x00,
};

void GBVideoInit(struct GBVideo* video) {
	video->renderer = NULL;
	video->vram = anonymousMemoryMap(GB_SIZE_VRAM);
	video->frameskip = 0;

	video->modeEvent.context = video;
	video->modeEvent.name = "GB Video Mode";
	video->modeEvent.callback = _ppuStep;
	video->modeEvent.priority = 8;
	video->frameEvent.context = video;
	video->frameEvent.name = "GB Video Frame";
	video->frameEvent.callback = _updateFrameCount;
	video->frameEvent.priority = 9;
	video->irqEvent.context = video;
	video->irqEvent.name = "GB Video IRQ";
	video->irqEvent.callback = _ppuIRQ;
	video->irqEvent.priority = 7;

	video->dmgPalette[0] = 0x7FFF;
	video->dmgPalette[1] = 0x56B5;
	video->dmgPalette[2] = 0x294A;
	video->dmgPalette[3] = 0x0000;
	video->dmgPalette[4] = 0x7FFF;
	video->dmgPalette[5] = 0x56B5;
	video->dmgPalette[6] = 0x294A;
	video->dmgPalette[7] = 0x0000;
	video->dmgPalette[8] = 0x7FFF;
	video->dmgPalette[9] = 0x56B5;
	video->dmgPalette[10] = 0x294A;
	video->dmgPalette[11] = 0x0000;

	video->sgbBorders = true;
}

void GBVideoReset(struct GBVideo* video) {
	video->ly = 0;
	video->x = 0;
	video->mode = 0;
	video->stat = 0;
	video->ppuState = 0;
	video->lineCycles = 0;
	video->modeForInterrupt = -1;
	video->lyForComparison = 0;
	video->statLine = false;
	video->lycLine = false;
	video->wyTriggered = false;
	video->oamReadBlocked = false;
	video->oamWriteBlocked = false;
	video->vramReadBlocked = false;
	video->vramWriteBlocked = false;
	video->paletteBlocked = false;
	video->lineObjCount = 0;

	video->frameCounter = 0;
	video->frameskipCounter = 0;

	GBVideoSwitchBank(video, 0);
	memset(video->vram, 0, GB_SIZE_VRAM);
	video->renderer->vram = video->vram;
	memset(&video->oam, 0, sizeof(video->oam));
	video->renderer->oam = &video->oam;
	memset(&video->palette, 0, sizeof(video->palette));

	if (video->p->model & GB_MODEL_SGB) {
		if (video->renderer->sgbCharRam) {
			memset(video->renderer->sgbCharRam, 0, SGB_SIZE_CHAR_RAM);
		} else {
			video->renderer->sgbCharRam = anonymousMemoryMap(SGB_SIZE_CHAR_RAM);
		}
		memcpy(video->renderer->sgbCharRam, _defaultBorderChardata, sizeof(_defaultBorderChardata));

		if (video->renderer->sgbMapRam) {
			memset(video->renderer->sgbMapRam, 0, SGB_SIZE_MAP_RAM);
		} else {
			video->renderer->sgbMapRam = anonymousMemoryMap(SGB_SIZE_MAP_RAM);
		}
		memcpy(video->renderer->sgbMapRam, _defaultBorderTilemap, sizeof(_defaultBorderTilemap));
		int i;
		for (i = 0; i < 16; ++i) {
			STORE_16LE(_defaultBorderPalette[i], 0x800 + i * 2, video->renderer->sgbMapRam);
		}

		if (video->renderer->sgbPalRam) {
			memset(video->renderer->sgbPalRam, 0, SGB_SIZE_PAL_RAM);
		} else {
			video->renderer->sgbPalRam = anonymousMemoryMap(SGB_SIZE_PAL_RAM);
		}

		if (video->renderer->sgbAttributeFiles) {
			memset(video->renderer->sgbAttributeFiles, 0, SGB_SIZE_ATF_RAM);
		} else {
			video->renderer->sgbAttributeFiles = anonymousMemoryMap(SGB_SIZE_ATF_RAM);
		}
		if (!video->renderer->sgbAttributes) {
			video->renderer->sgbAttributes = malloc(90 * 45);
		}
		memset(video->renderer->sgbAttributes, 0, 90 * 45);
		video->sgbCommandHeader = 0;
		video->sgbBufferIndex = 0;
	} else {
		if (video->renderer->sgbCharRam) {
			mappedMemoryFree(video->renderer->sgbCharRam, SGB_SIZE_CHAR_RAM);
			video->renderer->sgbCharRam = NULL;
		}
		if (video->renderer->sgbMapRam) {
			mappedMemoryFree(video->renderer->sgbMapRam, SGB_SIZE_MAP_RAM);
			video->renderer->sgbMapRam = NULL;
		}
		if (video->renderer->sgbPalRam) {
			mappedMemoryFree(video->renderer->sgbPalRam, SGB_SIZE_PAL_RAM);
			video->renderer->sgbPalRam = NULL;
		}
		if (video->renderer->sgbAttributeFiles) {
			mappedMemoryFree(video->renderer->sgbAttributeFiles, SGB_SIZE_ATF_RAM);
			video->renderer->sgbAttributeFiles = NULL;
		}
		if (video->renderer->sgbAttributes) {
			free(video->renderer->sgbAttributes);
			video->renderer->sgbAttributes = NULL;
		}
	}

	video->palette[0] = video->dmgPalette[0];
	video->palette[1] = video->dmgPalette[1];
	video->palette[2] = video->dmgPalette[2];
	video->palette[3] = video->dmgPalette[3];
	video->palette[8 * 4 + 0] = video->dmgPalette[4];
	video->palette[8 * 4 + 1] = video->dmgPalette[5];
	video->palette[8 * 4 + 2] = video->dmgPalette[6];
	video->palette[8 * 4 + 3] = video->dmgPalette[7];
	video->palette[9 * 4 + 0] = video->dmgPalette[8];
	video->palette[9 * 4 + 1] = video->dmgPalette[9];
	video->palette[9 * 4 + 2] = video->dmgPalette[10];
	video->palette[9 * 4 + 3] = video->dmgPalette[11];

	if (!video->renderer) {
		mLOG(GB_VIDEO, FATAL, "No renderer associated");
		return;
	}

	video->renderer->deinit(video->renderer);
	video->renderer->init(video->renderer, video->p->model, video->sgbBorders);

	video->renderer->writePalette(video->renderer, 0, video->palette[0]);
	video->renderer->writePalette(video->renderer, 1, video->palette[1]);
	video->renderer->writePalette(video->renderer, 2, video->palette[2]);
	video->renderer->writePalette(video->renderer, 3, video->palette[3]);
	video->renderer->writePalette(video->renderer, 8 * 4 + 0, video->palette[8 * 4 + 0]);
	video->renderer->writePalette(video->renderer, 8 * 4 + 1, video->palette[8 * 4 + 1]);
	video->renderer->writePalette(video->renderer, 8 * 4 + 2, video->palette[8 * 4 + 2]);
	video->renderer->writePalette(video->renderer, 8 * 4 + 3, video->palette[8 * 4 + 3]);
	video->renderer->writePalette(video->renderer, 9 * 4 + 0, video->palette[9 * 4 + 0]);
	video->renderer->writePalette(video->renderer, 9 * 4 + 1, video->palette[9 * 4 + 1]);
	video->renderer->writePalette(video->renderer, 9 * 4 + 2, video->palette[9 * 4 + 2]);
	video->renderer->writePalette(video->renderer, 9 * 4 + 3, video->palette[9 * 4 + 3]);

	mTimingSchedule(&video->p->timing, &video->frameEvent, GB_VIDEO_TOTAL_LENGTH << 1);
}

void GBVideoDeinit(struct GBVideo* video) {
	video->renderer->deinit(video->renderer);
	mappedMemoryFree(video->vram, GB_SIZE_VRAM);
	if (video->renderer->sgbCharRam) {
		mappedMemoryFree(video->renderer->sgbCharRam, SGB_SIZE_CHAR_RAM);
		video->renderer->sgbCharRam = NULL;
	}
	if (video->renderer->sgbMapRam) {
		mappedMemoryFree(video->renderer->sgbMapRam, SGB_SIZE_MAP_RAM);
		video->renderer->sgbMapRam = NULL;
	}
	if (video->renderer->sgbPalRam) {
		mappedMemoryFree(video->renderer->sgbPalRam, SGB_SIZE_PAL_RAM);
		video->renderer->sgbPalRam = NULL;
	}
	if (video->renderer->sgbAttributeFiles) {
		mappedMemoryFree(video->renderer->sgbAttributeFiles, SGB_SIZE_ATF_RAM);
		video->renderer->sgbAttributeFiles = NULL;
	}
	if (video->renderer->sgbAttributes) {
		free(video->renderer->sgbAttributes);
		video->renderer->sgbAttributes = NULL;
	}
}

void GBVideoDummyRendererCreate(struct GBVideoRenderer* renderer) {
	static const struct GBVideoRenderer dummyRenderer = {
		.init = GBVideoDummyRendererInit,
		.deinit = GBVideoDummyRendererDeinit,
		.writeVideoRegister = GBVideoDummyRendererWriteVideoRegister,
		.writeSGBPacket = GBVideoDummyRendererWriteSGBPacket,
		.writeVRAM = GBVideoDummyRendererWriteVRAM,
		.writeOAM = GBVideoDummyRendererWriteOAM,
		.writePalette = GBVideoDummyRendererWritePalette,
		.drawRange = GBVideoDummyRendererDrawRange,
		.finishScanline = GBVideoDummyRendererFinishScanline,
		.finishFrame = GBVideoDummyRendererFinishFrame,
		.enableSGBBorder = GBVideoDummyRendererEnableSGBBorder,
		.getPixels = GBVideoDummyRendererGetPixels,
		.putPixels = GBVideoDummyRendererPutPixels,
	};
	memcpy(renderer, &dummyRenderer, sizeof(*renderer));
}

void GBVideoAssociateRenderer(struct GBVideo* video, struct GBVideoRenderer* renderer) {
	if (video->renderer) {
		video->renderer->deinit(video->renderer);
		renderer->cache = video->renderer->cache;
		renderer->sgbRenderMode = video->renderer->sgbRenderMode;
		renderer->sgbCharRam = video->renderer->sgbCharRam;
		renderer->sgbMapRam = video->renderer->sgbMapRam;
		renderer->sgbPalRam = video->renderer->sgbPalRam;
		renderer->sgbAttributeFiles = video->renderer->sgbAttributeFiles;
		renderer->sgbAttributes = video->renderer->sgbAttributes;
	} else {
		renderer->cache = NULL;
		renderer->sgbRenderMode = 0;
		renderer->sgbCharRam = NULL;
		renderer->sgbMapRam = NULL;
		renderer->sgbPalRam = NULL;
		renderer->sgbAttributeFiles = NULL;
		renderer->sgbAttributes = NULL;
	}
	video->renderer = renderer;
	renderer->vram = video->vram;
	video->renderer->init(video->renderer, video->p->model, video->sgbBorders);
}

// The PPU follows SameBoy's model of the hardware, a T-cycle at a time where it matters: each state is
// a point in a line where something visible changes, and the line's cycles count from its start
enum {
	PPU_OFF = 0,
	PPU_LCDON_OAM,
	PPU_LCDON_MODE3,
	PPU_LCDON_PALETTE,
	PPU_LCDON_VRAM,
	PPU_LINE_START,
	PPU_LINE_OAM,
	PPU_LINE_LY,
	PPU_LINE_MODE2,
	PPU_LINE_VRAM,
	PPU_LINE_MODE3,
	PPU_LINE_PALETTE,
	PPU_MODE3_START,
	PPU_MODE3_END,
	PPU_HBLANK_STAT,
	PPU_HBLANK_PALETTE,
	PPU_HBLANK_UNBLOCK,
	PPU_LINE_WY,
	PPU_LINE_NEXT,
	PPU_VBLANK_START,
	PPU_VBLANK_LY,
	PPU_VBLANK_LYC,
	PPU_VBLANK_MODE1,
	PPU_VBLANK_NEXT,
	PPU_LINE153_START,
	PPU_LINE153_LY,
	PPU_LINE153_LY0,
	PPU_LINE153_LYC,
	PPU_LINE153_LYC0,
	PPU_LINE153_HOLD,
	PPU_LINE153_NEXT,
};

static bool _isCGB(const struct GBVideo* video) {
	return video->p->model >= GB_MODEL_CGB;
}

static void _setMode(struct GBVideo* video, int mode) {
	video->mode = mode;
	video->stat = GBRegisterSTATSetMode(video->stat, mode);
	video->p->memory.io[GB_REG_STAT] = video->stat;
}

static void _blockAll(struct GBVideo* video, bool block) {
	video->oamReadBlocked = block;
	video->oamWriteBlocked = block;
	video->vramReadBlocked = block;
	video->vramWriteBlocked = block;
	video->paletteBlocked = block;
}

// The CPU decides on interrupts as its last access before an instruction happens, a little before the
// interpreter looks at them, so PPU interrupts reach it that much later
static void _raiseIRQs(struct GBVideo* video) {
	if (!mTimingIsScheduled(&video->p->timing, &video->irqEvent)) {
		// Measured against the interrupt timing tests: 1.5 T-cycles running, 2 T-cycles in HALT
		int t = video->p->cpu->tMultiplier;
		mTimingSchedule(&video->p->timing, &video->irqEvent, video->p->cpu->halted ? 2 * t : 3 * t / 2);
	}
}

static void _ppuIRQ(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(timing);
	UNUSED(cyclesLate);
	struct GBVideo* video = context;
	GBUpdateIRQs(video->p);
}

// The STAT interrupt is the OR of its enabled sources, and fires on a rising edge
static void _statUpdate(struct GBVideo* video) {
	struct GB* gb = video->p;
	if (!GBRegisterLCDCIsEnable(gb->memory.io[GB_REG_LCDC])) {
		return;
	}
	if (gb->memory.dmaRemaining && !gb->memory.dmaStarting && GBRegisterSTATGetMode(video->stat) == 2) {
		video->stat = GBRegisterSTATSetMode(video->stat, 0);
	}
	bool previous = video->statLine;
	if (video->lyForComparison != -1 || !_isCGB(video)) {
		if (video->lyForComparison == gb->memory.io[GB_REG_LYC]) {
			video->lycLine = true;
			video->stat = GBRegisterSTATFillLYC(video->stat);
		} else {
			if (video->lyForComparison != -1) {
				video->lycLine = false;
			}
			video->stat = GBRegisterSTATClearLYC(video->stat);
		}
	}
	bool line;
	switch (video->modeForInterrupt) {
	case 0:
		line = GBRegisterSTATIsHblankIRQ(video->stat);
		break;
	case 1:
		line = GBRegisterSTATIsVblankIRQ(video->stat);
		break;
	case 2:
		line = GBRegisterSTATIsOAMIRQ(video->stat);
		break;
	default:
		line = false;
		break;
	}
	if (GBRegisterSTATIsLYCIRQ(video->stat) && video->lycLine) {
		line = true;
	}
	video->statLine = line;
	gb->memory.io[GB_REG_STAT] = video->stat;
	if (line && !previous) {
		gb->memory.io[GB_REG_IF] |= (1 << GB_IRQ_LCDSTAT);
		_raiseIRQs(video);
	}
}

// Objects the OAM scan picks for a line, largest X first, as the fetcher meets them from the end
static void _scanObjects(struct GBVideo* video) {
	int height = GBRegisterLCDCIsObjSize(video->p->memory.io[GB_REG_LCDC]) ? 16 : 8;
	int n = 0;
	int i;
	for (i = 0; i < GB_VIDEO_MAX_OBJ && n < GB_VIDEO_MAX_LINE_OBJ; ++i) {
		int y = video->oam.obj[i].y - 16;
		if (y > video->ly || y + height <= video->ly) {
			continue;
		}
		uint8_t x = video->oam.obj[i].x;
		int j;
		for (j = 0; j < n; ++j) {
			if (video->lineObjX[j] <= x) {
				break;
			}
		}
		memmove(&video->lineObjX[j + 1], &video->lineObjX[j], n - j);
		video->lineObjX[j] = x;
		++n;
	}
	video->lineObjCount = n;
}

enum {
	FETCH_SLEEP,
	FETCH_TILE,
	FETCH_LOW,
	FETCH_HIGH,
	FETCH_PUSH,
};

// The background fetcher's steps, which only matter here for when it has a row of 8 pixels ready
static void _advanceFetcher(int* state, int* fifo, bool cgb, bool windowQuirk, uint8_t wx, uint8_t position) {
	static const uint8_t steps[8] = { FETCH_SLEEP, FETCH_TILE, FETCH_SLEEP, FETCH_LOW, FETCH_SLEEP, FETCH_HIGH, FETCH_PUSH, FETCH_PUSH };
	switch (steps[*state & 7]) {
	case FETCH_PUSH:
	case FETCH_HIGH:
		if (*state < 7) {
			++*state;
		}
		if (*fifo > 0) {
			break;
		}
		if (windowQuirk && !cgb) {
			uint8_t logical = position + 7;
			if (logical > 167) {
				logical = 0;
			}
			if (wx == logical) {
				*fifo = 1;
				break;
			}
		}
		*fifo = 8;
		*state = 0;
		break;
	default:
		++*state;
		break;
	}
}

// Mode 3 runs until 160 pixels are out: 8 dropped at the start, SCX & 7 dropped for scrolling, and
// stalls for each object fetch and for the window
static int _mode3Length(struct GBVideo* video) {
	struct GB* gb = video->p;
	bool cgb = _isCGB(video);
	uint8_t lcdc = gb->memory.io[GB_REG_LCDC];
	uint8_t scx = gb->memory.io[GB_REG_SCX];
	uint8_t wx = gb->memory.io[GB_REG_WX];
	bool window = video->wyTriggered && (lcdc & 0x20);
	int n = video->lineObjCount;
	if (!n && !window) {
		return 167 + (scx & 7);
	}
	static const int8_t wx0Position[8] = { -7, -1, -2, -3, -4, -5, -6, -6 };
	bool objects = (lcdc & 2) || cgb;
	bool windowQuirk = video->wyTriggered && !(lcdc & 0x20);
	uint8_t position = -16;
	int fetcher = 0;
	int fifo = 8;
	bool wxTriggered = false;
	bool windowFetching = false;
	int cycles = 0;
	while (true) {
		if (!wxTriggered && window) {
			bool activate = false;
			if (wx == 0) {
				activate = position == (uint8_t) wx0Position[scx & 7];
			} else if (wx < 166 + cgb) {
				activate = wx == (uint8_t) (position + 7) || wx == (uint8_t) (position + 6);
			}
			if (activate) {
				if (wx == 0 && (scx & 7)) {
					++cycles;
				}
				wxTriggered = true;
				fifo = 0;
				fetcher = 0;
				windowFetching = true;
			}
		}
		if (!cgb && wxTriggered && !windowFetching && fetcher == 0 && wx == (uint8_t) (position + 7) && fifo == 8) {
			++fifo;
		}

		uint8_t match = position + 8;
		if (match > (uint8_t) -16) {
			match = 0;
		}
		while (n && video->lineObjX[n - 1] < match) {
			--n;
		}
		while (n && objects && video->lineObjX[n - 1] == match) {
			while (fetcher < 5 || !fifo) {
				_advanceFetcher(&fetcher, &fifo, cgb, windowQuirk, wx, position);
				++cycles;
			}
			_advanceFetcher(&fetcher, &fifo, cgb, windowQuirk, wx, position);
			_advanceFetcher(&fetcher, &fifo, cgb, windowQuirk, wx, position);
			cycles += 6;
			--n;
		}

		if (!(n && objects && video->lineObjX[n - 1] == 0) && !(wxTriggered && !fifo)) {
			--fifo;
			bool drawn = true;
			if ((uint8_t) (position + 16) < 8) {
				if ((position & 7) == (scx & 7)) {
					position = -8;
				} else if (position == (uint8_t) -9) {
					position = -16;
					drawn = false;
				}
			}
			if (drawn) {
				if (position < 160) {
					windowFetching = false;
				}
				++position;
			}
		}
		_advanceFetcher(&fetcher, &fifo, cgb, windowQuirk, wx, position);
		if (position == 160) {
			break;
		}
		++cycles;
	}
	return cycles;
}

static void _ppuSchedule(struct GBVideo* video, int state, int dots, int32_t cyclesLate) {
	video->ppuState = state;
	mTimingSchedule(&video->p->timing, &video->modeEvent, (dots << 1) - cyclesLate);
}

static void _startMode3(struct GBVideo* video, int32_t cyclesLate) {
	struct GB* gb = video->p;
	video->x = -(gb->memory.io[GB_REG_SCX] & 7);
	video->dotClock = mTimingCurrentTime(&gb->timing) - cyclesLate + 10 - (video->x << 1);
	_setMode(video, 3);
	video->modeForInterrupt = 3;
}

static void _startLine(struct GBVideo* video, int32_t cyclesLate) {
	video->oamWriteBlocked = _isCGB(video) && !video->p->doubleSpeed;
	video->lineCycles = 0;
	_ppuSchedule(video, PPU_LINE_OAM, 2, cyclesLate);
}

static void _startVBlankLine(struct GBVideo* video, int32_t cyclesLate) {
	video->lyForComparison = -1;
	_statUpdate(video);
	_ppuSchedule(video, PPU_VBLANK_LY, 2, cyclesLate);
}

static void _ppuStep(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(timing);
	struct GBVideo* video = context;
	struct GB* gb = video->p;
	bool cgb = _isCGB(video);
	switch (video->ppuState) {
	case PPU_OFF:
		return;

	// The first line after the LCD turns on: no OAM scan, and HBlank shorter by 8
	case PPU_LCDON_OAM:
		video->oamWriteBlocked = true;
		video->lineCycles += 2;
		_statUpdate(video);
		_ppuSchedule(video, PPU_LCDON_MODE3, 2, cyclesLate);
		return;
	case PPU_LCDON_MODE3:
		video->lineCycles += 8;
		video->lineObjCount = 0;
		_startMode3(video, cyclesLate);
		video->oamReadBlocked = true;
		video->oamWriteBlocked = true;
		video->vramReadBlocked = cgb ? gb->doubleSpeed : true;
		video->vramWriteBlocked = video->vramReadBlocked;
		video->lineCycles += 2;
		_ppuSchedule(video, PPU_LCDON_PALETTE, 2, cyclesLate);
		return;
	case PPU_LCDON_PALETTE:
		video->paletteBlocked = true;
		video->lineCycles += 3;
		_ppuSchedule(video, PPU_LCDON_VRAM, 3, cyclesLate);
		return;
	case PPU_LCDON_VRAM:
		video->vramReadBlocked = true;
		video->vramWriteBlocked = true;
		// Fall through
	case PPU_MODE3_START: {
		if ((gb->memory.io[GB_REG_LCDC] & 0x20) && gb->memory.io[GB_REG_WY] == video->ly) {
			video->wyTriggered = true;
		}
		int length = _mode3Length(video);
		video->lineCycles += length;
		_ppuSchedule(video, PPU_MODE3_END, length, cyclesLate);
		return;
	}

	// Lines 0 to 143
	case PPU_LINE_OAM:
		video->oamWriteBlocked = cgb;
		_ppuSchedule(video, PPU_LINE_LY, 1, cyclesLate);
		return;
	case PPU_LINE_LY:
		gb->memory.io[GB_REG_LY] = video->ly;
		video->oamReadBlocked = true;
		video->lyForComparison = video->ly ? -1 : 0;
		if (video->ly) {
			// The OAM interrupt comes a T-cycle before STAT shows mode 2
			video->modeForInterrupt = 2;
			_setMode(video, 0);
		} else if (!cgb) {
			_setMode(video, 0);
		}
		_statUpdate(video);
		_ppuSchedule(video, PPU_LINE_MODE2, 1, cyclesLate);
		return;
	case PPU_LINE_MODE2:
		_setMode(video, 2);
		video->modeForInterrupt = 2;
		video->oamWriteBlocked = true;
		video->lyForComparison = video->ly;
		_statUpdate(video);
		video->modeForInterrupt = -1;
		_statUpdate(video);
		_ppuSchedule(video, PPU_LINE_VRAM, 76, cyclesLate);
		return;
	case PPU_LINE_VRAM:
		video->vramReadBlocked = !cgb;
		video->vramWriteBlocked = false;
		video->paletteBlocked = false;
		video->oamWriteBlocked = cgb;
		_ppuSchedule(video, PPU_LINE_MODE3, 4, cyclesLate);
		return;
	case PPU_LINE_MODE3:
		video->lineCycles = GB_VIDEO_MODE_2_LENGTH + 4;
		_scanObjects(video);
		_cleanOAM(video, video->ly);
		_startMode3(video, cyclesLate);
		video->vramReadBlocked = true;
		video->vramWriteBlocked = true;
		video->paletteBlocked = false;
		video->oamWriteBlocked = true;
		video->oamReadBlocked = true;
		_statUpdate(video);
		video->lineCycles += 3;
		_ppuSchedule(video, PPU_LINE_PALETTE, 3, cyclesLate);
		return;
	case PPU_LINE_PALETTE:
		video->paletteBlocked = true;
		video->lineCycles += 2;
		_ppuSchedule(video, PPU_MODE3_START, 2, cyclesLate);
		return;
	case PPU_MODE3_END:
		GBVideoProcessDots(video, cyclesLate);
		if (!gb->doubleSpeed) {
			_setMode(video, 0);
			video->modeForInterrupt = 0;
			_blockAll(video, false);
		}
		video->lineCycles += 1;
		_ppuSchedule(video, PPU_HBLANK_STAT, 1, cyclesLate);
		return;
	case PPU_HBLANK_STAT:
		_setMode(video, 0);
		video->modeForInterrupt = 0;
		_blockAll(video, false);
		_statUpdate(video);
		video->lineCycles += 2;
		_ppuSchedule(video, PPU_HBLANK_PALETTE, 2, cyclesLate);
		return;
	case PPU_HBLANK_PALETTE:
		video->paletteBlocked = !gb->doubleSpeed;
		if (gb->memory.isHdma && gb->memory.io[GB_REG_HDMA5] != 0xFF && !gb->cpu->halted) {
			gb->memory.hdmaRemaining = 0x10;
			gb->cpuBlocked = true;
			mTimingDeschedule(&gb->timing, &gb->memory.hdmaEvent);
			mTimingSchedule(&gb->timing, &gb->memory.hdmaEvent, 0);
		}
		video->lineCycles += 2;
		_ppuSchedule(video, PPU_HBLANK_UNBLOCK, 2, cyclesLate);
		return;
	case PPU_HBLANK_UNBLOCK: {
		video->paletteBlocked = false;
		int remaining = GB_VIDEO_HORIZONTAL_LENGTH - 2 - video->lineCycles;
		if (remaining < 0) {
			remaining = 0;
		}
		_ppuSchedule(video, PPU_LINE_WY, remaining, cyclesLate);
		return;
	}
	case PPU_LINE_WY:
		if ((gb->memory.io[GB_REG_LCDC] & 0x20) && gb->memory.io[GB_REG_WY] == video->ly) {
			video->wyTriggered = true;
		}
		_ppuSchedule(video, PPU_LINE_NEXT, 2, cyclesLate);
		return;
	case PPU_LINE_NEXT:
		if (video->ly != GB_VIDEO_VERTICAL_PIXELS - 1) {
			video->modeForInterrupt = 2;
		}
		if (video->frameskipCounter <= 0) {
			video->renderer->finishScanline(video->renderer, video->ly);
		}
		++video->ly;
		if (video->ly < GB_VIDEO_VERTICAL_PIXELS) {
			_startLine(video, cyclesLate);
		} else {
			_startVBlankLine(video, cyclesLate);
		}
		return;

	// Lines 144 to 152
	case PPU_VBLANK_LY:
		gb->memory.io[GB_REG_LY] = video->ly;
		if (video->ly == GB_VIDEO_VERTICAL_PIXELS && !video->statLine && GBRegisterSTATIsOAMIRQ(video->stat)) {
			gb->memory.io[GB_REG_IF] |= (1 << GB_IRQ_LCDSTAT);
			_raiseIRQs(video);
		}
		_ppuSchedule(video, PPU_VBLANK_LYC, 2, cyclesLate);
		return;
	case PPU_VBLANK_LYC:
		video->lyForComparison = video->ly;
		_statUpdate(video);
		_ppuSchedule(video, PPU_VBLANK_MODE1, 1, cyclesLate);
		return;
	case PPU_VBLANK_MODE1:
		if (video->ly == GB_VIDEO_VERTICAL_PIXELS) {
			// Entering VBlank also sets off the OAM interrupt
			_setMode(video, 1);
			gb->memory.io[GB_REG_IF] |= (1 << GB_IRQ_VBLANK);
			if (!video->statLine && GBRegisterSTATIsOAMIRQ(video->stat)) {
				gb->memory.io[GB_REG_IF] |= (1 << GB_IRQ_LCDSTAT);
			}
			video->modeForInterrupt = 1;
			_statUpdate(video);
			_raiseIRQs(video);
			mTimingDeschedule(&gb->timing, &video->frameEvent);
			mTimingSchedule(&gb->timing, &video->frameEvent, -cyclesLate);
		}
		_ppuSchedule(video, PPU_VBLANK_NEXT, GB_VIDEO_HORIZONTAL_LENGTH - 5, cyclesLate);
		return;
	case PPU_VBLANK_NEXT:
		++video->ly;
		if (video->ly < GB_VIDEO_VERTICAL_TOTAL_PIXELS - 1) {
			_startVBlankLine(video, cyclesLate);
			return;
		}
		// Line 153, where LY reads 0 for most of the line
		video->lyForComparison = -1;
		_statUpdate(video);
		_ppuSchedule(video, PPU_LINE153_LY, 2, cyclesLate);
		return;
	case PPU_LINE153_LY:
		gb->memory.io[GB_REG_LY] = 153;
		_ppuSchedule(video, PPU_LINE153_LY0, cgb ? 2 : 4, cyclesLate);
		return;
	case PPU_LINE153_LY0:
		if (!cgb && !gb->doubleSpeed) {
			gb->memory.io[GB_REG_LY] = 0;
		}
		video->lyForComparison = 153;
		_statUpdate(video);
		_ppuSchedule(video, PPU_LINE153_LYC, cgb ? 4 : 2, cyclesLate);
		return;
	case PPU_LINE153_LYC:
		gb->memory.io[GB_REG_LY] = 0;
		video->lyForComparison = cgb ? 153 : -1;
		_statUpdate(video);
		_ppuSchedule(video, PPU_LINE153_LYC0, 4, cyclesLate);
		return;
	case PPU_LINE153_LYC0:
		video->lyForComparison = 0;
		_statUpdate(video);
		_ppuSchedule(video, PPU_LINE153_HOLD, 12, cyclesLate);
		return;
	case PPU_LINE153_HOLD:
		_ppuSchedule(video, PPU_LINE153_NEXT, GB_VIDEO_HORIZONTAL_LENGTH - 24, cyclesLate);
		return;
	case PPU_LINE153_NEXT:
		video->ly = 0;
		video->wyTriggered = false;
		_startLine(video, cyclesLate);
		return;
	}
}

void GBVideoSkipBIOS(struct GBVideo* video) {
	_blockAll(video, false);
	video->modeForInterrupt = 1;
	_setMode(video, 1);
	mTimingDeschedule(&video->p->timing, &video->modeEvent);
	if (video->p->model & GB_MODEL_CGB) {
		int i;
		for (i = 0; i < 0x40; ++i) {
			video->palette[i] = 0x7FFF;
			video->renderer->writePalette(video->renderer, i, video->palette[i]);
		}
		// VBlank on LY 144, 18 dots before the next line
		video->ly = GB_VIDEO_VERTICAL_PIXELS;
		video->p->memory.io[GB_REG_LY] = video->ly;
		video->lyForComparison = video->ly;
		_ppuSchedule(video, PPU_VBLANK_NEXT, 18, 0);
	} else {
		// The end of line 153, where LY already reads 0
		video->ly = GB_VIDEO_VERTICAL_TOTAL_PIXELS - 1;
		video->p->memory.io[GB_REG_LY] = 0;
		video->lyForComparison = 0;
		_ppuSchedule(video, PPU_LINE153_NEXT, 53, 0);
	}
	video->stat = GBRegisterSTATClearLYC(video->stat);
	_statUpdate(video);

	video->p->memory.io[GB_REG_IF] |= (1 << GB_IRQ_VBLANK);
	GBUpdateIRQs(video->p);
	video->p->memory.io[GB_REG_STAT] = video->stat;
}

void _updateFrameCount(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(cyclesLate);
	struct GBVideo* video = context;
	if (video->p->cpu->executionState != SM83_CORE_FETCH) {
		mTimingSchedule(timing, &video->frameEvent, (4 - ((video->p->cpu->executionState + 1) & 3)) * (2 - video->p->doubleSpeed));
		return;
	}
	if (!GBRegisterLCDCIsEnable(video->p->memory.io[GB_REG_LCDC])) {
		mTimingSchedule(timing, &video->frameEvent, GB_VIDEO_TOTAL_LENGTH << 1);
	}

	--video->frameskipCounter;
	if (video->frameskipCounter < 0) {
		video->renderer->finishFrame(video->renderer);
		video->frameskipCounter = video->frameskip;
	}
	GBFrameEnded(video->p);
	mCoreSyncPostFrame(video->p->sync);
	++video->frameCounter;
	GBInterrupt(video->p);

	GBFrameStarted(video->p);
}

static void _cleanOAM(struct GBVideo* video, int y) {
	int spriteHeight = 8;
	if (GBRegisterLCDCIsObjSize(video->p->memory.io[GB_REG_LCDC])) {
		spriteHeight = 16;
	}
	int o = 0;
	int i;
	for (i = 0; i < GB_VIDEO_MAX_OBJ && o < GB_VIDEO_MAX_LINE_OBJ; ++i) {
		uint8_t oy = video->oam.obj[i].y;
		if (y < oy - 16 || y >= oy - 16 + spriteHeight) {
			continue;
		}
		++o;
	}
	video->objMax = o;
}

void GBVideoProcessDots(struct GBVideo* video, uint32_t cyclesLate) {
	if (video->mode != 3) {
		return;
	}
	int oldX = video->x;
	video->x = ((int32_t) (mTimingCurrentTime(&video->p->timing) - cyclesLate - video->dotClock)) >> 1;
	if (video->x > GB_VIDEO_HORIZONTAL_PIXELS) {
		video->x = GB_VIDEO_HORIZONTAL_PIXELS;
	} else if (video->x < 0) {
		return;
	}
	if (oldX < 0) {
		oldX = 0;
	}
	if (video->frameskipCounter <= 0) {
		video->renderer->drawRange(video->renderer, oldX, video->x, video->ly);
	}
}

void GBVideoWriteLCDC(struct GBVideo* video, GBRegisterLCDC value) {
	struct GB* gb = video->p;
	if (!GBRegisterLCDCIsEnable(gb->memory.io[GB_REG_LCDC]) && GBRegisterLCDCIsEnable(value)) {
		gb->memory.io[GB_REG_LCDC] = value;
		// Line 0 starts with mode 0, a T-cycle after the write on a DMG and as it happens on a CGB
		video->ly = 0;
		gb->memory.io[GB_REG_LY] = 0;
		video->wyTriggered = false;
		video->lyForComparison = 0;
		_setMode(video, 0);
		video->modeForInterrupt = -1;
		_blockAll(video, false);
		video->lineCycles = GB_VIDEO_MODE_2_LENGTH - 4;
		_statUpdate(video);
		video->renderer->writePalette(video->renderer, 0, video->palette[0]);

		int t = gb->cpu->tMultiplier;
		video->ppuState = PPU_LCDON_OAM;
		mTimingDeschedule(&gb->timing, &video->modeEvent);
		mTimingSchedule(&gb->timing, &video->modeEvent, ((GB_VIDEO_MODE_2_LENGTH - 4) << 1) - t + (_isCGB(video) ? 0 : 2));
		mTimingDeschedule(&gb->timing, &video->frameEvent);
	}
	if (GBRegisterLCDCIsEnable(gb->memory.io[GB_REG_LCDC]) && !GBRegisterLCDCIsEnable(value)) {
		// When the LCD is off, LY is 0, STAT shows mode 0 and everything is open
		video->ppuState = PPU_OFF;
		_setMode(video, 0);
		video->ly = 0;
		gb->memory.io[GB_REG_LY] = 0;
		video->lyForComparison = 0;
		video->wyTriggered = false;
		_blockAll(video, false);
		video->renderer->writePalette(video->renderer, 0, video->dmgPalette[0]);

		mTimingDeschedule(&gb->timing, &video->modeEvent);
		mTimingDeschedule(&gb->timing, &video->frameEvent);
		mTimingSchedule(&gb->timing, &video->frameEvent, GB_VIDEO_TOTAL_LENGTH << 1);
	}
	gb->memory.io[GB_REG_STAT] = video->stat;
}

void GBVideoWriteSTAT(struct GBVideo* video, GBRegisterSTAT value) {
	video->stat = (video->stat & 0x7) | (value & 0x78);
	_statUpdate(video);
}

void GBVideoWriteLYC(struct GBVideo* video, uint8_t value) {
	struct GB* gb = video->p;
	bool cgb = _isCGB(video);
	if (video->ppuState == PPU_LINE153_HOLD && cgb) {
		video->lyForComparison = 153;
		_statUpdate(video);
		video->lyForComparison = 0;
	}
	gb->memory.io[GB_REG_LYC] = value;
	// On a CGB, a write as LY changes is seen when the PPU compares
	if (!cgb || (video->ppuState != PPU_LINE_OAM && video->ppuState != PPU_VBLANK_LY &&
	             video->ppuState != PPU_LINE153_LYC && video->ppuState != PPU_LINE153_LYC0)) {
		if (video->ppuState == PPU_LINE153_LY0 && cgb) {
			video->lyForComparison = 153;
			_statUpdate(video);
			video->lyForComparison = -1;
		} else {
			_statUpdate(video);
		}
	}
	gb->memory.io[GB_REG_STAT] = video->stat;
}

void GBVideoWritePalette(struct GBVideo* video, uint16_t address, uint8_t value) {
	if (video->p->model < GB_MODEL_SGB) {
		switch (address) {
		case GB_REG_BGP:
			video->palette[0] = video->dmgPalette[value & 3];
			video->palette[1] = video->dmgPalette[(value >> 2) & 3];
			video->palette[2] = video->dmgPalette[(value >> 4) & 3];
			video->palette[3] = video->dmgPalette[(value >> 6) & 3];
			video->renderer->writePalette(video->renderer, 0, video->palette[0]);
			video->renderer->writePalette(video->renderer, 1, video->palette[1]);
			video->renderer->writePalette(video->renderer, 2, video->palette[2]);
			video->renderer->writePalette(video->renderer, 3, video->palette[3]);
			break;
		case GB_REG_OBP0:
			video->palette[8 * 4 + 0] = video->dmgPalette[(value & 3) + 4];
			video->palette[8 * 4 + 1] = video->dmgPalette[((value >> 2) & 3) + 4];
			video->palette[8 * 4 + 2] = video->dmgPalette[((value >> 4) & 3) + 4];
			video->palette[8 * 4 + 3] = video->dmgPalette[((value >> 6) & 3) + 4];
			video->renderer->writePalette(video->renderer, 8 * 4 + 0, video->palette[8 * 4 + 0]);
			video->renderer->writePalette(video->renderer, 8 * 4 + 1, video->palette[8 * 4 + 1]);
			video->renderer->writePalette(video->renderer, 8 * 4 + 2, video->palette[8 * 4 + 2]);
			video->renderer->writePalette(video->renderer, 8 * 4 + 3, video->palette[8 * 4 + 3]);
			break;
		case GB_REG_OBP1:
			video->palette[9 * 4 + 0] = video->dmgPalette[(value & 3) + 8];
			video->palette[9 * 4 + 1] = video->dmgPalette[((value >> 2) & 3) + 8];
			video->palette[9 * 4 + 2] = video->dmgPalette[((value >> 4) & 3) + 8];
			video->palette[9 * 4 + 3] = video->dmgPalette[((value >> 6) & 3) + 8];
			video->renderer->writePalette(video->renderer, 9 * 4 + 0, video->palette[9 * 4 + 0]);
			video->renderer->writePalette(video->renderer, 9 * 4 + 1, video->palette[9 * 4 + 1]);
			video->renderer->writePalette(video->renderer, 9 * 4 + 2, video->palette[9 * 4 + 2]);
			video->renderer->writePalette(video->renderer, 9 * 4 + 3, video->palette[9 * 4 + 3]);
			break;
		}
	} else if (video->p->model >= GB_MODEL_CGB) {
		switch (address) {
		case GB_REG_BCPD:
			if (!video->paletteBlocked) {
				if (video->bcpIndex & 1) {
					video->palette[video->bcpIndex >> 1] &= 0x00FF;
					video->palette[video->bcpIndex >> 1] |= value << 8;
				} else {
					video->palette[video->bcpIndex >> 1] &= 0xFF00;
					video->palette[video->bcpIndex >> 1] |= value;
				}
				video->renderer->writePalette(video->renderer, video->bcpIndex >> 1, video->palette[video->bcpIndex >> 1]);
			}
			if (video->bcpIncrement) {
				++video->bcpIndex;
				video->bcpIndex &= 0x3F;
				video->p->memory.io[GB_REG_BCPS] &= 0x80;
				video->p->memory.io[GB_REG_BCPS] |= video->bcpIndex;
			}
			video->p->memory.io[GB_REG_BCPD] = video->palette[video->bcpIndex >> 1] >> (8 * (video->bcpIndex & 1));
			break;
		case GB_REG_OCPD:
			if (!video->paletteBlocked) {
				if (video->ocpIndex & 1) {
					video->palette[8 * 4 + (video->ocpIndex >> 1)] &= 0x00FF;
					video->palette[8 * 4 + (video->ocpIndex >> 1)] |= value << 8;
				} else {
					video->palette[8 * 4 + (video->ocpIndex >> 1)] &= 0xFF00;
					video->palette[8 * 4 + (video->ocpIndex >> 1)] |= value;
				}
				video->renderer->writePalette(video->renderer, 8 * 4 + (video->ocpIndex >> 1), video->palette[8 * 4 + (video->ocpIndex >> 1)]);
			}
			if (video->ocpIncrement) {
				++video->ocpIndex;
				video->ocpIndex &= 0x3F;
				video->p->memory.io[GB_REG_OCPS] &= 0x80;
				video->p->memory.io[GB_REG_OCPS] |= video->ocpIndex;
			}
			video->p->memory.io[GB_REG_OCPD] = video->palette[8 * 4 + (video->ocpIndex >> 1)] >> (8 * (video->ocpIndex & 1));
			break;
		}
	} else {
		video->renderer->writeVideoRegister(video->renderer, address, value);
	}
}

void GBVideoSwitchBank(struct GBVideo* video, uint8_t value) {
	value &= 1;
	video->vramBank = &video->vram[value * GB_SIZE_VRAM_BANK0];
	video->vramCurrentBank = value;
}

void GBVideoSetPalette(struct GBVideo* video, unsigned index, uint32_t color) {
	if (index >= 12) {
		return;
	}
	video->dmgPalette[index] = M_RGB8_TO_RGB5(color);
}

void GBVideoDisableCGB(struct GBVideo* video) {
	video->dmgPalette[0] = video->palette[0];
	video->dmgPalette[1] = video->palette[1];
	video->dmgPalette[2] = video->palette[2];
	video->dmgPalette[3] = video->palette[3];
	video->dmgPalette[4] = video->palette[8 * 4 + 0];
	video->dmgPalette[5] = video->palette[8 * 4 + 1];
	video->dmgPalette[6] = video->palette[8 * 4 + 2];
	video->dmgPalette[7] = video->palette[8 * 4 + 3];
	video->dmgPalette[8] = video->palette[9 * 4 + 0];
	video->dmgPalette[9] = video->palette[9 * 4 + 1];
	video->dmgPalette[10] = video->palette[9 * 4 + 2];
	video->dmgPalette[11] = video->palette[9 * 4 + 3];
	video->renderer->deinit(video->renderer);
	video->renderer->init(video->renderer, video->p->model, video->sgbBorders);
}

void GBVideoWriteSGBPacket(struct GBVideo* video, uint8_t* data) {
	int i;
	if (!(video->sgbCommandHeader & 7)) {
		video->sgbBufferIndex = 0;
		if ((data[0] >> 3) > SGB_OBJ_TRN) {
			video->sgbCommandHeader = 0;
			return;
		}
		video->sgbCommandHeader = data[0];
	}
	--video->sgbCommandHeader;
	memcpy(&video->sgbPacketBuffer[video->sgbBufferIndex << 4], data, 16);
	++video->sgbBufferIndex;
	if (video->sgbCommandHeader & 7) {
		return;
	}
	switch (video->sgbCommandHeader >> 3) {
	case SGB_PAL01:
		video->palette[0] = video->sgbPacketBuffer[1] | (video->sgbPacketBuffer[2] << 8);
		video->palette[1] = video->sgbPacketBuffer[3] | (video->sgbPacketBuffer[4] << 8);
		video->palette[2] = video->sgbPacketBuffer[5] | (video->sgbPacketBuffer[6] << 8);
		video->palette[3] = video->sgbPacketBuffer[7] | (video->sgbPacketBuffer[8] << 8);

		video->palette[4] = video->sgbPacketBuffer[1] | (video->sgbPacketBuffer[2] << 8);
		video->palette[5] = video->sgbPacketBuffer[9] | (video->sgbPacketBuffer[10] << 8);
		video->palette[6] = video->sgbPacketBuffer[11] | (video->sgbPacketBuffer[12] << 8);
		video->palette[7] = video->sgbPacketBuffer[13] | (video->sgbPacketBuffer[14] << 8);

		video->palette[8] = video->sgbPacketBuffer[1] | (video->sgbPacketBuffer[2] << 8);
		video->palette[12] = video->sgbPacketBuffer[1] | (video->sgbPacketBuffer[2] << 8);

		video->renderer->writePalette(video->renderer, 0, video->palette[0]);
		video->renderer->writePalette(video->renderer, 1, video->palette[1]);
		video->renderer->writePalette(video->renderer, 2, video->palette[2]);
		video->renderer->writePalette(video->renderer, 3, video->palette[3]);
		video->renderer->writePalette(video->renderer, 4, video->palette[4]);
		video->renderer->writePalette(video->renderer, 5, video->palette[5]);
		video->renderer->writePalette(video->renderer, 6, video->palette[6]);
		video->renderer->writePalette(video->renderer, 7, video->palette[7]);
		video->renderer->writePalette(video->renderer, 8, video->palette[8]);
		video->renderer->writePalette(video->renderer, 12, video->palette[12]);
		break;
	case SGB_PAL23:
		video->palette[9] = video->sgbPacketBuffer[3] | (video->sgbPacketBuffer[4] << 8);
		video->palette[10] = video->sgbPacketBuffer[5] | (video->sgbPacketBuffer[6] << 8);
		video->palette[11] = video->sgbPacketBuffer[7] | (video->sgbPacketBuffer[8] << 8);

		video->palette[13] = video->sgbPacketBuffer[9] | (video->sgbPacketBuffer[10] << 8);
		video->palette[14] = video->sgbPacketBuffer[11] | (video->sgbPacketBuffer[12] << 8);
		video->palette[15] = video->sgbPacketBuffer[13] | (video->sgbPacketBuffer[14] << 8);
		video->renderer->writePalette(video->renderer, 9, video->palette[9]);
		video->renderer->writePalette(video->renderer, 10, video->palette[10]);
		video->renderer->writePalette(video->renderer, 11, video->palette[11]);
		video->renderer->writePalette(video->renderer, 13, video->palette[13]);
		video->renderer->writePalette(video->renderer, 14, video->palette[14]);
		video->renderer->writePalette(video->renderer, 15, video->palette[15]);
		break;
	case SGB_PAL03:
		video->palette[0] = video->sgbPacketBuffer[1] | (video->sgbPacketBuffer[2] << 8);
		video->palette[1] = video->sgbPacketBuffer[3] | (video->sgbPacketBuffer[4] << 8);
		video->palette[2] = video->sgbPacketBuffer[5] | (video->sgbPacketBuffer[6] << 8);
		video->palette[3] = video->sgbPacketBuffer[7] | (video->sgbPacketBuffer[8] << 8);

		video->palette[4] = video->sgbPacketBuffer[1] | (video->sgbPacketBuffer[2] << 8);
		video->palette[8] = video->sgbPacketBuffer[1] | (video->sgbPacketBuffer[2] << 8);

		video->palette[12] = video->sgbPacketBuffer[1] | (video->sgbPacketBuffer[2] << 8);
		video->palette[13] = video->sgbPacketBuffer[9] | (video->sgbPacketBuffer[10] << 8);
		video->palette[14] = video->sgbPacketBuffer[11] | (video->sgbPacketBuffer[12] << 8);
		video->palette[15] = video->sgbPacketBuffer[13] | (video->sgbPacketBuffer[14] << 8);
		video->renderer->writePalette(video->renderer, 0, video->palette[0]);
		video->renderer->writePalette(video->renderer, 1, video->palette[1]);
		video->renderer->writePalette(video->renderer, 2, video->palette[2]);
		video->renderer->writePalette(video->renderer, 3, video->palette[3]);
		video->renderer->writePalette(video->renderer, 4, video->palette[4]);
		video->renderer->writePalette(video->renderer, 8, video->palette[8]);
		video->renderer->writePalette(video->renderer, 12, video->palette[12]);
		video->renderer->writePalette(video->renderer, 13, video->palette[13]);
		video->renderer->writePalette(video->renderer, 14, video->palette[14]);
		video->renderer->writePalette(video->renderer, 15, video->palette[15]);
		break;
	case SGB_PAL12:
		video->palette[5] = video->sgbPacketBuffer[3] | (video->sgbPacketBuffer[4] << 8);
		video->palette[6] = video->sgbPacketBuffer[5] | (video->sgbPacketBuffer[6] << 8);
		video->palette[7] = video->sgbPacketBuffer[7] | (video->sgbPacketBuffer[8] << 8);

		video->palette[9] = video->sgbPacketBuffer[9] | (video->sgbPacketBuffer[10] << 8);
		video->palette[10] = video->sgbPacketBuffer[11] | (video->sgbPacketBuffer[12] << 8);
		video->palette[11] = video->sgbPacketBuffer[13] | (video->sgbPacketBuffer[14] << 8);
		video->renderer->writePalette(video->renderer, 5, video->palette[5]);
		video->renderer->writePalette(video->renderer, 6, video->palette[6]);
		video->renderer->writePalette(video->renderer, 7, video->palette[7]);
		video->renderer->writePalette(video->renderer, 9, video->palette[9]);
		video->renderer->writePalette(video->renderer, 10, video->palette[10]);
		video->renderer->writePalette(video->renderer, 11, video->palette[11]);
		break;
	case SGB_PAL_SET:
		for (i = 0; i < 4; ++i) {
			uint16_t entry = (video->sgbPacketBuffer[2 + (i * 2)] << 8) | video->sgbPacketBuffer[1 + (i * 2)];
			if (entry >= 0x200) {
				mLOG(GB, STUB, "Unimplemented SGB palette overflow: %03X", entry);
				continue;
			}
			LOAD_16LE(video->palette[i * 4 + 0], entry * 8 + 0, video->renderer->sgbPalRam);
			video->renderer->writePalette(video->renderer, i * 4 + 0, video->palette[i * 4 + 0]);
			LOAD_16LE(video->palette[i * 4 + 1], entry * 8 + 2, video->renderer->sgbPalRam);
			video->renderer->writePalette(video->renderer, i * 4 + 1, video->palette[i * 4 + 1]);
			LOAD_16LE(video->palette[i * 4 + 2], entry * 8 + 4, video->renderer->sgbPalRam);
			video->renderer->writePalette(video->renderer, i * 4 + 2, video->palette[i * 4 + 2]);
			LOAD_16LE(video->palette[i * 4 + 3], entry * 8 + 6, video->renderer->sgbPalRam);
			video->renderer->writePalette(video->renderer, i * 4 + 3, video->palette[i * 4 + 3]);
		}
		break;
	case SGB_ATTR_BLK:
	case SGB_ATTR_DIV:
	case SGB_ATTR_CHR:
	case SGB_ATTR_LIN:
	case SGB_PAL_TRN:
	case SGB_ATRC_EN:
	case SGB_CHR_TRN:
	case SGB_PCT_TRN:
	case SGB_ATTR_TRN:
	case SGB_ATTR_SET:
		break;
	case SGB_MLT_REQ:
		if ((video->sgbPacketBuffer[1] & 0x3) == 2) { // XXX: This unmasked increment appears to be an SGB hardware bug
			++video->p->sgbCurrentController;
		}
		video->p->sgbControllers = video->sgbPacketBuffer[1] & 0x3;
		video->p->sgbCurrentController &= video->p->sgbControllers;
		return;
	case SGB_MASK_EN:
		video->renderer->sgbRenderMode = video->sgbPacketBuffer[1] & 0x3;
		break;
	default:
		mLOG(GB, STUB, "Unimplemented SGB command: %02X", video->sgbPacketBuffer[0] >> 3);
		return;
	}
	video->renderer->writeSGBPacket(video->renderer, video->sgbPacketBuffer);
}

static void GBVideoDummyRendererInit(struct GBVideoRenderer* renderer, enum GBModel model, bool borders) {
	UNUSED(renderer);
	UNUSED(model);
	UNUSED(borders);
	// Nothing to do
}

static void GBVideoDummyRendererDeinit(struct GBVideoRenderer* renderer) {
	UNUSED(renderer);
	// Nothing to do
}

static uint8_t GBVideoDummyRendererWriteVideoRegister(struct GBVideoRenderer* renderer, uint16_t address, uint8_t value) {
	if (renderer->cache) {
		GBVideoCacheWriteVideoRegister(renderer->cache, address, value);
	}
	return value;
}

static void GBVideoDummyRendererWriteSGBPacket(struct GBVideoRenderer* renderer, uint8_t* data) {
	UNUSED(renderer);
	UNUSED(data);
}

static void GBVideoDummyRendererWriteVRAM(struct GBVideoRenderer* renderer, uint16_t address) {
	if (renderer->cache) {
		mCacheSetWriteVRAM(renderer->cache, address);
	}
}

static void GBVideoDummyRendererWriteOAM(struct GBVideoRenderer* renderer, uint16_t oam) {
	UNUSED(renderer);
	UNUSED(oam);
	// Nothing to do
}

static void GBVideoDummyRendererWritePalette(struct GBVideoRenderer* renderer, int index, uint16_t value) {
	if (renderer->cache) {
		mCacheSetWritePalette(renderer->cache, index, mColorFrom555(value));
	}
}

static void GBVideoDummyRendererDrawRange(struct GBVideoRenderer* renderer, int startX, int endX, int y) {
	UNUSED(renderer);
	UNUSED(endX);
	UNUSED(startX);
	UNUSED(y);
	// Nothing to do
}

static void GBVideoDummyRendererFinishScanline(struct GBVideoRenderer* renderer, int y) {
	UNUSED(renderer);
	UNUSED(y);
	// Nothing to do
}

static void GBVideoDummyRendererFinishFrame(struct GBVideoRenderer* renderer) {
	UNUSED(renderer);
	// Nothing to do
}

static void GBVideoDummyRendererEnableSGBBorder(struct GBVideoRenderer* renderer, bool enable) {
	UNUSED(renderer);
	UNUSED(enable);
	// Nothing to do
}

static void GBVideoDummyRendererGetPixels(struct GBVideoRenderer* renderer, size_t* stride, const void** pixels) {
	UNUSED(renderer);
	UNUSED(stride);
	UNUSED(pixels);
	// Nothing to do
}

static void GBVideoDummyRendererPutPixels(struct GBVideoRenderer* renderer, size_t stride, const void* pixels) {
	UNUSED(renderer);
	UNUSED(stride);
	UNUSED(pixels);
	// Nothing to do
}

void GBVideoSerialize(const struct GBVideo* video, struct GBSerializedState* state) {
	STORE_16LE(video->x, 0, &state->video.x);
	STORE_16LE(video->ly, 0, &state->video.ly);
	STORE_32LE(video->frameCounter, 0, &state->video.frameCounter);
	STORE_32LE(video->dotClock, 0, &state->video.dotCounter);
	state->video.vramCurrentBank = video->vramCurrentBank;

	GBSerializedVideoFlags flags = 0;
	flags = GBSerializedVideoFlagsSetBcpIncrement(flags, video->bcpIncrement);
	flags = GBSerializedVideoFlagsSetOcpIncrement(flags, video->ocpIncrement);
	flags = GBSerializedVideoFlagsSetMode(flags, video->mode);
	flags = GBSerializedVideoFlagsSetNotModeEventScheduled(flags, !mTimingIsScheduled(&video->p->timing, &video->modeEvent));
	flags = GBSerializedVideoFlagsSetNotFrameEventScheduled(flags, !mTimingIsScheduled(&video->p->timing, &video->frameEvent));
	state->video.flags = flags;
	STORE_16LE(video->bcpIndex, 0, &state->video.bcpIndex);
	STORE_16LE(video->ocpIndex, 0, &state->video.ocpIndex);

	size_t i;
	for (i = 0; i < 64; ++i) {
		STORE_16LE(video->palette[i], i * 2, state->video.palette);
	}

	STORE_32LE(video->modeEvent.when - mTimingCurrentTime(&video->p->timing), 0, &state->video.nextMode);
	STORE_32LE(video->frameEvent.when - mTimingCurrentTime(&video->p->timing), 0, &state->video.nextFrame);

	memcpy(state->vram, video->vram, GB_SIZE_VRAM);
	memcpy(state->oam, &video->oam.raw, GB_SIZE_OAM);

	state->ppu.state = video->ppuState;
	state->ppu.modeForInterrupt = video->modeForInterrupt;
	STORE_16LE(video->lyForComparison, 0, &state->ppu.lyForComparison);
	STORE_16LE(video->lineCycles, 0, &state->ppu.lineCycles);
	GBSerializedPPUFlags ppuFlags = 0;
	ppuFlags = GBSerializedPPUFlagsSetStatLine(ppuFlags, video->statLine);
	ppuFlags = GBSerializedPPUFlagsSetLycLine(ppuFlags, video->lycLine);
	ppuFlags = GBSerializedPPUFlagsSetWyTriggered(ppuFlags, video->wyTriggered);
	ppuFlags = GBSerializedPPUFlagsSetOamReadBlocked(ppuFlags, video->oamReadBlocked);
	ppuFlags = GBSerializedPPUFlagsSetOamWriteBlocked(ppuFlags, video->oamWriteBlocked);
	ppuFlags = GBSerializedPPUFlagsSetVramReadBlocked(ppuFlags, video->vramReadBlocked);
	ppuFlags = GBSerializedPPUFlagsSetVramWriteBlocked(ppuFlags, video->vramWriteBlocked);
	ppuFlags = GBSerializedPPUFlagsSetPaletteBlocked(ppuFlags, video->paletteBlocked);
	state->ppu.flags = ppuFlags;
}

void GBVideoDeserialize(struct GBVideo* video, const struct GBSerializedState* state) {
	LOAD_16LE(video->x, 0, &state->video.x);
	LOAD_16LE(video->ly, 0, &state->video.ly);
	LOAD_32LE(video->frameCounter, 0, &state->video.frameCounter);
	LOAD_32LE(video->dotClock, 0, &state->video.dotCounter);
	video->x = (int16_t) video->x; // Ensure proper sign extension--the LOAD_16 is unsigned

	video->vramCurrentBank = state->video.vramCurrentBank;

	GBSerializedVideoFlags flags = state->video.flags;
	video->bcpIncrement = GBSerializedVideoFlagsGetBcpIncrement(flags);
	video->ocpIncrement = GBSerializedVideoFlagsGetOcpIncrement(flags);
	video->mode = GBSerializedVideoFlagsGetMode(flags);
	video->ppuState = state->ppu.state;
	video->modeForInterrupt = state->ppu.modeForInterrupt;
	int16_t lyForComparison;
	LOAD_16LE(lyForComparison, 0, &state->ppu.lyForComparison);
	video->lyForComparison = lyForComparison;
	int16_t lineCycles;
	LOAD_16LE(lineCycles, 0, &state->ppu.lineCycles);
	video->lineCycles = lineCycles;
	GBSerializedPPUFlags ppuFlags = state->ppu.flags;
	video->statLine = GBSerializedPPUFlagsGetStatLine(ppuFlags);
	video->lycLine = GBSerializedPPUFlagsGetLycLine(ppuFlags);
	video->wyTriggered = GBSerializedPPUFlagsGetWyTriggered(ppuFlags);
	video->oamReadBlocked = GBSerializedPPUFlagsGetOamReadBlocked(ppuFlags);
	video->oamWriteBlocked = GBSerializedPPUFlagsGetOamWriteBlocked(ppuFlags);
	video->vramReadBlocked = GBSerializedPPUFlagsGetVramReadBlocked(ppuFlags);
	video->vramWriteBlocked = GBSerializedPPUFlagsGetVramWriteBlocked(ppuFlags);
	video->paletteBlocked = GBSerializedPPUFlagsGetPaletteBlocked(ppuFlags);
	LOAD_16LE(video->bcpIndex, 0, &state->video.bcpIndex);
	video->bcpIndex &= 0x3F;
	LOAD_16LE(video->ocpIndex, 0, &state->video.ocpIndex);
	video->ocpIndex &= 0x3F;

	video->modeEvent.callback = _ppuStep;
	if (video->ppuState == PPU_OFF && GBRegisterLCDCIsEnable(state->io[GB_REG_LCDC])) {
		// A state from before the PPU kept its own steps: resume at the step that ends the mode it was in
		static const int resume[4] = { PPU_LINE_NEXT, PPU_VBLANK_NEXT, PPU_LINE_MODE3, PPU_MODE3_END };
		video->ppuState = resume[video->mode & 3];
		if (video->mode == 1 && video->ly >= GB_VIDEO_VERTICAL_TOTAL_PIXELS - 1) {
			video->ppuState = PPU_LINE153_NEXT;
		}
		video->modeForInterrupt = video->mode == 3 ? 3 : video->mode;
		video->lyForComparison = state->io[GB_REG_LY];
		video->lineCycles = video->mode == 3 ? 89 + 167 + (state->io[GB_REG_SCX] & 7) : GB_VIDEO_MODE_2_LENGTH + 4;
		video->oamReadBlocked = video->mode >= 2;
		video->oamWriteBlocked = video->mode >= 2;
		video->vramReadBlocked = video->mode == 3;
		video->vramWriteBlocked = video->mode == 3;
		video->paletteBlocked = video->mode == 3;
	}

	uint32_t when;
	LOAD_32LE(when, 0, &state->video.nextMode);
	if (!GBSerializedVideoFlagsIsNotModeEventScheduled(flags)) {
		mTimingSchedule(&video->p->timing, &video->modeEvent, when);
	} else {
		video->modeEvent.when = when + mTimingCurrentTime(&video->p->timing);
	}
	LOAD_32LE(when, 0, &state->video.nextFrame);
	if (!GBSerializedVideoFlagsIsNotFrameEventScheduled(flags)) {
		mTimingSchedule(&video->p->timing, &video->frameEvent, when);
	} else {
		video->frameEvent.when = when + mTimingCurrentTime(&video->p->timing);
	}

	video->renderer->deinit(video->renderer);
	video->renderer->init(video->renderer, video->p->model, video->sgbBorders);

	size_t i;
	for (i = 0; i < 64; ++i) {
		LOAD_16LE(video->palette[i], i * 2, state->video.palette);
		video->renderer->writePalette(video->renderer, i, video->palette[i]);
	}

	memcpy(video->vram, state->vram, GB_SIZE_VRAM);
	memcpy(&video->oam.raw, state->oam, GB_SIZE_OAM);

	_cleanOAM(video, video->ly);
	_scanObjects(video);
	GBVideoSwitchBank(video, video->vramCurrentBank);
}
