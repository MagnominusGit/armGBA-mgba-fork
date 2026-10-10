/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// AArch64 backend for jit.c

#define R_CPU 19
#define R_JIT 20
#define R_IWRAM 21
#define R_EWRAM 22
// A transfer's base writeback, or a value kept across C calls
#define R_WB 23
#define R_COVER 24
#define A64_ZR 31
#define A64_SP 31
// The cycle count lives here while generated code runs
#define R_CYCLES 25
// cpu->nextEvent, reloaded whenever C code may have changed it
#define R_NEXT 27
#define R_GBA 28
// The guest NZCV in bits 28 to 31 while generated code runs; CPSR in memory gets it before any C code runs
#define R_FLAGS 26
// A shared routine that calls more routines keeps its own return address here
#define R_RETURN 29
#define FRAME_SIZE 128
// Stack slots: a C call's cycles and the address of the access in flight
#define CYCLE_SLOT 96
#define ADDRESS_SLOT (CYCLE_SLOT + 4)
// The return address of generated code that calls a shared routine
#define LR_SLOT (CYCLE_SLOT + 16)
// The PC a cartridge access passes to its routine for the prefetch stall
#define PC_SLOT (CYCLE_SLOT + 24)

enum {
	A64_EQ = 0,
	A64_NE = 1,
	A64_HS = 2,
	A64_LO = 3,
	A64_HI = 8,
	A64_GE = 10,
};

// Data processing on w registers, or x registers with A64_X: rd = rn op rm, with rm optionally shifted left for AND to SUBS
enum {
	A64_X = 0x80000000,
	A64_AND = 0x0A000000, A64_BIC = 0x0A200000, A64_ORR = 0x2A000000, A64_ORN = 0x2A200000, A64_EOR = 0x4A000000, A64_ANDS = 0x6A000000,
	A64_ADD = 0x0B000000, A64_ADDS = 0x2B000000, A64_SUB = 0x4B000000, A64_SUBS = 0x6B000000,
	A64_ADC = 0x1A000000, A64_ADCS = 0x3A000000, A64_SBC = 0x5A000000, A64_SBCS = 0x7A000000,
	A64_LSLV = 0x1AC02000, A64_LSRV = 0x1AC02400, A64_RORV = 0x1AC02C00,
};

// Immediate forms: a 12-bit value for add and subtract, a bitmask for the logical ones
enum {
	A64_ADD_IMM = 0x11000000, A64_ADDS_IMM = 0x31000000, A64_SUB_IMM = 0x51000000, A64_SUBS_IMM = 0x71000000,
	A64_AND_IMM = 0x12000000, A64_ORR_IMM = 0x32000000, A64_EOR_IMM = 0x52000000, A64_ANDS_IMM = 0x72000000,
};

// Loads and stores at [rn + rm], rm zero-extended (UXTW) or 64-bit (LSL), optionally scaled by the size
enum {
	A64_LDRB_R = 0x38600800, A64_LDRH_R = 0x78600800, A64_LDR_R = 0xB8600800, A64_LDRX_R = 0xF8600800,
	A64_STRB_R = 0x38200800, A64_STRH_R = 0x78200800, A64_STR_R = 0xB8200800,
	A64_UXTW = 0x4000, A64_LSL = 0x6000, A64_SCALED = 0x1000,
};

// The prefetch stall of a data access from cartridge code, as generated code takes it from the tables, for waits past them
static int32_t _romStallC(struct GBA* gba, uint32_t pc, int32_t wait) {
	struct ARMCore* cpu = gba->cpu;
	int32_t s = cpu->memory.activeSeqCycles16;
	int32_t n = cpu->memory.activeNonseqCycles16;
	uint64_t advances;
	uint64_t stalls;
	_stallTables(s, wait, &advances, &stalls);
	uint32_t ahead = gba->memory.lastPrefetchedPc - pc;
	unsigned shift = ahead < 16 ? ((ahead >> 1) & 7) * 8 : 0;
	gba->memory.lastPrefetchedPc = pc + 2 * ((advances >> shift) & 0xFF);
	return (int8_t) (stalls >> shift) + wait - (n - s);
}

static void _emit(struct Emitter* e, uint32_t insn) {
	memcpy(e->p, &insn, 4);
	e->p += 4;
}

static uint8_t* _emitSite(struct Emitter* e, uint32_t insn) {
	uint8_t* at = e->p;
	_emit(e, insn);
	return at;
}

// Immediates and branches that do not fit their fields are compiler bugs, so they stop it
static uint32_t _field(uint32_t value, unsigned bits) {
	if (value >> bits) {
		abort();
	}
	return value;
}

// B and BL take 26 bits, TBZ and TBNZ 14, B.cond, CBZ and CBNZ 19
static void _patch(uint8_t* at, const uint8_t* target) {
	int32_t offset = (target - at) / 4;
	uint32_t insn;
	memcpy(&insn, at, 4);
	unsigned bits = (insn & 0x7C000000) == 0x14000000 ? 26 : (insn & 0x7E000000) == 0x36000000 ? 14 : 19;
	int32_t limit = 1 << (bits - 1);
	if (offset < -limit || offset >= limit) {
		abort();
	}
	uint32_t field = (uint32_t) offset & ((1u << bits) - 1);
	insn |= bits == 26 ? field : field << 5;
	memcpy(at, &insn, 4);
}

static uint32_t _ror(uint32_t value, unsigned amount, unsigned size) {
	uint32_t mask = size == 32 ? 0xFFFFFFFF : (1u << size) - 1;
	return amount ? ((value >> amount) | (value << (size - amount))) & mask : value;
}

// immr and imms of a logical immediate: a rotated run of ones repeated in 2 to 32-bit elements
static bool _bitmaskImm(uint32_t value, uint32_t* fields) {
	if (!value || value == 0xFFFFFFFF) {
		return false;
	}
	unsigned size = 32;
	while (size > 2 && ((value >> (size / 2)) & ((1u << (size / 2)) - 1)) == (value & ((1u << (size / 2)) - 1))) {
		size /= 2;
	}
	uint32_t element = value & (size == 32 ? 0xFFFFFFFF : (1u << size) - 1);
	unsigned ones = __builtin_popcount(element);
	unsigned rotate;
	for (rotate = 0; rotate < size; ++rotate) {
		if (_ror(element, rotate, size) == (1u << ones) - 1) {
			*fields = (((size - rotate) % size) << 16) | ((((~(size - 1)) << 1) & 0x3F) | (ones - 1)) << 10;
			return true;
		}
	}
	return false;
}

static void _dp(struct Emitter* e, uint32_t op, int rd, int rn, int rm, unsigned lsl) {
	_emit(e, op | (rm << 16) | (lsl << 10) | (rn << 5) | rd);
}

static void _dpImm(struct Emitter* e, uint32_t op, int rd, int rn, unsigned imm) {
	_emit(e, op | (_field(imm, 12) << 10) | (rn << 5) | rd);
}

// Logical immediates must be encodable bitmasks
static void _logicImm(struct Emitter* e, uint32_t op, int rd, int rn, uint32_t value) {
	uint32_t fields = 0;
	if (!_bitmaskImm(value, &fields)) {
		abort();
	}
	_emit(e, op | fields | (rn << 5) | rd);
}

static void _ldstR(struct Emitter* e, uint32_t op, int rt, int rn, int rm) {
	_emit(e, op | (rm << 16) | (rn << 5) | rt);
}

// One MOVZ or MOVN when a halfword is all zeros or all ones, else MOVZ and MOVK
static void _movImm32(struct Emitter* e, int rd, uint32_t value) {
	if (!(value & 0xFFFF) && value) {
		_emit(e, 0x52A00000 | ((value >> 16) << 5) | rd);
	} else if ((value >> 16) == 0xFFFF) {
		_emit(e, 0x12800000 | ((~value & 0xFFFF) << 5) | rd);
	} else {
		_emit(e, 0x52800000 | ((value & 0xFFFF) << 5) | rd);
		if (value >> 16) {
			_emit(e, 0x72A00000 | ((value >> 16) << 5) | rd);
		}
	}
}

// MOVN for mostly-ones values, MOVZ otherwise, then MOVK for each halfword left
static void _movImm64(struct Emitter* e, int rd, uint64_t value) {
	unsigned ones = 0;
	int hw;
	for (hw = 0; hw < 4; ++hw) {
		ones += ((value >> (16 * hw)) & 0xFFFF) == 0xFFFF;
	}
	uint64_t fill = ones > 1 ? 0xFFFF : 0;
	int first = 0;
	for (hw = 0; hw < 4; ++hw) {
		if (((value >> (16 * hw)) & 0xFFFF) != fill) {
			first = hw;
			break;
		}
	}
	uint32_t part = (value >> (16 * first)) & 0xFFFF;
	_emit(e, (fill ? 0x92800000 | ((~part & 0xFFFF) << 5) : 0xD2800000 | (part << 5)) | (first << 21) | rd);
	for (hw = first + 1; hw < 4; ++hw) {
		part = (value >> (16 * hw)) & 0xFFFF;
		if (part != fill) {
			_emit(e, 0xF2800000 | (hw << 21) | (part << 5) | rd);
		}
	}
}

// A load or store's unsigned offset, in units of the access size
static uint32_t _scaled(unsigned offset, unsigned scale) {
	if (offset & ((1 << scale) - 1)) {
		abort();
	}
	return _field(offset >> scale, 12) << 10;
}

static void _ldrW(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0xB9400000 | _scaled(offset, 2) | (rn << 5) | rt);
}

static void _strW(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0xB9000000 | _scaled(offset, 2) | (rn << 5) | rt);
}

static void _ldrbW(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0x39400000 | _scaled(offset, 0) | (rn << 5) | rt);
}

static void _ldrhW(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0x79400000 | _scaled(offset, 1) | (rn << 5) | rt);
}

static void _strbW(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0x39000000 | _scaled(offset, 0) | (rn << 5) | rt);
}

static void _ldrX(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0xF9400000 | _scaled(offset, 3) | (rn << 5) | rt);
}

static void _strX(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0xF9000000 | _scaled(offset, 3) | (rn << 5) | rt);
}

static void _stpX(struct Emitter* e, int rt, int rt2, int offset) {
	_emit(e, 0xA9000000 | (((offset / 8) & 0x7F) << 15) | (rt2 << 10) | (31 << 5) | rt);
}

static void _ldpX(struct Emitter* e, int rt, int rt2, int offset) {
	_emit(e, 0xA9400000 | (((offset / 8) & 0x7F) << 15) | (rt2 << 10) | (31 << 5) | rt);
}

// stp rt, rt2, [sp, #-bytes]! and ldp rt, rt2, [sp], #bytes
static void _pushPair(struct Emitter* e, int rt, int rt2, int bytes) {
	_emit(e, 0xA9800000 | (((-bytes / 8) & 0x7F) << 15) | (rt2 << 10) | (31 << 5) | rt);
}

static void _popPair(struct Emitter* e, int rt, int rt2, int bytes) {
	_emit(e, 0xA8C00000 | ((bytes / 8) << 15) | (rt2 << 10) | (31 << 5) | rt);
}

static void _addWImm(struct Emitter* e, int rd, int rn, unsigned imm) {
	_dpImm(e, A64_ADD_IMM, rd, rn, imm);
}

static void _addXImm(struct Emitter* e, int rd, int rn, unsigned imm) {
	_dpImm(e, A64_ADD_IMM | A64_X, rd, rn, imm);
}

static void _addW(struct Emitter* e, int rd, int rn, int rm) {
	_dp(e, A64_ADD, rd, rn, rm, 0);
}

static void _cmpW(struct Emitter* e, int rn, int rm) {
	_dp(e, A64_SUBS, A64_ZR, rn, rm, 0);
}

static void _cmpWImm(struct Emitter* e, int rn, unsigned imm) {
	_dpImm(e, A64_SUBS_IMM, A64_ZR, rn, imm);
}

static void _movW(struct Emitter* e, int rd, int rm) {
	_dp(e, A64_ORR, rd, A64_ZR, rm, 0);
}

static void _movX(struct Emitter* e, int rd, int rm) {
	_dp(e, A64_ORR | A64_X, rd, A64_ZR, rm, 0);
}

static void _negW(struct Emitter* e, int rd, int rm) {
	_dp(e, A64_SUB, rd, A64_ZR, rm, 0);
}

// AND with a contiguous run of ones: len bits starting at lsb
static void _andImm(struct Emitter* e, int rd, int rn, unsigned lsb, unsigned len) {
	_emit(e, 0x12000000 | (((32 - lsb) & 31) << 16) | ((len - 1) << 10) | (rn << 5) | rd);
}

static void _ubfx(struct Emitter* e, int rd, int rn, unsigned lsb, unsigned width) {
	_emit(e, 0x53000000 | (lsb << 16) | ((lsb + width - 1) << 10) | (rn << 5) | rd);
}

static void _lslWImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	_emit(e, 0x53000000 | (((32 - shift) & 31) << 16) | ((31 - shift) << 10) | (rn << 5) | rd);
}

static void _lsrWImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	_emit(e, 0x53007C00 | (shift << 16) | (rn << 5) | rd);
}

static void _asrWImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	_emit(e, 0x13007C00 | (shift << 16) | (rn << 5) | rd);
}

static void _rorWImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	_emit(e, 0x13800000 | (rn << 16) | (shift << 10) | (rn << 5) | rd);
}

static void _sxtb(struct Emitter* e, int rd, int rn) {
	_emit(e, 0x13001C00 | (rn << 5) | rd);
}

static void _sxth(struct Emitter* e, int rd, int rn) {
	_emit(e, 0x13003C00 | (rn << 5) | rd);
}

static void _madd(struct Emitter* e, int rd, int rn, int rm, int ra) {
	_emit(e, 0x1B000000 | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}

static void _clzW(struct Emitter* e, int rd, int rn) {
	_emit(e, 0x5AC01000 | (rn << 5) | rd);
}

static void _csel(struct Emitter* e, int rd, int rn, int rm, int cond) {
	_emit(e, 0x1A800000 | (rm << 16) | (cond << 12) | (rn << 5) | rd);
}

// rd = cond ? rn + 1 : rn, and rd = cond ? 1 : 0
static void _mrsNzcv(struct Emitter* e, int rt) {
	_emit(e, 0xD53B4200 | rt);
}

static void _msrNzcv(struct Emitter* e, int rt) {
	_emit(e, 0xD51B4200 | rt);
}

static void _blr(struct Emitter* e, int rn) {
	_emit(e, 0xD63F0000 | (rn << 5));
}

static void _br(struct Emitter* e, int rn) {
	_emit(e, 0xD61F0000 | (rn << 5));
}

static void _ret(struct Emitter* e, int rn) {
	_emit(e, 0xD65F0000 | (rn << 5));
}

static uint8_t* _bCond(struct Emitter* e, int cond) {
	return _emitSite(e, 0x54000000 | cond);
}

static uint8_t* _cbzW(struct Emitter* e, int rt) {
	return _emitSite(e, 0x34000000 | rt);
}

static uint8_t* _cbnzW(struct Emitter* e, int rt) {
	return _emitSite(e, 0x35000000 | rt);
}

static uint8_t* _cbzX(struct Emitter* e, int rt) {
	return _emitSite(e, 0xB4000000 | rt);
}

static uint8_t* _cbnzX(struct Emitter* e, int rt) {
	return _emitSite(e, 0xB5000000 | rt);
}

static uint8_t* _tbz(struct Emitter* e, int rt, unsigned bit) {
	return _emitSite(e, 0x36000000 | (bit << 19) | rt);
}

static uint8_t* _tbnz(struct Emitter* e, int rt, unsigned bit) {
	return _emitSite(e, 0x37000000 | (bit << 19) | rt);
}

static uint8_t* _b(struct Emitter* e) {
	return _emitSite(e, 0x14000000);
}

static uint8_t* _bl(struct Emitter* e) {
	return _emitSite(e, 0x94000000);
}

// C sees and may change the cycle count and the next event
static void _blrC(struct Emitter* e, int rn) {
	_strW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	_blr(e, rn);
	_ldrW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	_ldrW(e, R_NEXT, R_CPU, OFF_NEXT_EVENT);
}

// The flags register from CPSR, and back; generated code runs only while CPSR bits 24 to 27 are clear, so the top byte is the flags
static void _loadFlagsRegister(struct Emitter* e) {
	_ldrW(e, R_FLAGS, R_CPU, OFF_CPSR);
	_andImm(e, R_FLAGS, R_FLAGS, 28, 4);
}

static void _storeFlagsRegister(struct Emitter* e) {
	_lsrWImm(e, 9, R_FLAGS, 24);
	_strbW(e, 9, R_CPU, OFF_CPSR + 3);
}

// Calls a shared routine, which returns here
static void _callRoutine(struct Emitter* e, const uint8_t* routine) {
	_patch(_bl(e), routine);
}

// A routine calling C for generated code: the function, or the one in x16 when NULL. C sees the flags in CPSR and generated code takes them back.
// An instruction handler can leave CPSR bits 24 to 27 set, and generated code then hands over to C.
static uint8_t* _emitCallRoutine(struct Emitter* e, const void* function, const uint8_t* toCDirect) {
	uint8_t* routine = e->p;
	_strX(e, 30, A64_SP, LR_SLOT);
	_storeFlagsRegister(e);
	if (function) {
		_movImm64(e, 16, (uintptr_t) function);
	}
	_blrC(e, 16);
	_ldrW(e, 9, R_CPU, OFF_CPSR);
	_andImm(e, R_FLAGS, 9, 28, 4);
	if (toCDirect) {
		_logicImm(e, A64_ANDS_IMM, A64_ZR, 9, 0x0F000000);
		_patch(_bCond(e, A64_NE), toCDirect);
	}
	_ldrX(e, 30, A64_SP, LR_SLOT);
	_ret(e, 30);
	return routine;
}

// Runs the due events; Z is set when the frame is done
static void _callEvents(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_movX(e, 0, R_CPU);
	_callRoutine(e, c->jit->callEvents);
	_logicImm(e, A64_ANDS_IMM, A64_ZR, 0, 0xFF);
}

static void _epilogueDirect(struct Compiler* c);

static void _prologue(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_pushPair(e, 29, 30, FRAME_SIZE);
	_stpX(e, 19, 20, 16);
	_stpX(e, 21, 22, 32);
	_stpX(e, 23, 24, 48);
	_stpX(e, 25, 26, 64);
	_stpX(e, 27, 28, 80);
	_movX(e, R_CPU, 0);
	if (c->gba) {
		_movImm64(e, R_JIT, (uintptr_t) c->jit);
		_movImm64(e, R_IWRAM, (uintptr_t) c->gba->memory.iwram);
		_movImm64(e, R_EWRAM, (uintptr_t) c->gba->memory.wram);
		_movImm64(e, R_COVER, (uintptr_t) c->jit->cover);
		_movImm64(e, R_GBA, (uintptr_t) c->gba);
	}
	_ldrW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	_ldrW(e, R_NEXT, R_CPU, OFF_NEXT_EVENT);
	_loadFlagsRegister(e);
}

static void _epilogue(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_storeFlagsRegister(e);
	_epilogueDirect(c);
}

static void _epilogueDirect(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_strW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	_ldpX(e, 27, 28, 80);
	_ldpX(e, 25, 26, 64);
	_ldpX(e, 23, 24, 48);
	_ldpX(e, 21, 22, 32);
	_ldpX(e, 19, 20, 16);
	_popPair(e, 29, 30, FRAME_SIZE);
	_ret(e, 30);
}

static void _jumpTo(struct Compiler* c, const uint8_t* target) {
	_patch(_b(&c->e), target);
}

static void _patchLink(uint8_t* site, const uint8_t* target) {
	uint32_t insn = 0x14000000 | (((target - site) / 4) & 0x3FFFFFF);
	memcpy(site, &insn, 4);
	__builtin___clear_cache((char*) site, (char*) site + 4);
}

// Entering at ops[index]: take patched words from the pipeline
static void _entryStub(struct Compiler* c, unsigned index, const uint8_t* target) {
	struct Emitter* e = &c->e;
	unsigned i;
	for (i = 0; i < 2; ++i) {
		if (c->patched[index + i]) {
			_ldrW(e, 0, R_CPU, i ? OFF_PREFETCH1 : OFF_PREFETCH0);
			_strW(e, 0, R_JIT, JIT_FETCHED + 4 * (index + i));
		}
	}
	_jumpTo(c, target);
}

static void _linkJump(struct Compiler* c) {
	struct ARMJitLink* link = &c->block->links[c->nLinks++];
	link->site = _b(&c->e);
}

static void _linkStub(struct Compiler* c, struct ARMJitLink* link) {
	_movImm64(&c->e, 2, (uintptr_t) link);
	_jumpTo(c, c->jit->linkDispatch);
}

// Starts generated code at the address in x1
static void _emitEnter(struct Compiler* c) {
	struct Emitter* e = &c->e;
	c->jit->enter = (void (*)(struct ARMCore*, void*)) e->p;
	_prologue(c);
	_br(e, 1);
}

// The block for the PC and mode in the CPU state, if it is compiled and nothing is due; otherwise back to C
static void _emitLookup(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_strbW(e, A64_ZR, R_JIT, JIT_BLOCKS_DROPPED);
	_ldrW(e, R_NEXT, R_CPU, OFF_NEXT_EVENT);
	_cmpW(e, R_CYCLES, R_NEXT);
	_patch(_bCond(e, A64_GE), c->jit->events);
	_ldrW(e, 2, R_CPU, OFF_EXECUTION_MODE);
	_ldrW(e, 3, R_CPU, OFF_PC);
	_dpImm(e, A64_SUB_IMM, 3, 3, 4);
	_dp(e, A64_ADD, 3, 3, 2, 1);
	_ubfx(e, 4, 3, 12, 16);
	_addXImm(e, 5, R_JIT, JIT_PAGES);
	_ldstR(e, A64_LDRX_R | A64_LSL | A64_SCALED, 5, 5, 4);
	_patch(_cbzX(e, 5), c->jit->toC);
	_ubfx(e, 6, 3, 1, 11);
	_ldstR(e, A64_LDRX_R | A64_LSL | A64_SCALED, 5, 5, 6);
	_patch(_cbzX(e, 5), c->jit->toC);
	_ldrW(e, 7, 5, ENTRY_PC);
	_cmpW(e, 7, 3);
	_patch(_bCond(e, A64_NE), c->jit->toC);
	_ldrbW(e, 7, 5, ENTRY_THUMB);
	_cmpW(e, 7, 2);
	_patch(_bCond(e, A64_NE), c->jit->toC);
	unsigned k;
	for (k = 0; k < 2; ++k) {
		_ldrW(e, 7, R_CPU, k ? OFF_PREFETCH1 : OFF_PREFETCH0);
		_ldrW(e, 8, 5, k ? ENTRY_OP1 : ENTRY_OP0);
		_dp(e, A64_EOR, 7, 7, 8, 0);
		_ldrW(e, 8, 5, k ? ENTRY_OP_MASK1 : ENTRY_OP_MASK0);
		_dp(e, A64_ANDS, A64_ZR, 7, 8, 0);
		_patch(_bCond(e, A64_NE), c->jit->toC);
	}
	// A pending link gets patched to jump straight to the block
	_ldrX(e, 1, R_JIT, JIT_PENDING_LINK);
	uint8_t* noLink = _cbzX(e, 1);
	_movX(e, R_WB, 5);
	_movX(e, 0, R_JIT);
	_movX(e, 2, 5);
	_movImm64(e, 16, (uintptr_t) ARMJitLink);
	_blr(e, 16);
	_movX(e, 5, R_WB);
	_patch(noLink, e->p);
	_ldrX(e, 16, 5, ENTRY_CODE);
	_br(e, 16);
}

// Runs a due exit's events, then returns to the block unless the PC moved, its code changed or the frame ended
static void _emitResume(struct Compiler* c) {
	struct Emitter* e = &c->e;
	c->jit->resume = e->p;
	_ldrW(e, R_WB, R_CPU, OFF_PC);
	_movX(e, R_RETURN, 30);
	_callEvents(c);
	_patch(_bCond(e, A64_EQ), c->jit->toC);
	_ldrW(e, 0, R_CPU, OFF_PC);
	_cmpW(e, 0, R_WB);
	_patch(_bCond(e, A64_NE), c->jit->dispatch);
	_ldrbW(e, 0, R_JIT, JIT_BLOCKS_DROPPED);
	_patch(_cbnzW(e, 0), c->jit->dispatch);
	_ret(e, R_RETURN);
}

// The routines all generated code shares
static void _emitMemRoutines(struct Compiler* c);
static void _emitBranchRoutines(struct Compiler* c);
static void _emitMultiRoutines(struct Compiler* c);
static void _emitExitRoutines(struct Compiler* c);

static void _emitTrampoline(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_emitEnter(c);
	c->jit->toC = e->p;
	_storeFlagsRegister(e);
	c->jit->toCDirect = e->p;
	_epilogueDirect(c);
	c->jit->callC = _emitCallRoutine(e, NULL, NULL);
	c->jit->callHandler = _emitCallRoutine(e, NULL, c->jit->toCDirect);
	c->jit->callEvents = _emitCallRoutine(e, ARMJitEvents, NULL);
	c->jit->callInvalidate = _emitCallRoutine(e, ARMJitInvalidateWord, NULL);
	c->jit->callReadTimer = _emitCallRoutine(e, _readTimer, NULL);
	c->jit->callVramWait = _emitCallRoutine(e, GBAMemoryVRAMWait, NULL);
	c->jit->callRomStall = _emitCallRoutine(e, _romStallC, NULL);
	_emitMemRoutines(c);
	_emitBranchRoutines(c);
	_emitMultiRoutines(c);

	// A link stub passes its link in x2 so the exit gets patched to the block found
	c->jit->linkDispatch = e->p;
	_strX(e, 2, R_JIT, JIT_PENDING_LINK);
	uint8_t* toLookup = _b(e);

	// Due events run here; the frame loop decides whether to come back
	c->jit->events = e->p;
	_callEvents(c);
	_patch(_bCond(e, A64_EQ), c->jit->toC);

	// Next block for the PC and mode in the CPU state, if it is compiled and nothing is due
	c->jit->dispatch = e->p;
	_strX(e, A64_ZR, R_JIT, JIT_PENDING_LINK);
	_patch(toLookup, e->p);
	_emitLookup(c);
	_emitResume(c);
	_emitExitRoutines(c);
}

static void _exitJump(struct Compiler* c, int index) {
	_exitAt(c, _b(&c->e), index);
}

static void _storePrefetch(struct Compiler* c, unsigned offset, unsigned index) {
	struct Emitter* e = &c->e;
	if (c->patched[index]) {
		_ldrW(e, 0, R_JIT, JIT_FETCHED + 4 * index);
	} else {
		_movImm32(e, 0, c->ops[index]);
	}
	_strW(e, 0, R_CPU, offset);
}

// Interpreter state just before ops[index] runs
static void _storeState(struct Compiler* c, unsigned index) {
	struct Emitter* e = &c->e;
	_movImm32(e, 0, c->pc + c->width * (index + 1));
	_strW(e, 0, R_CPU, OFF_PC);
	_storePrefetch(c, OFF_PREFETCH0, index);
	_storePrefetch(c, OFF_PREFETCH1, index + 1);
}

// A word or halfword of IWRAM or EWRAM, through their base registers
static void _ldrRam(struct Compiler* c, int rt, uint32_t address, bool halfword) {
	struct Emitter* e = &c->e;
	int base = (address >> 24) == GBA_REGION_IWRAM ? R_IWRAM : R_EWRAM;
	uint32_t offset = address & (base == R_IWRAM ? GBA_SIZE_IWRAM - 1 : GBA_SIZE_EWRAM - 1);
	unsigned scale = halfword ? 2 : 4;
	if (offset < 0x1000 * scale && !(offset & (scale - 1))) {
		(halfword ? _ldrhW : _ldrW)(e, rt, base, offset);
	} else {
		_movImm32(e, rt, offset);
		_ldstR(e, (halfword ? A64_LDRH_R : A64_LDR_R) | A64_UXTW, rt, base, rt);
	}
}

// Copy a patched word as it is now into jit->fetched
static void _fetchWord(struct Compiler* c, unsigned index) {
	struct Emitter* e = &c->e;
	if (index > c->count + 1 || !c->patched[index]) {
		return;
	}
	_ldrRam(c, 0, c->pc + c->width * index, c->thumb);
	_strW(e, 0, R_JIT, JIT_FETCHED + 4 * index);
}

// A patched word two instructions ahead, when the GBA's pipeline would fetch it
static void _fetchAhead(struct Compiler* c, unsigned i) {
	_fetchWord(c, i + 2);
}

static void _addCyclesReg(struct Compiler* c, int reg, uint32_t constant) {
	struct Emitter* e = &c->e;
	if (reg >= 0) {
		_addW(e, R_CYCLES, R_CYCLES, reg);
	}
	if (constant) {
		_addWImm(e, R_CYCLES, R_CYCLES, constant);
	}
}

static void _addCycles(struct Compiler* c, uint32_t constant) {
	_addCyclesReg(c, -1, constant);
}

static void _eventCheck(struct Compiler* c, unsigned index) {
	struct Emitter* e = &c->e;
	_cmpW(e, R_CYCLES, R_NEXT);
	_exitAt(c, _bCond(e, A64_GE), index | EXIT_DUE);
}

// A conditional instruction that does not run still takes its fetch
static void _skipped(struct Compiler* c, uint8_t* fail) {
	struct Emitter* e = &c->e;
	if (fail) {
		uint8_t* after = _b(e);
		_patch(fail, e->p);
		_addCycles(c, c->aluCycles);
		_patch(after, e->p);
	}
}

// Jumps to the returned site when an event comes due before the last instruction of a run
static uint8_t* _runCheck(struct Compiler* c, uint32_t cycles) {
	struct Emitter* e = &c->e;
	_addWImm(e, 0, R_CYCLES, cycles);
	_cmpW(e, 0, R_NEXT);
	return _bCond(e, A64_GE);
}

static void _smcCheck(struct Compiler* c, int index) {
	struct Emitter* e = &c->e;
	_ldrbW(e, 0, R_JIT, JIT_BLOCKS_DROPPED);
	_exitAt(c, _cbnzW(e, 0), index);
}

static void _loadSource(struct Compiler* c, int rt, struct Source source) {
	if (source.constant) {
		_movImm32(&c->e, rt, source.value);
	} else {
		_ldrW(&c->e, rt, R_CPU, 4 * source.reg);
	}
}

// Registers no ALU code uses, for guest values within an ALU run
static const uint8_t _cacheRegs[] = { 5, 6, 7, 8, 11, 12, 13, 14 };

// A register for a guest value, taking the one least recently used, but none the current instruction uses
static int _cacheTake(struct Compiler* c, unsigned reg) {
	unsigned best = 0;
	unsigned k;
	for (k = 1; k < sizeof(_cacheRegs); ++k) {
		if (c->slotUsed[k] < c->slotUsed[best]) {
			best = k;
		}
	}
	int host = _cacheRegs[best];
	unsigned r;
	for (r = 0; r < 16; ++r) {
		if (c->cached[r] == host) {
			c->cached[r] = 0;
		}
	}
	c->cached[reg] = host;
	c->slotUsed[best] = c->useClock;
	return host;
}

static void _cacheTouch(struct Compiler* c, int host) {
	unsigned k;
	for (k = 0; k < sizeof(_cacheRegs); ++k) {
		if (_cacheRegs[k] == host) {
			c->slotUsed[k] = c->useClock;
		}
	}
}

// The register holding a source: its run copy, or scratch; code that may be skipped takes no new copy
static int _sourceReg(struct Compiler* c, int scratch, struct Source source, bool keep) {
	if (source.constant) {
		_movImm32(&c->e, scratch, source.value);
		return scratch;
	}
	int host = c->cached[source.reg];
	if (host) {
		_cacheTouch(c, host);
		return host;
	}
	if (keep) {
		host = _cacheTake(c, source.reg);
		_ldrW(&c->e, host, R_CPU, 4 * source.reg);
		return host;
	}
	_ldrW(&c->e, scratch, R_CPU, 4 * source.reg);
	return scratch;
}

// Keeps a result for later instructions in the run; when skippable, only a copy that already exists
static void _cacheResult(struct Compiler* c, unsigned reg, int value, bool keep) {
	int host = c->cached[reg];
	if (!host && keep) {
		host = _cacheTake(c, reg);
	}
	if (host) {
		_movW(&c->e, host, value);
	}
}

// Host NZCV = guest NZCV
static void _loadFlags(struct Compiler* c) {
	_msrNzcv(&c->e, R_FLAGS);
}

static void _loadCarry(struct Compiler* c, int rd) {
	_ubfx(&c->e, rd, R_FLAGS, 29, 1);
}

// Barrel shifter for an immediate shift amount on a source register: value in w1, carry out in w2 if wanted
static void _shiftImm(struct Compiler* c, int source, unsigned type, unsigned amount, bool carry) {
	struct Emitter* e = &c->e;
	switch (type) {
	case SHIFT_LSL:
		if (!amount) {
			_movW(e, 1, source);
			if (carry) {
				_loadCarry(c, 2);
			}
		} else {
			_lslWImm(e, 1, source, amount);
			if (carry) {
				_ubfx(e, 2, source, 32 - amount, 1);
			}
		}
		break;
	case SHIFT_LSR:
		if (!amount) {
			_movImm32(e, 1, 0);
			if (carry) {
				_lsrWImm(e, 2, source, 31);
			}
		} else {
			_lsrWImm(e, 1, source, amount);
			if (carry) {
				_ubfx(e, 2, source, amount - 1, 1);
			}
		}
		break;
	case SHIFT_ASR:
		if (!amount) {
			_asrWImm(e, 1, source, 31);
			if (carry) {
				_lsrWImm(e, 2, source, 31);
			}
		} else {
			_asrWImm(e, 1, source, amount);
			if (carry) {
				_ubfx(e, 2, source, amount - 1, 1);
			}
		}
		break;
	case SHIFT_ROR:
		if (!amount) {
			_loadCarry(c, 2);
			_lsrWImm(e, 1, source, 1);
			_dp(e, A64_ORR, 1, 1, 2, 31);
			if (carry) {
				_andImm(e, 2, source, 0, 1);
			}
		} else {
			_rorWImm(e, 1, source, amount);
			if (carry) {
				_ubfx(e, 2, source, amount - 1, 1);
			}
		}
		break;
	}
}

// The host instruction for w3 = w0 op imm, or 0 when the immediate does not fit one
static uint32_t _aluImm(unsigned opcode, bool s, uint32_t imm) {
	uint32_t fields;
	switch (opcode) {
	case ALU_ADD:
	case ALU_CMN:
	case ALU_SUB:
	case ALU_CMP: {
		bool sub = opcode == ALU_SUB || opcode == ALU_CMP;
		uint32_t op = s ? (sub ? A64_SUBS_IMM : A64_ADDS_IMM) : (sub ? A64_SUB_IMM : A64_ADD_IMM);
		if (imm < 0x1000) {
			return op | (imm << 10) | 3;
		}
		if (!(imm & 0xFFF) && imm < 0x1000000) {
			return op | (1 << 22) | ((imm >> 12) << 10) | 3;
		}
		return 0;
	}
	case ALU_AND:
	case ALU_TST:
		return _bitmaskImm(imm, &fields) ? A64_AND_IMM | fields | 3 : 0;
	case ALU_EOR:
	case ALU_TEQ:
		return _bitmaskImm(imm, &fields) ? A64_EOR_IMM | fields | 3 : 0;
	case ALU_ORR:
		return _bitmaskImm(imm, &fields) ? A64_ORR_IMM | fields | 3 : 0;
	case ALU_BIC:
		return _bitmaskImm(~imm, &fields) ? A64_AND_IMM | fields | 3 : 0;
	default:
		return 0;
	}
}

// The flags register with N and Z from the result and, of C and V, only the bits in keep
static void _setNZ(struct Emitter* e, int result, uint32_t keep) {
	_dp(e, A64_ANDS, A64_ZR, result, result, 0);
	_mrsNzcv(e, 9);
	_logicImm(e, A64_AND_IMM, R_FLAGS, R_FLAGS, keep);
	_dp(e, A64_ORR, R_FLAGS, R_FLAGS, 9, 0);
}

// storeCarry: MULS and MLAS take C from the last shifter result, so keep it for them
static void _emitAlu(struct Compiler* c, const struct AluOp* alu, bool storeCarry) {
	struct Emitter* e = &c->e;
	unsigned opcode = alu->opcode;
	bool s = alu->s;
	bool logical = _isLogical(opcode);
	bool carryIn = opcode == ALU_ADC || opcode == ALU_SBC || opcode == ALU_RSC;
	bool setFlags = s && alu->flagsLive;
	bool carryOut = (setFlags && logical) || storeCarry;
	// Copies are kept only in runs, for values read again: other ALU code, like the forms of a patched word, has no single path through it
	bool keep = alu->cond == 0xE && c->inRun;
	bool writesRd = opcode < ALU_TST || opcode > ALU_CMN;
	++c->useClock;

	uint8_t* skip = NULL;
	if (alu->cond != 0xE) {
		_loadFlags(c);
		skip = _bCond(e, alu->cond ^ 1);
	}

	// Both registers load before either is used, so the in-order core waits on one load at most
	int rm = 3;
	int rn = 0;
	if (!alu->immediate) {
		rm = _sourceReg(c, 3, alu->m, keep && (alu->readLater >> alu->m.reg & 1) && !(writesRd && alu->rd == alu->m.reg));
	}
	if (opcode != ALU_MOV && opcode != ALU_MVN) {
		rn = _sourceReg(c, 0, alu->n, keep && (alu->readLater >> alu->n.reg & 1) && !(writesRd && alu->rd == alu->n.reg));
	}
	// Immediates go into the host instruction when they fit
	uint32_t hostImm = alu->immediate ? _aluImm(opcode, s, alu->imm) : 0;
	if (alu->immediate) {
		if (opcode == ALU_MOV || opcode == ALU_MVN) {
			_movImm32(e, 3, opcode == ALU_MOV ? alu->imm : ~alu->imm);
		} else if (!hostImm) {
			_movImm32(e, 1, alu->imm);
		}
		if (carryOut) {
			if (alu->immCarry >= 0) {
				_movImm32(e, 2, alu->immCarry);
			} else {
				_loadCarry(c, 2);
			}
		}
	} else {
		_shiftImm(c, rm, alu->shiftType, alu->shiftAmount, carryOut);
	}
	if (carryIn) {
		_loadFlags(c);
	}

	// A kept result goes straight into its run copy; a register MOV leaves its value where the shifter put it
	bool keepResult = writesRd && keep && (alu->readLater >> alu->rd & 1);
	int d = 3;
	if (keepResult && !(opcode == ALU_MOV && !alu->immediate)) {
		d = c->cached[alu->rd] ? c->cached[alu->rd] : _cacheTake(c, alu->rd);
	}
	if (alu->immediate && (opcode == ALU_MOV || opcode == ALU_MVN) && d != 3) {
		_movW(e, d, 3);
	}
	int result = d;
	if (hostImm) {
		_emit(e, (hostImm & ~0x1F) | d | (rn << 5));
	} else switch (opcode) {
	case ALU_AND:
	case ALU_TST:
		_dp(e, A64_AND, d, rn, 1, 0);
		break;
	case ALU_EOR:
	case ALU_TEQ:
		_dp(e, A64_EOR, d, rn, 1, 0);
		break;
	case ALU_ORR:
		_dp(e, A64_ORR, d, rn, 1, 0);
		break;
	case ALU_BIC:
		_dp(e, A64_BIC, d, rn, 1, 0);
		break;
	case ALU_MOV:
		if (!alu->immediate) {
			result = 1;
		}
		break;
	case ALU_MVN:
		if (!alu->immediate) {
			_dp(e, A64_ORN, d, A64_ZR, 1, 0);
		}
		break;
	case ALU_ADD:
	case ALU_CMN:
		_dp(e, s ? A64_ADDS : A64_ADD, d, rn, 1, 0);
		break;
	case ALU_SUB:
	case ALU_CMP:
		_dp(e, s ? A64_SUBS : A64_SUB, d, rn, 1, 0);
		break;
	case ALU_RSB:
		_dp(e, s ? A64_SUBS : A64_SUB, d, 1, rn, 0);
		break;
	case ALU_ADC:
		_dp(e, s ? A64_ADCS : A64_ADC, d, rn, 1, 0);
		break;
	case ALU_SBC:
		_dp(e, s ? A64_SBCS : A64_SBC, d, rn, 1, 0);
		break;
	case ALU_RSC:
		_dp(e, s ? A64_SBCS : A64_SBC, d, 1, rn, 0);
		break;
	}

	if (setFlags) {
		if (logical) {
			// N and Z from the result, C from the shifter, V kept
			_setNZ(e, result, 0x10000000);
			_dp(e, A64_ORR, R_FLAGS, R_FLAGS, 2, 29);
		} else {
			_mrsNzcv(e, R_FLAGS);
		}
	}
	if (writesRd) {
		_strW(e, result, R_CPU, 4 * alu->rd);
		if (result != c->cached[alu->rd] || !result) {
			_cacheResult(c, alu->rd, result, keepResult);
		}
	}
	if (storeCarry) {
		_strW(e, 2, R_CPU, OFF_SHIFTER_CARRY);
	}
	if (skip) {
		_patch(skip, e->p);
	}
}

// Access at [base + w8]; loads land in w0, rotated like the GBA by the address in w4; stores take w6
static void _memAccess(struct Emitter* e, bool load, unsigned size, int base) {
	static const uint32_t loads[] = { 0, A64_LDRB_R, A64_LDRH_R, 0, A64_LDR_R };
	static const uint32_t stores[] = { 0, A64_STRB_R, A64_STRH_R, 0, A64_STR_R };
	if (load) {
		_ldstR(e, loads[size] | A64_UXTW, 0, base, 8);
		if (size > 1) {
			_andImm(e, 9, 4, 0, size == 4 ? 2 : 1);
			_lslWImm(e, 9, 9, 3);
			_dp(e, A64_RORV, 0, 0, 9, 0);
		}
	} else {
		_ldstR(e, stores[size] | A64_UXTW, 6, base, 8);
	}
}

// w3 = what GBAMemoryStall makes of a data access wait from cartridge code with prefetch on
static void _romStall(struct Compiler* c, unsigned i, int32_t wait) {
	struct Emitter* e = &c->e;
	int32_t s = c->seq16;
	int32_t n = c->nonseq16;
	uint32_t pc = c->pc + c->width * (i + 2);
	uint64_t advances;
	uint64_t stalls;
	_stallTables(s, wait, &advances, &stalls);
	_ldrW(e, 10, R_GBA, offsetof(struct GBA, memory.lastPrefetchedPc));
	int32_t adjust = wait - (n - s);
	if (advances == 0x0706050403020100ULL && stalls == (uint8_t) stalls * 0x0101010101010101ULL) {
		// Short waits: the prefetch moves on by the loads it had done, and the stall is one value
		_movImm32(e, 11, pc);
		_dp(e, A64_SUB, 10, 10, 11, 0);
		_ubfx(e, 12, 10, 1, 3);
		_cmpWImm(e, 10, 16);
		_csel(e, 12, 12, A64_ZR, A64_LO);
		_dp(e, A64_ADD, 13, 11, 12, 1);
		_strW(e, 13, R_GBA, offsetof(struct GBA, memory.lastPrefetchedPc));
		_movImm32(e, 3, (int8_t) stalls + adjust);
		return;
	}
	// The tables load, or are built, while the load is in flight
	if (s >= 0 && s < ARM_JIT_STALL_SEQ && wait >= 0 && wait < ARM_JIT_STALL_WAIT) {
		_ldrX(e, 13, R_JIT, JIT_STALL_TABLES + 8 * (2 * (s * ARM_JIT_STALL_WAIT + wait)));
		_ldrX(e, 14, R_JIT, JIT_STALL_TABLES + 8 * (2 * (s * ARM_JIT_STALL_WAIT + wait) + 1));
	} else {
		_movImm64(e, 13, advances);
		_movImm64(e, 14, stalls);
	}
	_movImm32(e, 11, pc);
	_dp(e, A64_SUB, 10, 10, 11, 0);
	// w12 = 8 * previous loads, or 0 when the last prefetch is not 0 to 14 bytes ahead
	_ubfx(e, 12, 10, 1, 3);
	_lslWImm(e, 12, 12, 3);
	_cmpWImm(e, 10, 16);
	_csel(e, 12, 12, A64_ZR, A64_LO);
	// lastPrefetchedPc = pc + 2 * (loads + previous - 1)
	_dp(e, A64_LSRV | A64_X, 13, 13, 12, 0);
	_andImm(e, 13, 13, 0, 8);
	_dp(e, A64_ADD, 13, 11, 13, 1);
	_strW(e, 13, R_GBA, offsetof(struct GBA, memory.lastPrefetchedPc));
	// max(wait, stall) - stall - (n - s), as wait - (n - s) - min(wait, stall)
	_dp(e, A64_LSRV | A64_X, 14, 14, 12, 0);
	_sxtb(e, 14, 14);
	_dpImm(e, adjust >= 0 ? A64_ADD_IMM : A64_SUB_IMM, 3, 14, adjust >= 0 ? adjust : -adjust);
}

// w3 = a data access wait, through GBAMemoryStall when cartridge code runs with prefetch on
static void _dataWait(struct Compiler* c, unsigned i, int32_t wait) {
	if (c->romCode && c->prefetch) {
		_romStall(c, i, wait);
	} else {
		_movImm32(&c->e, 3, wait);
	}
}

static void _memResult(struct Compiler* c, const struct MemOp* mem) {
	struct Emitter* e = &c->e;
	if (mem->signExtend) {
		uint8_t* halfword = NULL;
		if (mem->size == 2) {
			// LDRSH of an odd address sign-extends the rotated byte
			_ldrW(e, 9, A64_SP, ADDRESS_SLOT);
			halfword = _tbz(e, 9, 0);
		}
		_sxtb(e, 0, 0);
		if (halfword) {
			uint8_t* extended = _b(e);
			_patch(halfword, e->p);
			_sxth(e, 0, 0);
			_patch(extended, e->p);
		}
	}
	if (mem->load) {
		_strW(e, 0, R_CPU, 4 * mem->rd);
	} else if (mem->writeback) {
		_strW(e, R_WB, R_CPU, 4 * mem->base.reg);
	}
	_addCyclesReg(c, 3, c->memCycles);
}

// Video status, which GBAIORead only reads after clearing the idle loop flag
static void _memStatus(struct Compiler* c, const struct MemOp* mem) {
	struct Emitter* e = &c->e;
	_movImm32(e, 10, offsetof(struct GBA, haltPending));
	_ldstR(e, A64_STRB_R | A64_UXTW, A64_ZR, R_GBA, 10);
	_andImm(e, 8, 4, 0, 3);
	_addXImm(e, 10, R_GBA, offsetof(struct GBA, memory.io));
	_memAccess(e, true, mem->size, 10);
}

// I/O reads without the generic load and I/O dispatch: DISPSTAT and VCOUNT, then timer counters
static void _memIo(struct Compiler* c, unsigned i, const struct MemOp* mem, uint8_t** miss, uint8_t** slow) {
	struct Emitter* e = &c->e;
	_cmpWImm(e, 7, GBA_REGION_IO);
	*miss = _bCond(e, A64_NE);
	_movImm32(e, 9, mem->size == 2 ? 0x00FFFFFD : 0x00FFFFFC);
	_dp(e, A64_AND, 9, 4, 9, 0);
	_cmpWImm(e, 9, GBA_REG_DISPSTAT);
	if (mem->size == 1) {
		*slow = _bCond(e, A64_NE);
		_memStatus(c, mem);
	} else {
		uint8_t* timer = _bCond(e, A64_NE);
		_memStatus(c, mem);
		uint8_t* done = _b(e);
		_patch(timer, e->p);
		_movImm32(e, 9, 0x00FFFFF3);
		_dp(e, A64_AND, 9, 4, 9, 0);
		_cmpWImm(e, 9, GBA_REG_TM0CNT_LO);
		*slow = _bCond(e, A64_NE);
		_movX(e, 0, R_GBA);
		_movW(e, 1, 4);
		_callRoutine(e, c->jit->callReadTimer);
		_patch(done, e->p);
	}
	_dataWait(c, i, 2);
}

// VRAM: stores tell the renderer about changed halfwords; byte stores stay with the handlers
static void _memVram(struct Compiler* c, const struct MemOp* mem, uint8_t** miss, uint8_t** slow) {
	struct Emitter* e = &c->e;
	bool load = mem->load;
	unsigned size = mem->size;
	_cmpWImm(e, 7, GBA_REGION_VRAM);
	*miss = _bCond(e, A64_NE);
	_andImm(e, 8, 4, 0, 17);
	_movImm32(e, 9, GBA_SIZE_VRAM);
	_cmpW(e, 8, 9);
	*slow = _bCond(e, A64_HS);
	_andImm(e, 8, 4, size == 4 ? 2 : size == 2 ? 1 : 0, size == 4 ? 15 : size == 2 ? 16 : 17);
	_ldrX(e, 10, R_GBA, offsetof(struct GBA, video.vram));
	_strW(e, 4, A64_SP, ADDRESS_SLOT);
	if (load) {
		_memAccess(e, true, size, 10);
		_movW(e, R_WB, 0); // writeback is already stored for loads
	} else {
		if (size == 4) {
			_ldstR(e, A64_LDR_R | A64_UXTW, 9, 10, 8);
			_cmpW(e, 9, 6);
		} else {
			_ldstR(e, A64_LDRH_R | A64_UXTW, 9, 10, 8);
			_andImm(e, 11, 6, 0, 16);
			_cmpW(e, 9, 11);
		}
		uint8_t* unchanged = _bCond(e, A64_EQ);
		_memAccess(e, false, size, 10);
		// Once per 4 KiB of VRAM between events, unless a tile cache wants every address
		_ldrX(e, 9, R_GBA, offsetof(struct GBA, video.renderer));
		_ldrX(e, 9, 9, offsetof(struct GBAVideoRenderer, cache));
		uint8_t* cached = _cbnzX(e, 9);
		_ldrW(e, 9, A64_SP, ADDRESS_SLOT);
		_ubfx(e, 9, 9, 12, 5);
		_ldrW(e, 10, R_JIT, JIT_VRAM_NOTIFIED);
		_dp(e, A64_LSRV, 11, 10, 9, 0);
		uint8_t* notified = _tbnz(e, 11, 0);
		_movImm32(e, 11, 1);
		_dp(e, A64_LSLV, 11, 11, 9, 0);
		_dp(e, A64_ORR, 10, 10, 11, 0);
		_strW(e, 10, R_JIT, JIT_VRAM_NOTIFIED);
		_patch(cached, e->p);
		unsigned call;
		for (call = 0; call < (size == 4 ? 2 : 1); ++call) {
			_ldrX(e, 0, R_GBA, offsetof(struct GBA, video.renderer));
			_ldrW(e, 1, A64_SP, ADDRESS_SLOT);
			_andImm(e, 1, 1, size == 4 ? 2 : 1, size == 4 ? 15 : 16);
			if (size == 4 && call == 0) {
				_addWImm(e, 1, 1, 2);
			}
			_ldrX(e, 16, 0, offsetof(struct GBAVideoRenderer, writeVRAM));
			_callRoutine(e, c->jit->callC);
		}
		_patch(notified, e->p);
		_ldrW(e, 4, A64_SP, ADDRESS_SLOT);
		_patch(unchanged, e->p);
	}
	_ldrW(e, 9, R_GBA, offsetof(struct GBA, video.stallMask));
	uint8_t* noStall = _cbzW(e, 9);
	_movX(e, 0, R_GBA);
	_movW(e, 1, 4);
	_movImm32(e, 2, size);
	_callRoutine(e, c->jit->callVramWait);
	_movW(e, 3, 0);
	uint8_t* waited = _b(e);
	_patch(noStall, e->p);
	_movImm32(e, 3, size == 4 ? 1 : 0);
	_patch(waited, e->p);
	_addWImm(e, 3, 3, load ? 2 : 1);
	if (load) {
		_movW(e, 0, R_WB);
	}
}

// Everything else goes through the memory handlers
static void _memSlow(struct Compiler* c, unsigned i, const struct MemOp* mem) {
	struct Emitter* e = &c->e;
	unsigned size = mem->size;
	_storeState(c, i + 1);
	_strW(e, A64_ZR, A64_SP, CYCLE_SLOT);
	_movX(e, 0, R_CPU);
	_movW(e, 1, 4);
	unsigned offset;
	if (mem->load) {
		_addXImm(e, 2, A64_SP, CYCLE_SLOT);
		offset = size == 4 ? offsetof(struct ARMMemory, load32) : size == 2 ? offsetof(struct ARMMemory, load16) : offsetof(struct ARMMemory, load8);
	} else {
		_movW(e, 2, 6);
		_addXImm(e, 3, A64_SP, CYCLE_SLOT);
		offset = size == 4 ? offsetof(struct ARMMemory, store32) : size == 2 ? offsetof(struct ARMMemory, store16) : offsetof(struct ARMMemory, store8);
	}
	_ldrX(e, 16, R_CPU, OFF_MEMORY + offset);
	_callRoutine(e, c->jit->callC);
	_ldrW(e, 3, A64_SP, CYCLE_SLOT);
	if (!mem->load) {
		// The interpreter charges a store N - S as the store leaves them, not as compiled
		unsigned nonseq;
		unsigned seq;
		_fetchWaits(c, &nonseq, &seq);
		_ldrW(e, 9, R_CPU, nonseq);
		_ldrW(e, 10, R_CPU, seq);
		_dp(e, A64_SUB, 9, 9, 10, 0);
		_dp(e, A64_ADD, 3, 3, 9, 0);
		_dpImm(e, A64_SUB_IMM, 3, 3, c->memCycles - c->aluCycles);
	}
}

static unsigned _memRoutineIndex(bool load, unsigned size, bool signExtend) {
	unsigned s = size == 4 ? 2 : size == 2 ? 1 : 0;
	return !load ? s : signExtend ? 6 + s : 3 + s;
}

// w3 = the EWRAM wait as the GBA has it now, which blocks would have compiled in
static void _ewramWait(struct Emitter* e, unsigned size) {
	_addXImm(e, 9, R_GBA, size == 4 ? offsetof(struct GBA, memory.waitstatesNonseq32) : offsetof(struct GBA, memory.waitstatesNonseq16));
	_ldrbW(e, 3, 9, GBA_REGION_EWRAM);
}

// w3 = a data access wait from cartridge code as GBAMemoryStall makes it, the code's PC in PC_SLOT, when prefetch is on
static void _routineRomStall(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_ldrbW(e, 9, R_GBA, offsetof(struct GBA, memory.prefetch));
	uint8_t* off = _cbzW(e, 9);
	_ldrW(e, 11, A64_SP, PC_SLOT);
	_ldrW(e, 13, R_CPU, OFF_MEMORY + offsetof(struct ARMMemory, activeSeqCycles16));
	_cmpWImm(e, 13, ARM_JIT_STALL_SEQ);
	uint8_t* wide = _bCond(e, A64_HS);
	_cmpWImm(e, 3, ARM_JIT_STALL_WAIT);
	uint8_t* longWait = _bCond(e, A64_HS);
	// The byte tables for this sequential wait and data wait
	_dp(e, A64_ADD, 12, 3, 13, 4);
	_lslWImm(e, 12, 12, 4);
	_addXImm(e, 10, R_JIT, JIT_STALL_TABLES);
	_ldstR(e, A64_LDRX_R | A64_UXTW, 14, 10, 12);
	_addWImm(e, 12, 12, 8);
	_ldstR(e, A64_LDRX_R | A64_UXTW, 15, 10, 12);
	// The stall's adjustment, wait - (n - s)
	_ldrW(e, 9, R_CPU, OFF_MEMORY + offsetof(struct ARMMemory, activeNonseqCycles16));
	_dp(e, A64_SUB, 3, 3, 9, 0);
	_dp(e, A64_ADD, 3, 3, 13, 0);
	// w12 = 8 * previous loads, or 0 when the last prefetch is not 0 to 14 bytes ahead
	_ldrW(e, 10, R_GBA, offsetof(struct GBA, memory.lastPrefetchedPc));
	_dp(e, A64_SUB, 10, 10, 11, 0);
	_ubfx(e, 12, 10, 1, 3);
	_lslWImm(e, 12, 12, 3);
	_cmpWImm(e, 10, 16);
	_csel(e, 12, 12, A64_ZR, A64_LO);
	_dp(e, A64_LSRV | A64_X, 14, 14, 12, 0);
	_andImm(e, 14, 14, 0, 8);
	_dp(e, A64_ADD, 14, 11, 14, 1);
	_strW(e, 14, R_GBA, offsetof(struct GBA, memory.lastPrefetchedPc));
	_dp(e, A64_LSRV | A64_X, 15, 15, 12, 0);
	_sxtb(e, 15, 15);
	_dp(e, A64_ADD, 3, 3, 15, 0);
	uint8_t* done = _b(e);
	_patch(wide, e->p);
	_patch(longWait, e->p);
	_movX(e, 0, R_GBA);
	_movW(e, 1, 11);
	_movW(e, 2, 3);
	_callRoutine(e, c->jit->callRomStall);
	_movW(e, 3, 0);
	_patch(done, e->p);
	_patch(off, e->p);
}

// An access, address in w4 and a store's value in w6; a load leaves its value in w0 and every wait goes on the cycle count. From cartridge code the PC comes in w11.
// It returns past the caller's next instruction, or to that instruction with w17 = 0 for the handlers, or w17 = 1 after a store into compiled code.
static uint8_t* _emitMemRoutine(struct Compiler* c, bool load, unsigned size, bool signExtend, bool rom) {
	struct Emitter* e = &c->e;
	struct MemOp mem;
	memset(&mem, 0, sizeof(mem));
	mem.load = load;
	mem.size = size;
	mem.signExtend = signExtend;
	uint8_t* routine = e->p;
	uint8_t* done[6];
	unsigned nDone = 0;
	uint8_t* handlers[4];
	unsigned nHandlers = 0;
	uint8_t* miss;
	uint8_t* slow;
	_movX(e, R_RETURN, 30);
	if (signExtend && size == 2) {
		_strW(e, 4, A64_SP, ADDRESS_SLOT);
	}
	if (rom) {
		_strW(e, 11, A64_SP, PC_SLOT);
	}
	_lsrWImm(e, 7, 4, 24);
	unsigned x;
	for (x = 0; x < 2; ++x) {
		bool ewram = x;
		unsigned lsb = size == 4 ? 2 : size == 2 ? 1 : 0;
		_cmpWImm(e, 7, ewram ? GBA_REGION_EWRAM : GBA_REGION_IWRAM);
		miss = _bCond(e, A64_NE);
		_andImm(e, 8, 4, lsb, (ewram ? 18 : 15) - lsb);
		_memAccess(e, load, size, ewram ? R_EWRAM : R_IWRAM);
		if (ewram) {
			_ewramWait(e, size);
		} else {
			_movImm32(e, 3, 0);
		}
		_addWImm(e, 3, 3, load ? 2 : 1);
		if (rom) {
			_routineRomStall(c);
		}
		_addW(e, R_CYCLES, R_CYCLES, 3);
		if (!load) {
			_lsrWImm(e, 9, 8, 2);
			if (ewram) {
				_movImm32(e, 10, ARM_JIT_IWRAM_WORDS);
				_addW(e, 9, 9, 10);
			}
			_ldstR(e, A64_LDRB_R | A64_UXTW, 10, R_COVER, 9);
			uint8_t* clean = _cbzW(e, 10);
			_movX(e, 0, R_JIT);
			_movW(e, 1, 9);
			_callRoutine(e, c->jit->callInvalidate);
			_movImm32(e, 17, 1);
			_ret(e, R_RETURN);
			_patch(clean, e->p);
		}
		done[nDone++] = _b(e);
		_patch(miss, e->p);
	}
	// Cartridge code leaves VRAM to the handlers
	if (!rom && (load || size > 1)) {
		_memVram(c, &mem, &miss, &slow);
		_addW(e, R_CYCLES, R_CYCLES, 3);
		if (signExtend && size == 2) {
			_ldrW(e, 4, A64_SP, ADDRESS_SLOT);
		}
		done[nDone++] = _b(e);
		handlers[nHandlers++] = slow;
		_patch(miss, e->p);
	}
	if (load && size <= 2) {
		_memIo(c, 0, &mem, &miss, &slow);
		if (rom) {
			_routineRomStall(c);
		}
		_addW(e, R_CYCLES, R_CYCLES, 3);
		if (signExtend && size == 2) {
			_ldrW(e, 4, A64_SP, ADDRESS_SLOT);
		}
		done[nDone++] = _b(e);
		handlers[nHandlers++] = slow;
		_patch(miss, e->p);
	}
	if (load) {
		// Cartridge reads, as _memCart has them but with the ROM size read now
		_dpImm(e, A64_SUB_IMM, 9, 7, GBA_REGION_ROM0);
		_cmpWImm(e, 9, (GBA_REGION_ROM2 - GBA_REGION_ROM0));
		handlers[nHandlers++] = _bCond(e, A64_HI);
		_andImm(e, 8, 4, size == 4 ? 2 : size == 2 ? 1 : 0, size == 4 ? 23 : size == 2 ? 24 : 25);
		_ldrW(e, 9, R_GBA, offsetof(struct GBA, memory.romSize));
		_cmpW(e, 8, 9);
		uint8_t* outside = _bCond(e, A64_HS);
		_ldrX(e, 10, R_GBA, offsetof(struct GBA, memory.rom));
		_memAccess(e, true, size, 10);
		_addXImm(e, 10, R_GBA, size == 4 ? offsetof(struct GBA, memory.waitstatesNonseq32) : offsetof(struct GBA, memory.waitstatesNonseq16));
		_ldstR(e, A64_LDRB_R | A64_UXTW, 3, 10, 7);
		_addWImm(e, 3, 3, 2);
		_addW(e, R_CYCLES, R_CYCLES, 3);
		done[nDone++] = _b(e);
		_patch(outside, e->p);
	} else {
		handlers[nHandlers++] = _b(e);
	}
	unsigned k;
	for (k = 0; k < nHandlers; ++k) {
		_patch(handlers[k], e->p);
	}
	_movImm32(e, 17, 0);
	_ret(e, R_RETURN);
	for (k = 0; k < nDone; ++k) {
		_patch(done[k], e->p);
	}
	if (signExtend) {
		uint8_t* halfword = NULL;
		if (size == 2) {
			halfword = _tbz(e, 4, 0);
		}
		_sxtb(e, 0, 0);
		if (halfword) {
			uint8_t* extended = _b(e);
			_patch(halfword, e->p);
			_sxth(e, 0, 0);
			_patch(extended, e->p);
		}
	}
	_addXImm(e, 30, R_RETURN, 4);
	_ret(e, 30);
	return routine;
}

static void _emitMemRoutines(struct Compiler* c) {
	static const unsigned sizes[] = { 1, 2, 4 };
	bool romCode = c->romCode;
	c->romCode = false;
	unsigned rom;
	for (rom = 0; rom < 2; ++rom) {
		uint8_t** routines = rom ? c->jit->memRomRoutines : c->jit->memRoutines;
		unsigned s;
		for (s = 0; s < 3; ++s) {
			routines[_memRoutineIndex(false, sizes[s], false)] = _emitMemRoutine(c, false, sizes[s], false, rom);
			routines[_memRoutineIndex(true, sizes[s], false)] = _emitMemRoutine(c, true, sizes[s], false, rom);
		}
		routines[_memRoutineIndex(true, 1, true)] = _emitMemRoutine(c, true, 1, true, rom);
		routines[_memRoutineIndex(true, 2, true)] = _emitMemRoutine(c, true, 2, true, rom);
	}
	c->romCode = romCode;
}

// The handlers or a store into compiled code, after a shared routine sent the access back
static void _emitMemReturn(struct Compiler* c, unsigned k) {
	struct Emitter* e = &c->e;
	unsigned i = c->cold[k].index;
	const struct MemOp* mem = &c->cold[k].mem;
	_patch(c->cold[k].site, e->p);
	uint8_t* stored = NULL;
	if (!mem->load) {
		stored = _cbnzW(e, 17);
	}
	if (mem->signExtend && mem->size == 2) {
		_strW(e, 4, A64_SP, ADDRESS_SLOT);
	}
	_memSlow(c, i, mem);
	_addW(e, R_CYCLES, R_CYCLES, 3);
	if (mem->load) {
		if (mem->signExtend) {
			uint8_t* halfword = NULL;
			if (mem->size == 2) {
				_ldrW(e, 9, A64_SP, ADDRESS_SLOT);
				halfword = _tbz(e, 9, 0);
			}
			_sxtb(e, 0, 0);
			if (halfword) {
				uint8_t* extended = _b(e);
				_patch(halfword, e->p);
				_sxth(e, 0, 0);
				_patch(extended, e->p);
			}
		}
		_jumpTo(c, c->cold[k].done);
		return;
	}
	_patch(stored, e->p);
	if (mem->writeback) {
		_strW(e, R_WB, R_CPU, 4 * mem->base.reg);
	}
	_addCycles(c, c->memCycles);
	_smcCheck(c, i + 1);
	_jumpTo(c, c->cold[k].toEvent);
}

static void _emitMem(struct Compiler* c, unsigned i, const struct MemOp* mem) {
	struct Emitter* e = &c->e;
	bool load = mem->load;
	unsigned k = c->nCold;

	uint8_t* fail = NULL;
	if (mem->cond != 0xE) {
		_loadFlags(c);
		fail = _bCond(e, mem->cond ^ 1);
	}
	uint32_t literal;
	void* literalHost;
	int32_t literalWait;
	switch (_literal(c, mem, &literal, &literalHost, &literalWait)) {
	case LITERAL_CONSTANT:
		_movImm32(e, 0, literal);
		_movImm32(e, 3, literalWait);
		goto tail;
	case LITERAL_RAM:
		_ldrRam(c, 0, mem->up ? mem->base.value + mem->offset : mem->base.value - mem->offset, false);
		_dataWait(c, i, literalWait);
		goto tail;
	}
	_loadSource(c, 4, mem->base);
	if (!load) {
		_loadSource(c, 6, _reg(mem->rd));
	}
	if (mem->runtimeOffset) {
		_ldrW(e, 1, R_JIT, JIT_FETCHED + 4 * i);
		_ubfx(e, 9, 1, 23, 1);
		_andImm(e, 1, 1, 0, 12);
		uint8_t* up = _cbnzW(e, 9);
		_negW(e, 1, 1);
		_patch(up, e->p);
	} else if (mem->immediateOffset && mem->offset >= 0x1000) {
		_movImm32(e, 1, mem->offset);
	} else if (!mem->immediateOffset) {
		_loadSource(c, 3, mem->m);
		_shiftImm(c, 3, mem->shiftType, mem->shiftAmount, false);
	}
	// Without writeback a pre-indexed access needs only the sum, so it replaces the base
	bool sumOnly = mem->pre && !mem->writeback;
	if (mem->immediateOffset && !mem->runtimeOffset && mem->offset < 0x1000) {
		if (mem->offset) {
			_dpImm(e, mem->up ? A64_ADD_IMM : A64_SUB_IMM, sumOnly ? 4 : 5, 4, mem->offset);
		} else if (!sumOnly) {
			_movW(e, 5, 4);
		}
	} else {
		_dp(e, mem->up || mem->runtimeOffset ? A64_ADD : A64_SUB, sumOnly ? 4 : 5, 4, 1, 0);
	}
	if (mem->writeback) {
		_movW(e, R_WB, 5);
		if (mem->pre) {
			_movW(e, 4, 5);
		}
	}
	if (load && mem->writeback) {
		_strW(e, R_WB, R_CPU, 4 * mem->base.reg);
	}
	// The routine for the access, which comes back past the jump to its handler path
	unsigned index = _memRoutineIndex(load, mem->size, mem->signExtend);
	if (c->romCode) {
		_movImm32(e, 11, c->pc + c->width * (i + 2));
		_callRoutine(e, c->jit->memRomRoutines[index]);
	} else {
		_callRoutine(e, c->jit->memRoutines[index]);
	}
	c->cold[k].index = i;
	c->cold[k].mem = *mem;
	c->cold[k].site = _b(e);
	c->cold[k].done = e->p;
	if (load) {
		_strW(e, 0, R_CPU, 4 * mem->rd);
	} else if (mem->writeback) {
		_strW(e, R_WB, R_CPU, 4 * mem->base.reg);
	}
	_addCycles(c, c->memCycles);
	_skipped(c, fail);
	c->cold[k].toEvent = e->p;
	++c->nCold;
	_eventCheck(c, i + 1);
	return;

tail:
	_memResult(c, mem);
	_skipped(c, fail);
	_eventCheck(c, i + 1);
}

static void _emitColdPaths(struct Compiler* c) {
	unsigned k;
	for (k = 0; k < c->nCold; ++k) {
		_emitMemReturn(c, k);
	}
}

// Jumps to the returned site unless the word the pipeline fetched for ops[i], masked, is value
static uint8_t* _fetchedMismatch(struct Compiler* c, unsigned i, uint32_t mask, uint32_t value) {
	struct Emitter* e = &c->e;
	_ldrW(e, 0, R_JIT, JIT_FETCHED + 4 * i);
	if (mask != 0xFFFFFFFF) {
		_movImm32(e, 1, mask);
		_dp(e, A64_AND, 0, 0, 1, 0);
	}
	_movImm32(e, 1, value);
	_cmpW(e, 0, 1);
	return _bCond(e, A64_NE);
}

static uint8_t* _forwardJump(struct Compiler* c) {
	return _b(&c->e);
}

// A patched instruction through its handler, with the opcode the pipeline fetched
static void _emitDynamicHandler(struct Compiler* c, unsigned i) {
	struct Emitter* e = &c->e;
	uint32_t address = c->pc + c->width * i;

	_storeState(c, i + 1);
	_ldrW(e, 1, R_JIT, JIT_FETCHED + 4 * i);
	uint8_t* toCheck = NULL;
	if (c->thumb) {
		_lsrWImm(e, 12, 1, 6);
		_movImm64(e, 14, (uintptr_t) _thumbTable);
	} else {
		_lsrWImm(e, 9, R_FLAGS, 28);
		_lsrWImm(e, 10, 1, 28);
		_movImm64(e, 11, (uintptr_t) _conditionLut);
		_ldstR(e, A64_LDRH_R | A64_UXTW | A64_SCALED, 11, 11, 10);
		_dp(e, A64_LSRV, 11, 11, 9, 0);
		uint8_t* toExec = _tbnz(e, 11, 0);
		_addCycles(c, c->aluCycles);
		toCheck = _b(e);
		_patch(toExec, e->p);
		_ubfx(e, 12, 1, 20, 8);
		_ubfx(e, 13, 1, 4, 4);
		_dp(e, A64_ORR, 12, 13, 12, 4);
		_movImm64(e, 14, (uintptr_t) _armTable);
	}
	_ldstR(e, A64_LDRX_R | A64_UXTW | A64_SCALED, 16, 14, 12);
	_movX(e, 0, R_CPU);
	_callRoutine(e, c->jit->callHandler);
	_ldrW(e, 0, R_CPU, OFF_PC);
	_movImm32(e, 1, address + 2 * c->width);
	_cmpW(e, 0, 1);
	_exitAt(c, _bCond(e, A64_NE), EXIT_DIRECT);
	if (toCheck) {
		_patch(toCheck, e->p);
	}
	_smcCheck(c, i + 1);
	_eventCheck(c, i + 1);
}

static void _emitFallback(struct Compiler* c, unsigned i) {
	struct Emitter* e = &c->e;
	uint32_t op = c->ops[i];
	uint32_t address = c->pc + c->width * i;
	unsigned cond = _condition(op, c->thumb);

	_storeState(c, i + 1);
	uint8_t* toCheck = NULL;
	if (cond != 0xE) {
		_lsrWImm(e, 0, R_FLAGS, 28);
		_movImm32(e, 1, _conditionLut[cond]);
		_dp(e, A64_LSRV, 1, 1, 0, 0);
		uint8_t* toExec = _tbnz(e, 1, 0);
		_addCycles(c, c->aluCycles);
		toCheck = _b(e);
		_patch(toExec, e->p);
	}

	_movX(e, 0, R_CPU);
	_movImm32(e, 1, op);
	_movImm64(e, 16, (uintptr_t) _handler(op, c->thumb));
	_callRoutine(e, c->jit->callHandler);
	_ldrW(e, 0, R_CPU, OFF_PC);
	_movImm32(e, 1, address + 2 * c->width);
	_cmpW(e, 0, 1);
	uint8_t* sequential = _bCond(e, A64_EQ);
	struct BranchOp branch;
	if (!_branchTarget(c, i, &branch)) {
		_exitJump(c, EXIT_DIRECT);
	} else if (branch.target == c->pc) {
		// A taken branch back to the start of this block keeps running here
		_movImm32(e, 1, c->pc + c->width);
		_cmpW(e, 0, 1);
		_exitAt(c, _bCond(e, A64_NE), EXIT_DIRECT);
		_ldrbW(e, 0, R_JIT, JIT_BLOCKS_DROPPED);
		_exitAt(c, _cbnzW(e, 0), EXIT_DIRECT);
		_cmpW(e, R_CYCLES, R_NEXT);
		_exitAt(c, _bCond(e, A64_GE), EXIT_EVENTS);
		c->loops[c->nLoops++] = _b(e);
	} else {
		_cmpW(e, R_CYCLES, R_NEXT);
		_exitAt(c, _bCond(e, A64_GE), EXIT_EVENTS);
		_linkJump(c);
	}
	_patch(sequential, e->p);
	if (toCheck) {
		_patch(toCheck, e->p);
	}
	_smcCheck(c, i + 1);
	_eventCheck(c, i + 1);
}

// A handler fallback that goes after the block, entered from the sites; it comes back to the next instruction
static void _deferFallback(struct Compiler* c, unsigned i, bool dynamic, uint8_t* const* sites, unsigned nSites) {
	unsigned d = c->nDeferred++;
	c->deferred[d].index = i;
	c->deferred[d].dynamic = dynamic;
	c->deferred[d].nSites = nSites;
	memcpy(c->deferred[d].sites, sites, nSites * sizeof(*sites));
}

static void _emitPatchedFallback(struct Compiler* c, unsigned i) {
	uint8_t* site = _b(&c->e);
	_deferFallback(c, i, true, &site, 1);
}

static void _emitDeferred(struct Compiler* c) {
	struct Emitter* e = &c->e;
	unsigned d;
	for (d = 0; d < c->nDeferred; ++d) {
		unsigned i = c->deferred[d].index;
		unsigned s;
		for (s = 0; s < c->deferred[d].nSites; ++s) {
			_patch(c->deferred[d].sites[s], e->p);
		}
		if (c->deferred[d].dynamic) {
			_emitDynamicHandler(c, i);
		} else {
			_emitFallback(c, i);
		}
		_jumpTo(c, c->fast[i + 1] ? c->fast[i + 1] : c->careful[i + 1]);
	}
}

// Thumb BX to the same mode and memory region: what the interpreter does when its region switch has nothing to switch, then the lookup
static void _emitBx(struct Compiler* c, unsigned i, unsigned rm) {
	struct Emitter* e = &c->e;
	unsigned region = c->pc >> 24;
	bool rom = region >= GBA_REGION_ROM0 && region <= GBA_REGION_ROM2_EX;
	if (!c->thumb || !c->gba || (!rom && region != GBA_REGION_IWRAM && region != GBA_REGION_EWRAM)) {
		_emitFallback(c, i);
		return;
	}
	uint8_t* slow[6];
	unsigned nSlow = 0;
	_ldrW(e, 0, R_CPU, 4 * rm);
	slow[nSlow++] = _tbz(e, 0, 0);
	_lsrWImm(e, 1, 0, 24);
	_cmpWImm(e, 1, region);
	slow[nSlow++] = _bCond(e, A64_NE);
	// No idle loop handling to do
	_ldrW(e, 2, R_GBA, offsetof(struct GBA, idleOptimization));
	_cmpWImm(e, 2, IDLE_LOOP_DETECT);
	slow[nSlow++] = _bCond(e, A64_GE);
	_andImm(e, 0, 0, 1, 31);
	_ldrW(e, 2, R_GBA, offsetof(struct GBA, idleLoop));
	_cmpW(e, 0, 2);
	slow[nSlow++] = _bCond(e, A64_EQ);
	int base;
	if (rom) {
		_andImm(e, 3, 0, 0, 25);
		_ldrW(e, 2, R_GBA, offsetof(struct GBA, memory.romSize));
		_cmpW(e, 3, 2);
		slow[nSlow++] = _bCond(e, A64_HS);
		_ldrW(e, 9, R_GBA, offsetof(struct GBA, memory.romMask));
		_ldrX(e, 10, R_GBA, offsetof(struct GBA, memory.rom));
		_dp(e, A64_AND, 3, 0, 9, 0);
		_dpImm(e, A64_ADD_IMM, 4, 0, WORD_SIZE_THUMB);
		_dp(e, A64_AND, 4, 4, 9, 0);
		base = 10;
	} else {
		unsigned bits = region == GBA_REGION_IWRAM ? 15 : 18;
		_andImm(e, 3, 0, 0, bits);
		_dpImm(e, A64_ADD_IMM, 4, 0, WORD_SIZE_THUMB);
		_andImm(e, 4, 4, 0, bits);
		base = region == GBA_REGION_IWRAM ? R_IWRAM : R_EWRAM;
	}
	_strW(e, 0, R_GBA, offsetof(struct GBA, lastJump));
	_strW(e, A64_ZR, R_GBA, offsetof(struct GBA, memory.lastPrefetchedPc));
	_ldstR(e, A64_LDRH_R | A64_UXTW, 5, base, 3);
	_ldstR(e, A64_LDRH_R | A64_UXTW, 6, base, 4);
	_strW(e, 5, R_CPU, OFF_PREFETCH0);
	_strW(e, 6, R_CPU, OFF_PREFETCH1);
	_dpImm(e, A64_ADD_IMM, 0, 0, WORD_SIZE_THUMB);
	_strW(e, 0, R_CPU, OFF_PC);
	// The prefetch, then the refill's nonsequential and sequential fetches, as for a branch
	_addCycles(c, 2 * c->aluCycles + c->memCycles);
	_exitJump(c, EXIT_DIRECT);
	_deferFallback(c, i, false, slow, nSlow);
}

// The pipeline at a branch target: RAM words as they are now, others as compiled
static void _storeTargetPipeline(struct Compiler* c, uint32_t target) {
	struct Emitter* e = &c->e;
	_movImm32(e, 0, target + c->width);
	_strW(e, 0, R_CPU, OFF_PC);
	unsigned k;
	for (k = 0; k < 2; ++k) {
		uint32_t address = target + c->width * k;
		if (ARMJitRamWord(address) < 0) {
			_movImm32(e, 0, _opAt(c, address));
		} else {
			_ldrRam(c, 0, address, c->thumb);
		}
		_strW(e, 0, R_CPU, k ? OFF_PREFETCH1 : OFF_PREFETCH0);
	}
}

// A branch to w10 within the active region: returns past the caller's next instruction after the bookkeeping, or to it when the handler has to take the branch
static uint8_t* _emitBranchRoutine(struct Compiler* c, bool thumb) {
	struct Emitter* e = &c->e;
	uint8_t* routine = e->p;
	uint8_t* slow[3];
	_lsrWImm(e, 11, 10, 24);
	_ldrW(e, 12, R_GBA, offsetof(struct GBA, memory.activeRegion));
	_cmpW(e, 12, 11);
	slow[0] = _bCond(e, A64_NE);
	// Branches into the BIOS skip the idle loop checks
	uint8_t* bios = _cbzW(e, 11);
	_ldrW(e, 12, R_GBA, offsetof(struct GBA, idleOptimization));
	_cmpWImm(e, 12, IDLE_LOOP_DETECT);
	slow[1] = _bCond(e, A64_GE);
	_ldrW(e, 12, R_GBA, offsetof(struct GBA, idleLoop));
	_cmpW(e, 12, 10);
	slow[2] = _bCond(e, A64_EQ);
	_patch(bios, e->p);
	_strW(e, 10, R_GBA, offsetof(struct GBA, lastJump));
	_strW(e, A64_ZR, R_GBA, offsetof(struct GBA, memory.lastPrefetchedPc));
	_ldrW(e, 12, R_CPU, OFF_ACTIVE_MASK);
	if (thumb) {
		_logicImm(e, A64_ORR_IMM, 12, 12, 2);
	} else {
		_andImm(e, 12, 12, 2, 30);
	}
	_strW(e, 12, R_CPU, OFF_ACTIVE_MASK);
	_addXImm(e, 30, 30, 4);
	_ret(e, 30);
	unsigned k;
	for (k = 0; k < 3; ++k) {
		_patch(slow[k], e->p);
	}
	_ret(e, 30);
	return routine;
}

static void _emitBranchRoutines(struct Compiler* c) {
	c->jit->branchRoutines[0] = _emitBranchRoutine(c, false);
	c->jit->branchRoutines[1] = _emitBranchRoutine(c, true);
}

// Branches within a region, as GBASetActiveRegion and ARMWritePC or ThumbWritePC run them
static void _emitBranch(struct Compiler* c, unsigned i, const struct BranchOp* b) {
	struct Emitter* e = &c->e;
	uint8_t* notTaken = NULL;
	if (b->cond != 0xE) {
		_loadFlags(c);
		notTaken = _bCond(e, b->cond ^ 1);
	}
	uint8_t* slow[4];
	unsigned nSlow = 0;
	if (b->thumbLink) {
		_ldrW(e, 10, R_CPU, 4 * ARM_LR);
		_movImm32(e, 11, b->lr);
		_cmpW(e, 10, 11);
		slow[nSlow++] = _bCond(e, A64_NE);
	}
	_movImm32(e, 10, b->target);
	_callRoutine(e, c->jit->branchRoutines[c->thumb]);
	slow[nSlow++] = _b(e);
	uint32_t address = c->pc + c->width * i;
	if (b->link || b->thumbLink) {
		_movImm32(e, 10, address + (b->link ? WORD_SIZE_ARM : 3));
		_strW(e, 10, R_CPU, 4 * ARM_LR);
	}
	_addCycles(c, 2 * c->aluCycles + c->memCycles);
	if (b->index >= 0) {
		_fetchWord(c, b->index);
		_fetchWord(c, b->index + 1);
		_eventCheck(c, b->index);
		_jumpTo(c, c->fast[b->index]);
	} else {
		_storeTargetPipeline(c, b->target);
		_cmpW(e, R_CYCLES, R_NEXT);
		_exitAt(c, _bCond(e, A64_GE), EXIT_EVENTS);
		_linkJump(c);
	}

	if (notTaken) {
		_patch(notTaken, e->p);
		_addCycles(c, c->aluCycles);
		_eventCheck(c, i + 1);
	}
	_deferFallback(c, i, false, slow, nSlow);
}

// LDM or STM of the registers in w1 from the address in w4, on IWRAM or EWRAM, timed as GBALoadMultiple and GBAStoreMultiple do; cartridge code passes its PC in w11.
// It returns past the caller's next instruction, or to it for the handler, before transferring anything.
static uint8_t* _emitMultiRoutine(struct Compiler* c, bool load, bool rom) {
	struct Emitter* e = &c->e;
	uint8_t* routine = e->p;
	_movX(e, R_RETURN, 30);
	if (rom) {
		_strW(e, 11, A64_SP, PC_SLOT);
	}
	_lsrWImm(e, 7, 4, 24);
	uint8_t* handler[3];
	unsigned nHandler = 0;
	uint8_t* done[2];
	unsigned pass;
	for (pass = 0; pass < 2; ++pass) {
		bool iwram = pass == 0;
		unsigned bits = iwram ? 13 : 16;
		int base = iwram ? R_IWRAM : R_EWRAM;
		_cmpWImm(e, 7, iwram ? GBA_REGION_IWRAM : GBA_REGION_EWRAM);
		uint8_t* other = _bCond(e, A64_NE);
		uint8_t* loop;
		uint8_t* end;
		if (!load) {
			// Any word written that holds compiled code sends the whole transfer to the handler
			_movW(e, 9, 1);
			_movImm32(e, 12, 0);
			loop = e->p;
			end = _cbzW(e, 9);
			_dp(e, A64_SUB, 15, A64_ZR, 9, 0);
			_dp(e, A64_AND, 15, 9, 15, 0);
			_dp(e, A64_BIC, 9, 9, 15, 0);
			_dp(e, A64_ADD, 8, 4, 12, 2);
			_andImm(e, 8, 8, 2, bits);
			_lsrWImm(e, 8, 8, 2);
			if (!iwram) {
				_movImm32(e, 16, ARM_JIT_IWRAM_WORDS);
				_addW(e, 8, 8, 16);
			}
			_ldstR(e, A64_LDRB_R | A64_UXTW, 15, R_COVER, 8);
			handler[nHandler++] = _cbnzW(e, 15);
			_addWImm(e, 12, 12, 1);
			_patch(_b(e), loop);
			_patch(end, e->p);
		}
		// Each register in order from the lowest, at consecutive words
		_movW(e, 9, 1);
		_movImm32(e, 12, 0);
		loop = e->p;
		end = _cbzW(e, 9);
		_dp(e, A64_SUB, 15, A64_ZR, 9, 0);
		_dp(e, A64_AND, 15, 9, 15, 0);
		_dp(e, A64_BIC, 9, 9, 15, 0);
		_clzW(e, 14, 15);
		_logicImm(e, A64_EOR_IMM, 14, 14, 31);
		_dp(e, A64_ADD, 8, 4, 12, 2);
		_andImm(e, 8, 8, 2, bits);
		if (load) {
			_ldstR(e, A64_LDR_R | A64_UXTW, 15, base, 8);
			_ldstR(e, A64_STR_R | A64_UXTW | A64_SCALED, 15, R_CPU, 14);
		} else {
			_ldstR(e, A64_LDR_R | A64_UXTW | A64_SCALED, 15, R_CPU, 14);
			_ldstR(e, A64_STR_R | A64_UXTW, 15, base, 8);
		}
		_addWImm(e, 12, 12, 1);
		_patch(_b(e), loop);
		_patch(end, e->p);
		// Seq - nonseq of the data region, then per word 1 plus the EWRAM sequential wait, and 1 for a load
		_addXImm(e, 9, R_GBA, offsetof(struct GBA, memory.waitstatesSeq32));
		_ldrbW(e, 3, 9, iwram ? GBA_REGION_IWRAM : GBA_REGION_EWRAM);
		_addXImm(e, 9, R_GBA, offsetof(struct GBA, memory.waitstatesNonseq32));
		_ldrbW(e, 13, 9, iwram ? GBA_REGION_IWRAM : GBA_REGION_EWRAM);
		_dp(e, A64_SUB, 3, 3, 13, 0);
		if (iwram) {
			_dp(e, A64_ADD, 3, 3, 12, 0);
		} else {
			_addXImm(e, 9, R_GBA, offsetof(struct GBA, memory.waitstatesSeq32));
			_ldrbW(e, 13, 9, GBA_REGION_EWRAM);
			_addWImm(e, 13, 13, 1);
			_madd(e, 3, 12, 13, 3);
		}
		if (load) {
			_addWImm(e, 3, 3, 1);
		}
		if (rom) {
			_routineRomStall(c);
		}
		_addW(e, R_CYCLES, R_CYCLES, 3);
		done[pass] = _b(e);
		_patch(other, e->p);
	}
	unsigned k;
	for (k = 0; k < nHandler; ++k) {
		_patch(handler[k], e->p);
	}
	_ret(e, R_RETURN);
	_patch(done[0], e->p);
	_patch(done[1], e->p);
	_addXImm(e, 30, R_RETURN, 4);
	_ret(e, 30);
	return routine;
}

static void _emitMultiRoutines(struct Compiler* c) {
	unsigned load;
	unsigned rom;
	for (load = 0; load < 2; ++load) {
		for (rom = 0; rom < 2; ++rom) {
			c->jit->multiRoutines[load][rom] = _emitMultiRoutine(c, load, rom);
		}
	}
}

// LDM and STM on IWRAM or EWRAM, timed as GBALoadMultiple and GBAStoreMultiple do
static void _emitMulti(struct Compiler* c, unsigned i, const struct MultiOp* m) {
	struct Emitter* e = &c->e;
	unsigned n = __builtin_popcount(m->list);
	uint8_t* fail = NULL;
	if (m->cond != 0xE) {
		_loadFlags(c);
		fail = _bCond(e, m->cond ^ 1);
	}
	_ldrW(e, 1, R_CPU, 4 * m->rn);
	int32_t start = m->up ? (m->pre ? 4 : 0) : (m->pre ? -4 * (int32_t) n : -4 * (int32_t) n + 4);
	_dpImm(e, start >= 0 ? A64_ADD_IMM : A64_SUB_IMM, 4, 1, start >= 0 ? start : -start);
	_dpImm(e, m->up ? A64_ADD_IMM : A64_SUB_IMM, R_WB, 1, 4 * n);
	if (c->romCode) {
		_movImm32(e, 11, c->pc + c->width * (i + 2));
	}
	_movImm32(e, 1, m->list);
	_callRoutine(e, c->jit->multiRoutines[m->load != 0][c->romCode]);
	uint8_t* slow = _b(e);
	if (m->writeback) {
		_strW(e, R_WB, R_CPU, 4 * m->rn);
	}
	_addCycles(c, c->memCycles);
	_skipped(c, fail);
	_eventCheck(c, i + 1);
	_deferFallback(c, i, false, &slow, 1);
}

// MUL, MLA and Thumb MUL, timed as ARM_WAIT_SMUL and the stall handler time them
static void _emitMul(struct Compiler* c, unsigned i, const struct MulOp* m) {
	struct Emitter* e = &c->e;
	uint8_t* fail = NULL;
	if (m->cond != 0xE) {
		_loadFlags(c);
		fail = _bCond(e, m->cond ^ 1);
	}
	// One more cycle for each significant byte past the first, counting leading ones as zeros: (31 - clz(x | 0xFF)) / 8
	_ldrW(e, 1, R_CPU, 4 * m->rs);
	_asrWImm(e, 9, 1, 31);
	_dp(e, A64_EOR, 9, 9, 1, 0);
	_logicImm(e, A64_ORR_IMM, 10, 9, 0xFF);
	_clzW(e, 10, 10);
	_logicImm(e, A64_EOR_IMM, 10, 10, 31);
	_lsrWImm(e, 10, 10, 3);
	_dpImm(e, A64_ADD_IMM, 3, 10, m->rn >= 0 ? 2 : 1);
	if (c->romCode && c->prefetch) {
		_movImm32(e, 0, c->pc + c->width * (i + 2));
		_strW(e, 0, R_CPU, OFF_PC);
		_movX(e, 0, R_CPU);
		_movW(e, 1, 3);
		_ldrX(e, 16, R_CPU, OFF_MEMORY + offsetof(struct ARMMemory, stall));
		_callRoutine(e, c->jit->callC);
		_movW(e, 3, 0);
		_ldrW(e, 1, R_CPU, 4 * m->rs);
	}
	_ldrW(e, 2, R_CPU, 4 * m->rm);
	if (m->rn >= 0) {
		_ldrW(e, 5, R_CPU, 4 * m->rn);
		_madd(e, 0, 2, 1, 5);
	} else {
		_madd(e, 0, 2, 1, A64_ZR);
	}
	_strW(e, 0, R_CPU, 4 * m->rd);
	if (m->s) {
		// N and Z from the result; ARM takes C from the last shifter result
		_setNZ(e, 0, c->thumb ? 0x30000000 : 0x10000000);
		if (!c->thumb) {
			_ldrW(e, 10, R_CPU, OFF_SHIFTER_CARRY);
			_andImm(e, 10, 10, 0, 1);
			_dp(e, A64_ORR, R_FLAGS, R_FLAGS, 10, 29);
		}
	}
	_addCyclesReg(c, 3, c->memCycles);
	_skipped(c, fail);
	_eventCheck(c, i + 1);
}


// The interpreter state before the instruction at w0 minus a word, with the pipeline as the code holds it, then the due events; the caller resumes after
static uint8_t* _emitExitRoutine(struct Compiler* c, bool thumb) {
	struct Emitter* e = &c->e;
	uint8_t* routine = e->p;
	unsigned width = thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;
	uint32_t load = (thumb ? A64_LDRH_R : A64_LDR_R) | A64_UXTW;
	_strW(e, 0, R_CPU, OFF_PC);
	_ldrX(e, 9, R_CPU, OFF_MEMORY + offsetof(struct ARMMemory, activeRegion));
	_ldrW(e, 10, R_CPU, OFF_MEMORY + offsetof(struct ARMMemory, activeMask));
	_dpImm(e, A64_SUB_IMM, 11, 0, width);
	_dp(e, A64_AND, 11, 11, 10, 0);
	_ldstR(e, load, 12, 9, 11);
	_strW(e, 12, R_CPU, OFF_PREFETCH0);
	_dp(e, A64_AND, 11, 0, 10, 0);
	_ldstR(e, load, 12, 9, 11);
	_strW(e, 12, R_CPU, OFF_PREFETCH1);
	_jumpTo(c, c->jit->resume);
	return routine;
}

static void _emitExitRoutines(struct Compiler* c) {
	c->jit->exitRoutines[0] = _emitExitRoutine(c, false);
	c->jit->exitRoutines[1] = _emitExitRoutine(c, true);
}

// Runs the due events, then comes back to target unless the resume routine goes elsewhere
static void _resumeAfterEvents(struct Compiler* c, const uint8_t* target) {
	uint8_t* site = _bl(&c->e);
	_patch(site, c->jit->resume);
	_jumpTo(c, target);
}

// Exits for due events read the pipeline from the code through a shared routine, unless it holds a patched word
static void _emitExitStub(struct Compiler* c, unsigned at, bool due) {
	const uint8_t* target = c->fast[at] ? c->fast[at] : c->careful[at];
	if (due && !c->patched[at] && !c->patched[at + 1]) {
		_movImm32(&c->e, 0, c->pc + c->width * (at + 1));
		_callRoutine(&c->e, c->jit->exitRoutines[c->thumb]);
		_jumpTo(c, target);
		return;
	}
	_storeState(c, at);
	if (due) {
		_resumeAfterEvents(c, target);
	} else {
		_exitJump(c, EXIT_DIRECT);
	}
}
