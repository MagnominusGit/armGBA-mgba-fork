/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/jit.h>

#include <mgba/internal/defines.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/io.h>
#include <mgba/internal/gb/memory.h>
#include <mgba/internal/sm83/sm83.h>

#if defined(__x86_64__) && !defined(_WIN32)
#include <sys/mman.h>

// Each instruction is one call to a handler that does all of its M-cycles at once, ending with the
// registers, flags and bus latches the interpreter's stage functions would leave. A handler that would
// touch anything but ROM, work RAM or high RAM returns -1 before changing state, and the interpreter
// runs that instruction instead.

// at: the M-cycle of the block a store lands on
typedef int (*GBJitOp)(struct SM83Core* cpu, uint32_t operand, uint32_t pc, uint32_t at);

#define OP(NAME) static int _op ## NAME(struct SM83Core* cpu, uint32_t operand, uint32_t pc, uint32_t at)
#define USE_ALL UNUSED(cpu); UNUSED(operand); UNUSED(pc); UNUSED(at)

static inline int _load(struct SM83Core* cpu, uint16_t address, uint32_t pc) {
	struct GBMemory* memory = &((struct GB*) cpu->master)->memory;
	switch (address >> 12) {
	case 0x0:
	case 0x1:
	case 0x2:
	case 0x3:
		if (address >= memory->romSize) {
			memory->cartBus = 0xFF;
		} else if (memory->mbcReadBank0) {
			return -1;
		} else {
			memory->cartBus = memory->romBase[address & (GB_SIZE_CART_BANK0 - 1)];
		}
		memory->cartBusPc = pc;
		return memory->cartBus;
	case 0x6:
	case 0x7:
		if (memory->mbcType == GB_MBC6 || (memory->mbcType == GB_UNL_NT_NEW && memory->mbcState.ntNew.splitMode)) {
			memory->cartBus = memory->romBank1[address & (GB_SIZE_CART_HALFBANK - 1)];
			memory->cartBusPc = pc;
			return memory->cartBus;
		}
		// Fall through
	case 0x4:
	case 0x5:
		if (address >= memory->romSize) {
			memory->cartBus = 0xFF;
		} else if (memory->mbcReadBank1) {
			return -1;
		} else {
			memory->cartBus = memory->romBank[address & (GB_SIZE_CART_BANK0 - 1)];
		}
		memory->cartBusPc = pc;
		return memory->cartBus;
	case 0xC:
	case 0xE:
		if (memory->mbcReadHigh) {
			return -1;
		}
		return memory->wram[address & (GB_SIZE_WORKING_RAM_BANK0 - 1)];
	case 0xD:
		if (memory->mbcReadHigh) {
			return -1;
		}
		return memory->wramBank[address & (GB_SIZE_WORKING_RAM_BANK0 - 1)];
	case 0xF:
		if (address < GB_BASE_OAM) {
			return memory->wramBank[address & (GB_SIZE_WORKING_RAM_BANK0 - 1)];
		}
		if (address >= GB_BASE_HRAM && address < GB_BASE_IE) {
			return memory->hram[address & GB_SIZE_HRAM];
		}
		return -1;
	default:
		return -1;
	}
}

static inline bool _storable(struct SM83Core* cpu, uint16_t address) {
	struct GBMemory* memory = &((struct GB*) cpu->master)->memory;
	switch (address >> 12) {
	case 0xC:
	case 0xD:
	case 0xE:
		return !memory->mbcWriteHigh;
	case 0xF:
		return address < GB_BASE_OAM || (address >= GB_BASE_HRAM && address < GB_BASE_IE);
	default:
		return false;
	}
}

static inline void _store(struct SM83Core* cpu, uint16_t address, uint8_t value) {
	struct GBMemory* memory = &((struct GB*) cpu->master)->memory;
	switch (address >> 12) {
	case 0xC:
	case 0xE:
		memory->wram[address & (GB_SIZE_WORKING_RAM_BANK0 - 1)] = value;
		break;
	case 0xD:
		memory->wramBank[address & (GB_SIZE_WORKING_RAM_BANK0 - 1)] = value;
		break;
	default:
		if (address < GB_BASE_OAM) {
			memory->wramBank[address & (GB_SIZE_WORKING_RAM_BANK0 - 1)] = value;
		} else {
			memory->hram[address & GB_SIZE_HRAM] = value;
		}
		break;
	}
}

// Accesses outside work and high RAM, mirroring GBLoad8 and GBStore8 where nothing else in the machine is touched
static int _loadAny(struct SM83Core* cpu, uint16_t address, uint32_t pc) {
	int value = _load(cpu, address, pc);
	if (value >= 0) {
		return value;
	}
	struct GB* gb = (struct GB*) cpu->master;
	struct GBMemory* memory = &gb->memory;
	switch (address >> 12) {
	case 0x8:
	case 0x9:
		if (!gb->video.vramReadBlocked) {
			return gb->video.vramBank[address & (GB_SIZE_VRAM_BANK0 - 1)];
		}
		return 0xFF;
	case 0xA:
	case 0xB:
		if (memory->rtcAccess) {
			memory->cartBus = memory->rtcRegs[memory->activeRtcReg];
		} else if (memory->mbcRead) {
			return -1;
		} else if (memory->sramAccess && memory->sram) {
			memory->cartBus = memory->sramBank[address & (GB_SIZE_EXTERNAL_RAM - 1)];
		} else if (memory->mbcType == GB_HuC3) {
			memory->cartBus = 0x01;
		} else if (cpu->tMultiplier * ((uint16_t) pc - memory->cartBusPc) >= memory->cartBusDecay) {
			memory->cartBus = 0xFF;
		}
		memory->cartBusPc = pc;
		return memory->cartBus;
	case 0xF:
		if (address >= GB_BASE_OAM && address < GB_BASE_UNUSABLE) {
			if (!gb->video.oamReadBlocked) {
				return gb->video.oam.raw[address & 0xFF];
			}
			return 0xFF;
		}
		if (address >= GB_BASE_IO && address < GB_BASE_HRAM) {
			return GBIOReadDirect(gb, address & (GB_SIZE_IO - 1));
		}
		if (address == GB_BASE_IE) {
			return GBIOReadDirect(gb, GB_REG_IE);
		}
		return -1;
	default:
		return -1;
	}
}

static inline bool _storableAny(struct SM83Core* cpu, uint16_t address) {
	struct GBMemory* memory = &((struct GB*) cpu->master)->memory;
	switch (address >> 12) {
	case 0xC:
	case 0xD:
	case 0xE:
		return !memory->mbcWriteHigh;
	case 0xF:
		return address < GB_BASE_UNUSABLE || (address >= GB_BASE_HRAM && address < GB_BASE_IE);
	default:
		return true;
	}
}

// 1 when the store changed what a block may assume, so it has to end after this instruction
static int _storeAny(struct SM83Core* cpu, uint16_t address, uint8_t value, uint32_t pc, uint32_t at) {
	if (_storable(cpu, address)) {
		_store(cpu, address, value);
		return 0;
	}
	struct GB* gb = (struct GB*) cpu->master;
	struct GBMemory* memory = &gb->memory;
	int32_t cycles = cpu->cycles;
	int32_t nextEvent = cpu->nextEvent;
	const uint8_t* region = cpu->memory.activeRegion;
	uint16_t regionEnd = cpu->memory.activeRegionEnd;
	// The interpreter's clock and pc at this access, for a mapper that reads them
	cpu->cycles += cpu->tMultiplier * (4 * at + 1);
	cpu->pc = pc;
	switch (address >> 12) {
	case 0x8:
	case 0x9:
		if (!gb->video.vramWriteBlocked) {
			gb->video.renderer->writeVRAM(gb->video.renderer, (address & (GB_SIZE_VRAM_BANK0 - 1)) | (GB_SIZE_VRAM_BANK0 * gb->video.vramCurrentBank));
			gb->video.vramBank[address & (GB_SIZE_VRAM_BANK0 - 1)] = value;
		}
		break;
	case 0xA:
	case 0xB:
		if (memory->rtcAccess) {
			memory->rtcRegs[memory->activeRtcReg] = value;
		} else if (memory->sramAccess && memory->sram && memory->directSramAccess) {
			if (memory->sramBank[address & (GB_SIZE_EXTERNAL_RAM - 1)] != value) {
				memory->sramBank[address & (GB_SIZE_EXTERNAL_RAM - 1)] = value;
				gb->sramDirty |= mSAVEDATA_DIRT_NEW;
			}
		} else {
			memory->mbcWrite(gb, address, value);
		}
		break;
	case 0xF:
		if (!gb->video.oamWriteBlocked) {
			gb->video.oam.raw[address & 0xFF] = value;
			gb->video.renderer->writeOAM(gb->video.renderer, address & 0xFF);
		}
		break;
	default:
		memory->mbcWrite(gb, address, value);
		cpu->memory.setActiveRegion(cpu, cpu->pc);
		break;
	}
	cpu->cycles = cycles;
	return cpu->nextEvent != nextEvent || cpu->irqPending || memory->dmaRemaining || gb->cpuBlocked ||
	       cpu->memory.activeRegion != region || cpu->memory.activeRegionEnd != regionEnd ||
	       cpu->memory.activeMask != GB_SIZE_CART_BANK0 - 1 || cpu->memory.cpuLoad8 != GBCartLoad8;
}

// Registers by their place in the opcode: B, C, D, E, H, L, (HL), A
#define R0 cpu->b
#define R1 cpu->c
#define R2 cpu->d
#define R3 cpu->e
#define R4 cpu->h
#define R5 cpu->l
#define R7 cpu->a

#define ALU_AND(V) \
	cpu->a &= (V); \
	cpu->f.z = !cpu->a; \
	cpu->f.n = 0; \
	cpu->f.c = 0; \
	cpu->f.h = 1;

#define ALU_XOR(V) \
	cpu->a ^= (V); \
	cpu->f.z = !cpu->a; \
	cpu->f.n = 0; \
	cpu->f.c = 0; \
	cpu->f.h = 0;

#define ALU_OR(V) \
	cpu->a |= (V); \
	cpu->f.z = !cpu->a; \
	cpu->f.n = 0; \
	cpu->f.c = 0; \
	cpu->f.h = 0;

#define ALU_CP(V) { \
	int diff = cpu->a - (V); \
	cpu->f.n = 1; \
	cpu->f.z = !(diff & 0xFF); \
	cpu->f.h = (cpu->a & 0xF) - ((V) & 0xF) < 0; \
	cpu->f.c = diff < 0; }

#define ALU_ADD(V) { \
	int diff = cpu->a + (V); \
	cpu->f.n = 0; \
	cpu->f.h = (cpu->a & 0xF) + ((V) & 0xF) >= 0x10; \
	cpu->f.c = diff >= 0x100; \
	cpu->a = diff; \
	cpu->f.z = !cpu->a; }

#define ALU_ADC(V) { \
	int diff = cpu->a + (V) + cpu->f.c; \
	cpu->f.n = 0; \
	cpu->f.h = (cpu->a & 0xF) + ((V) & 0xF) + cpu->f.c >= 0x10; \
	cpu->f.c = diff >= 0x100; \
	cpu->a = diff; \
	cpu->f.z = !cpu->a; }

#define ALU_SUB(V) { \
	int diff = cpu->a - (V); \
	cpu->f.n = 1; \
	cpu->f.h = (cpu->a & 0xF) - ((V) & 0xF) < 0; \
	cpu->f.c = diff < 0; \
	cpu->a = diff; \
	cpu->f.z = !cpu->a; }

#define ALU_SBC(V) { \
	int diff = cpu->a - (V) - cpu->f.c; \
	cpu->f.n = 1; \
	cpu->f.h = (cpu->a & 0xF) - ((V) & 0xF) - cpu->f.c < 0; \
	cpu->f.c = diff < 0; \
	cpu->a = diff; \
	cpu->f.z = !cpu->a; }

// One-M-cycle operations: the bus keeps the opcode
OP(NOP) {
	USE_ALL;
	cpu->bus = 0x00;
	return 0;
}

#define DEFINE_LD_RR(OPCODE, DST, SRC) \
	OP(LD ## OPCODE) { \
		USE_ALL; \
		DST = SRC; \
		cpu->bus = 0x ## OPCODE; \
		return 0; \
	}

#define DEFINE_LD_ROW(HI, DST) \
	DEFINE_LD_RR(HI ## 0, DST, R0) DEFINE_LD_RR(HI ## 1, DST, R1) DEFINE_LD_RR(HI ## 2, DST, R2) DEFINE_LD_RR(HI ## 3, DST, R3) \
	DEFINE_LD_RR(HI ## 4, DST, R4) DEFINE_LD_RR(HI ## 5, DST, R5) DEFINE_LD_RR(HI ## 7, DST, R7)

#define DEFINE_LD_ROW_HIGH(HI, DST) \
	DEFINE_LD_RR(HI ## 8, DST, R0) DEFINE_LD_RR(HI ## 9, DST, R1) DEFINE_LD_RR(HI ## A, DST, R2) DEFINE_LD_RR(HI ## B, DST, R3) \
	DEFINE_LD_RR(HI ## C, DST, R4) DEFINE_LD_RR(HI ## D, DST, R5) DEFINE_LD_RR(HI ## F, DST, R7)

DEFINE_LD_ROW(4, R0)
DEFINE_LD_ROW_HIGH(4, R1)
DEFINE_LD_ROW(5, R2)
DEFINE_LD_ROW_HIGH(5, R3)
DEFINE_LD_ROW(6, R4)
DEFINE_LD_ROW_HIGH(6, R5)
DEFINE_LD_ROW_HIGH(7, R7)

// LD r,(HL) and LD (HL),r
#define DEFINE_LD_R_HL(OPCODE, DST) \
	OP(LD ## OPCODE) { \
		USE_ALL; \
		int value = _loadAny(cpu, cpu->hl, pc); \
		if (value < 0) { \
			return -1; \
		} \
		cpu->index = cpu->hl; \
		cpu->bus = value; \
		DST = value; \
		return 0; \
	}

DEFINE_LD_R_HL(46, R0)
DEFINE_LD_R_HL(4E, R1)
DEFINE_LD_R_HL(56, R2)
DEFINE_LD_R_HL(5E, R3)
DEFINE_LD_R_HL(66, R4)
DEFINE_LD_R_HL(6E, R5)
DEFINE_LD_R_HL(7E, R7)

#define DEFINE_LD_HL_R(OPCODE, SRC) \
	OP(LD ## OPCODE) { \
		USE_ALL; \
		if (!_storableAny(cpu, cpu->hl)) { \
			return -1; \
		} \
		cpu->bus = SRC; \
		cpu->index = cpu->hl; \
		return _storeAny(cpu, cpu->index, cpu->bus, pc, at); \
	}

DEFINE_LD_HL_R(70, R0)
DEFINE_LD_HL_R(71, R1)
DEFINE_LD_HL_R(72, R2)
DEFINE_LD_HL_R(73, R3)
DEFINE_LD_HL_R(74, R4)
DEFINE_LD_HL_R(75, R5)
DEFINE_LD_HL_R(77, R7)

// ALU A,r / A,(HL) / A,n
#define DEFINE_ALU_R(OPCODE, ALU, SRC) \
	OP(ALU ## OPCODE) { \
		USE_ALL; \
		ALU(SRC) \
		cpu->bus = 0x ## OPCODE; \
		return 0; \
	}

#define DEFINE_ALU_HL(OPCODE, ALU) \
	OP(ALU ## OPCODE) { \
		USE_ALL; \
		int value = _loadAny(cpu, cpu->hl, pc); \
		if (value < 0) { \
			return -1; \
		} \
		cpu->index = cpu->hl; \
		cpu->bus = value; \
		ALU(cpu->bus) \
		return 0; \
	}

#define DEFINE_ALU_N(OPCODE, ALU) \
	OP(ALU ## OPCODE) { \
		USE_ALL; \
		cpu->bus = operand; \
		ALU(cpu->bus) \
		return 0; \
	}

#define DEFINE_ALU_ROW(LO, ALU, HL, N) \
	DEFINE_ALU_R(LO ## 0, ALU, R0) DEFINE_ALU_R(LO ## 1, ALU, R1) DEFINE_ALU_R(LO ## 2, ALU, R2) DEFINE_ALU_R(LO ## 3, ALU, R3) \
	DEFINE_ALU_R(LO ## 4, ALU, R4) DEFINE_ALU_R(LO ## 5, ALU, R5) DEFINE_ALU_HL(HL, ALU) DEFINE_ALU_R(LO ## 7, ALU, R7) \
	DEFINE_ALU_N(N, ALU)

#define DEFINE_ALU_ROW_HIGH(LO, ALU, HL, N) \
	DEFINE_ALU_R(LO ## 8, ALU, R0) DEFINE_ALU_R(LO ## 9, ALU, R1) DEFINE_ALU_R(LO ## A, ALU, R2) DEFINE_ALU_R(LO ## B, ALU, R3) \
	DEFINE_ALU_R(LO ## C, ALU, R4) DEFINE_ALU_R(LO ## D, ALU, R5) DEFINE_ALU_HL(HL, ALU) DEFINE_ALU_R(LO ## F, ALU, R7) \
	DEFINE_ALU_N(N, ALU)

DEFINE_ALU_ROW(8, ALU_ADD, 86, C6)
DEFINE_ALU_ROW_HIGH(8, ALU_ADC, 8E, CE)
DEFINE_ALU_ROW(9, ALU_SUB, 96, D6)
DEFINE_ALU_ROW_HIGH(9, ALU_SBC, 9E, DE)
DEFINE_ALU_ROW(A, ALU_AND, A6, E6)
DEFINE_ALU_ROW_HIGH(A, ALU_XOR, AE, EE)
DEFINE_ALU_ROW(B, ALU_OR, B6, F6)
DEFINE_ALU_ROW_HIGH(B, ALU_CP, BE, FE)

// INC r / DEC r / LD r,n
#define DEFINE_INC(OPCODE, REG) \
	OP(INC ## OPCODE) { \
		USE_ALL; \
		int diff = REG + 1; \
		cpu->f.h = (REG & 0xF) == 0xF; \
		REG = diff; \
		cpu->f.n = 0; \
		cpu->f.z = !REG; \
		cpu->bus = 0x ## OPCODE; \
		return 0; \
	}

#define DEFINE_DEC(OPCODE, REG) \
	OP(DEC ## OPCODE) { \
		USE_ALL; \
		int diff = REG - 1; \
		cpu->f.h = (REG & 0xF) == 0x0; \
		REG = diff; \
		cpu->f.n = 1; \
		cpu->f.z = !REG; \
		cpu->bus = 0x ## OPCODE; \
		return 0; \
	}

#define DEFINE_LD_N(OPCODE, REG) \
	OP(LDN ## OPCODE) { \
		USE_ALL; \
		cpu->bus = operand; \
		REG = cpu->bus; \
		return 0; \
	}

DEFINE_INC(04, R0) DEFINE_DEC(05, R0) DEFINE_LD_N(06, R0)
DEFINE_INC(0C, R1) DEFINE_DEC(0D, R1) DEFINE_LD_N(0E, R1)
DEFINE_INC(14, R2) DEFINE_DEC(15, R2) DEFINE_LD_N(16, R2)
DEFINE_INC(1C, R3) DEFINE_DEC(1D, R3) DEFINE_LD_N(1E, R3)
DEFINE_INC(24, R4) DEFINE_DEC(25, R4) DEFINE_LD_N(26, R4)
DEFINE_INC(2C, R5) DEFINE_DEC(2D, R5) DEFINE_LD_N(2E, R5)
DEFINE_INC(3C, R7) DEFINE_DEC(3D, R7) DEFINE_LD_N(3E, R7)

OP(INC_HL) {
	USE_ALL;
	if (!_storableAny(cpu, cpu->hl)) {
		return -1;
	}
	int value = _loadAny(cpu, cpu->hl, pc);
	if (value < 0) {
		return -1;
	}
	cpu->index = cpu->hl;
	cpu->bus = value;
	int diff = cpu->bus + 1;
	cpu->f.n = 0;
	cpu->f.h = (cpu->bus & 0xF) == 0xF;
	cpu->bus = diff;
	cpu->f.z = !cpu->bus;
	return _storeAny(cpu, cpu->index, cpu->bus, pc, at);
}

OP(DEC_HL) {
	USE_ALL;
	if (!_storableAny(cpu, cpu->hl)) {
		return -1;
	}
	int value = _loadAny(cpu, cpu->hl, pc);
	if (value < 0) {
		return -1;
	}
	cpu->index = cpu->hl;
	cpu->bus = value;
	int diff = cpu->bus - 1;
	cpu->f.n = 1;
	cpu->f.h = (cpu->bus & 0xF) == 0;
	cpu->bus = diff;
	cpu->f.z = !cpu->bus;
	return _storeAny(cpu, cpu->index, cpu->bus, pc, at);
}

OP(LD_HL_N) {
	USE_ALL;
	if (!_storableAny(cpu, cpu->hl)) {
		return -1;
	}
	cpu->bus = operand;
	cpu->index = cpu->hl;
	return _storeAny(cpu, cpu->index, cpu->bus, pc, at);
}

// Accumulator rotations and flag operations
OP(RLCA) {
	USE_ALL;
	cpu->a = (cpu->a << 1) | (cpu->a >> 7);
	cpu->f.z = 0;
	cpu->f.h = 0;
	cpu->f.n = 0;
	cpu->f.c = cpu->a & 1;
	cpu->bus = 0x07;
	return 0;
}

OP(RRCA) {
	USE_ALL;
	int low = cpu->a & 1;
	cpu->a = (cpu->a >> 1) | (low << 7);
	cpu->f.z = 0;
	cpu->f.h = 0;
	cpu->f.n = 0;
	cpu->f.c = low;
	cpu->bus = 0x0F;
	return 0;
}

OP(RLA) {
	USE_ALL;
	int wide = (cpu->a << 1) | cpu->f.c;
	cpu->a = wide;
	cpu->f.z = 0;
	cpu->f.h = 0;
	cpu->f.n = 0;
	cpu->f.c = wide >> 8;
	cpu->bus = 0x17;
	return 0;
}

OP(RRA) {
	USE_ALL;
	int low = cpu->a & 1;
	cpu->a = (cpu->a >> 1) | (cpu->f.c << 7);
	cpu->f.z = 0;
	cpu->f.h = 0;
	cpu->f.n = 0;
	cpu->f.c = low;
	cpu->bus = 0x1F;
	return 0;
}

OP(DAA) {
	USE_ALL;
	if (cpu->f.n) {
		if (cpu->f.h) {
			cpu->a += 0xFA;
		}
		if (cpu->f.c) {
			cpu->a += 0xA0;
		}
	} else {
		int a = cpu->a;
		if ((cpu->a & 0xF) > 0x9 || cpu->f.h) {
			a += 0x6;
		}
		if ((a & 0x1F0) > 0x90 || cpu->f.c) {
			a += 0x60;
			cpu->f.c = 1;
		} else {
			cpu->f.c = 0;
		}
		cpu->a = a;
	}
	cpu->f.h = 0;
	cpu->f.z = !cpu->a;
	cpu->bus = 0x27;
	return 0;
}

OP(CPL) {
	USE_ALL;
	cpu->a ^= 0xFF;
	cpu->f.h = 1;
	cpu->f.n = 1;
	cpu->bus = 0x2F;
	return 0;
}

OP(SCF) {
	USE_ALL;
	cpu->f.c = 1;
	cpu->f.h = 0;
	cpu->f.n = 0;
	cpu->bus = 0x37;
	return 0;
}

OP(CCF) {
	USE_ALL;
	cpu->f.c ^= 1;
	cpu->f.h = 0;
	cpu->f.n = 0;
	cpu->bus = 0x3F;
	return 0;
}

// 16-bit register operations
#define DEFINE_INCDEC_WIDE(INC_OP, DEC_OP, REG) \
	OP(INC ## REG) { \
		USE_ALL; \
		cpu->REG = cpu->REG + 1; \
		cpu->bus = 0x ## INC_OP; \
		return 0; \
	} \
	OP(DEC ## REG) { \
		USE_ALL; \
		cpu->REG = cpu->REG - 1; \
		cpu->bus = 0x ## DEC_OP; \
		return 0; \
	}

DEFINE_INCDEC_WIDE(03, 0B, bc)
DEFINE_INCDEC_WIDE(13, 1B, de)
DEFINE_INCDEC_WIDE(23, 2B, hl)
DEFINE_INCDEC_WIDE(33, 3B, sp)

#define DEFINE_ADD_HL(OPCODE, NAME, L, H) \
	OP(ADDHL ## NAME) { \
		USE_ALL; \
		int diff = L + cpu->l; \
		cpu->l = diff; \
		cpu->f.c = diff >= 0x100; \
		diff = H + cpu->h + cpu->f.c; \
		cpu->f.n = 0; \
		cpu->f.h = (H & 0xF) + (cpu->h & 0xF) + cpu->f.c >= 0x10; \
		cpu->f.c = diff >= 0x100; \
		cpu->h = diff; \
		cpu->bus = 0x ## OPCODE; \
		return 0; \
	}

DEFINE_ADD_HL(09, BC, cpu->c, cpu->b)
DEFINE_ADD_HL(19, DE, cpu->e, cpu->d)
DEFINE_ADD_HL(29, HL, cpu->l, cpu->h)
DEFINE_ADD_HL(39, SP, (cpu->sp & 0xFF), (cpu->sp >> 8))

#define DEFINE_LD_WIDE(NAME, HI, LO) \
	OP(LDW ## NAME) { \
		USE_ALL; \
		cpu->LO = operand; \
		cpu->bus = operand >> 8; \
		cpu->HI = cpu->bus; \
		return 0; \
	}

DEFINE_LD_WIDE(BC, b, c)
DEFINE_LD_WIDE(DE, d, e)
DEFINE_LD_WIDE(HL, h, l)

OP(LDWSP) {
	USE_ALL;
	cpu->sp = operand & 0xFF;
	cpu->bus = operand >> 8;
	cpu->sp |= cpu->bus << 8;
	return 0;
}

OP(LDSPHL) {
	USE_ALL;
	cpu->sp = cpu->hl;
	cpu->bus = 0xF9;
	return 0;
}

OP(LDNNSP) {
	USE_ALL;
	uint16_t address = operand;
	if (!_storable(cpu, address) || !_storable(cpu, (uint16_t) (address + 1))) {
		return -1;
	}
	cpu->index = address;
	cpu->bus = cpu->sp;
	_store(cpu, cpu->index, cpu->bus);
	++cpu->index;
	cpu->bus = cpu->sp >> 8;
	_store(cpu, cpu->index, cpu->bus);
	return 0;
}

OP(ADDSP) {
	USE_ALL;
	cpu->bus = operand;
	int diff = (int8_t) cpu->bus;
	int sum = cpu->sp + diff;
	cpu->index = sum;
	cpu->f.z = 0;
	cpu->f.n = 0;
	cpu->f.c = (diff & 0xFF) + (cpu->sp & 0xFF) >= 0x100;
	cpu->f.h = (diff & 0xF) + (cpu->sp & 0xF) >= 0x10;
	cpu->sp = cpu->index;
	return 0;
}

OP(LDHLSP) {
	USE_ALL;
	cpu->bus = operand;
	int diff = (int8_t) cpu->bus;
	int sum = cpu->sp + diff;
	cpu->hl = sum;
	cpu->f.z = 0;
	cpu->f.n = 0;
	cpu->f.c = (diff & 0xFF) + (cpu->sp & 0xFF) >= 0x100;
	cpu->f.h = (diff & 0xF) + (cpu->sp & 0xF) >= 0x10;
	return 0;
}

// Accumulator loads and stores through BC, DE, HL+/-, immediate and high addresses
#define DEFINE_LDA_IND(NAME, ADDRESS, AFTER) \
	OP(LDA ## NAME) { \
		USE_ALL; \
		uint16_t address = ADDRESS; \
		int value = _loadAny(cpu, address, pc); \
		if (value < 0) { \
			return -1; \
		} \
		cpu->index = address; \
		AFTER; \
		cpu->bus = value; \
		cpu->a = value; \
		return 0; \
	}

#define DEFINE_STA_IND(NAME, ADDRESS, AFTER) \
	OP(STA ## NAME) { \
		USE_ALL; \
		uint16_t address = ADDRESS; \
		if (!_storableAny(cpu, address)) { \
			return -1; \
		} \
		cpu->index = address; \
		AFTER; \
		cpu->bus = cpu->a; \
		return _storeAny(cpu, cpu->index, cpu->bus, pc, at); \
	}

DEFINE_LDA_IND(BC, cpu->bc, )
DEFINE_LDA_IND(DE, cpu->de, )
DEFINE_LDA_IND(HLI, cpu->hl, cpu->hl = cpu->index + 1)
DEFINE_LDA_IND(HLD, cpu->hl, cpu->hl = cpu->index - 1)
DEFINE_LDA_IND(NN, operand, )
DEFINE_LDA_IND(IO, 0xFF00 | operand, )
DEFINE_LDA_IND(IOC, 0xFF00 | cpu->c, )
DEFINE_STA_IND(BC, cpu->bc, )
DEFINE_STA_IND(DE, cpu->de, )
DEFINE_STA_IND(HLI, cpu->hl, cpu->hl = cpu->index + 1)
DEFINE_STA_IND(HLD, cpu->hl, cpu->hl = cpu->index - 1)
DEFINE_STA_IND(NN, operand, )
DEFINE_STA_IND(IO, 0xFF00 | operand, )
DEFINE_STA_IND(IOC, 0xFF00 | cpu->c, )

// Stack
#define DEFINE_PUSH(OPCODE, NAME, H, L) \
	OP(PUSH ## NAME) { \
		USE_ALL; \
		if (!_storable(cpu, (uint16_t) (cpu->sp - 1)) || !_storable(cpu, (uint16_t) (cpu->sp - 2))) { \
			return -1; \
		} \
		--cpu->sp; \
		cpu->index = cpu->sp; \
		cpu->bus = H; \
		_store(cpu, cpu->index, cpu->bus); \
		--cpu->sp; \
		cpu->index = cpu->sp; \
		cpu->bus = L; \
		_store(cpu, cpu->index, cpu->bus); \
		return 0; \
	}

#define DEFINE_POP(OPCODE, NAME, H, L) \
	OP(POP ## NAME) { \
		USE_ALL; \
		int low = _load(cpu, cpu->sp, pc); \
		if (low < 0) { \
			return -1; \
		} \
		int high = _load(cpu, (uint16_t) (cpu->sp + 1), pc); \
		if (high < 0) { \
			return -1; \
		} \
		cpu->index = cpu->sp; \
		++cpu->sp; \
		cpu->bus = low; \
		L = cpu->bus; \
		cpu->f.packed &= 0xF0; \
		cpu->index = cpu->sp; \
		++cpu->sp; \
		cpu->bus = high; \
		H = cpu->bus; \
		return 0; \
	}

DEFINE_PUSH(C5, BC, cpu->b, cpu->c) DEFINE_POP(C1, BC, cpu->b, cpu->c)
DEFINE_PUSH(D5, DE, cpu->d, cpu->e) DEFINE_POP(D1, DE, cpu->d, cpu->e)
DEFINE_PUSH(E5, HL, cpu->h, cpu->l) DEFINE_POP(E1, HL, cpu->h, cpu->l)
DEFINE_PUSH(F5, AF, cpu->a, cpu->f.packed) DEFINE_POP(F1, AF, cpu->a, cpu->f.packed)

// Control flow: every one ends a block, sets pc and returns its own M-cycles
#define COND_ALWAYS true
#define COND_NZ !cpu->f.z
#define COND_Z cpu->f.z
#define COND_NC !cpu->f.c
#define COND_C cpu->f.c

#define DEFINE_JP(NAME, COND) \
	OP(JP ## NAME) { \
		USE_ALL; \
		cpu->condition = COND; \
		cpu->index = operand & 0xFF; \
		cpu->bus = operand >> 8; \
		if (cpu->condition) { \
			cpu->pc = (cpu->bus << 8) | cpu->index; \
			cpu->memory.setActiveRegion(cpu, cpu->pc); \
			return 4; \
		} \
		cpu->pc = pc; \
		return 3; \
	}

#define DEFINE_JR(NAME, COND) \
	OP(JR ## NAME) { \
		USE_ALL; \
		cpu->condition = COND; \
		cpu->bus = operand; \
		cpu->pc = pc; \
		if (cpu->condition) { \
			cpu->pc += (int8_t) cpu->bus; \
			cpu->memory.setActiveRegion(cpu, cpu->pc); \
			return 3; \
		} \
		return 2; \
	}

#define DEFINE_CALL(NAME, COND) \
	OP(CALL ## NAME) { \
		USE_ALL; \
		cpu->condition = COND; \
		if (cpu->condition && (!_storable(cpu, (uint16_t) (cpu->sp - 1)) || !_storable(cpu, (uint16_t) (cpu->sp - 2)))) { \
			return -1; \
		} \
		cpu->index = operand & 0xFF; \
		cpu->bus = operand >> 8; \
		cpu->pc = pc; \
		if (!cpu->condition) { \
			return 3; \
		} \
		int newPc = (cpu->bus << 8) | cpu->index; \
		cpu->bus = cpu->pc >> 8; \
		cpu->index = cpu->sp - 1; \
		cpu->sp = cpu->pc; \
		cpu->pc = newPc; \
		cpu->memory.setActiveRegion(cpu, cpu->pc); \
		_store(cpu, cpu->index, cpu->bus); \
		--cpu->index; \
		cpu->bus = cpu->sp; \
		cpu->sp = cpu->index; \
		_store(cpu, cpu->index, cpu->bus); \
		return 6; \
	}

// The second stack read happens after pc took the low byte, which the open bus sees
#define DEFINE_RET(NAME, COND, TAKEN, NOT_TAKEN) \
	OP(RET ## NAME) { \
		USE_ALL; \
		cpu->condition = COND; \
		if (!cpu->condition) { \
			cpu->pc = pc; \
			cpu->bus = operand; \
			return NOT_TAKEN; \
		} \
		struct GBMemory* memory = &((struct GB*) cpu->master)->memory; \
		uint8_t cartBus = memory->cartBus; \
		uint16_t cartBusPc = memory->cartBusPc; \
		int low = _load(cpu, cpu->sp, pc); \
		if (low < 0) { \
			return -1; \
		} \
		int high = _load(cpu, (uint16_t) (cpu->sp + 1), low); \
		if (high < 0) { \
			memory->cartBus = cartBus; \
			memory->cartBusPc = cartBusPc; \
			return -1; \
		} \
		cpu->index = cpu->sp + 1; \
		cpu->bus = high; \
		cpu->sp += 2; \
		cpu->pc = low | (high << 8); \
		cpu->memory.setActiveRegion(cpu, cpu->pc); \
		return TAKEN; \
	}

DEFINE_JP(, COND_ALWAYS) DEFINE_JP(NZ, COND_NZ) DEFINE_JP(Z, COND_Z) DEFINE_JP(NC, COND_NC) DEFINE_JP(C, COND_C)
DEFINE_JR(, COND_ALWAYS) DEFINE_JR(NZ, COND_NZ) DEFINE_JR(Z, COND_Z) DEFINE_JR(NC, COND_NC) DEFINE_JR(C, COND_C)
DEFINE_CALL(, COND_ALWAYS) DEFINE_CALL(NZ, COND_NZ) DEFINE_CALL(Z, COND_Z) DEFINE_CALL(NC, COND_NC) DEFINE_CALL(C, COND_C)
DEFINE_RET(, COND_ALWAYS, 4, 0) DEFINE_RET(NZ, COND_NZ, 5, 2) DEFINE_RET(Z, COND_Z, 5, 2) DEFINE_RET(NC, COND_NC, 5, 2) DEFINE_RET(C, COND_C, 5, 2)

OP(JPHL) {
	USE_ALL;
	cpu->pc = cpu->hl;
	cpu->memory.setActiveRegion(cpu, cpu->pc);
	cpu->bus = 0xE9;
	return 1;
}

#define DEFINE_RST(VEC) \
	OP(RST ## VEC) { \
		USE_ALL; \
		if (!_storable(cpu, (uint16_t) (cpu->sp - 1)) || !_storable(cpu, (uint16_t) (cpu->sp - 2))) { \
			return -1; \
		} \
		cpu->pc = pc; \
		--cpu->sp; \
		cpu->index = cpu->sp; \
		cpu->bus = cpu->pc >> 8; \
		_store(cpu, cpu->index, cpu->bus); \
		--cpu->sp; \
		cpu->index = cpu->sp; \
		cpu->bus = cpu->pc; \
		cpu->pc = 0x ## VEC; \
		cpu->memory.setActiveRegion(cpu, cpu->pc); \
		_store(cpu, cpu->index, cpu->bus); \
		return 4; \
	}

DEFINE_RST(00) DEFINE_RST(08) DEFINE_RST(10) DEFINE_RST(18) DEFINE_RST(20) DEFINE_RST(28) DEFINE_RST(30) DEFINE_RST(38)

// CB prefix: operand is the second opcode
#define CB_REG(I) (I == 0 ? cpu->b : I == 1 ? cpu->c : I == 2 ? cpu->d : I == 3 ? cpu->e : I == 4 ? cpu->h : I == 5 ? cpu->l : cpu->a)
#define CB_SET_REG(I, V) do { \
		switch (I) { \
		case 0: cpu->b = V; break; \
		case 1: cpu->c = V; break; \
		case 2: cpu->d = V; break; \
		case 3: cpu->e = V; break; \
		case 4: cpu->h = V; break; \
		case 5: cpu->l = V; break; \
		default: cpu->a = V; break; \
		} \
	} while (0)

static inline uint8_t _cbAlu(struct SM83Core* cpu, unsigned op, uint8_t reg) {
	switch (op) {
	case 0: // RLC
		reg = (reg << 1) | (reg >> 7);
		cpu->f.c = reg & 1;
		break;
	case 1: { // RRC
		int low = reg & 1;
		reg = (reg >> 1) | (low << 7);
		cpu->f.c = low;
		break;
	}
	case 2: { // RL
		int wide = (reg << 1) | cpu->f.c;
		reg = wide;
		cpu->f.c = wide >> 8;
		break;
	}
	case 3: { // RR
		int low = reg & 1;
		reg = (reg >> 1) | (cpu->f.c << 7);
		cpu->f.c = low;
		break;
	}
	case 4: // SLA
		cpu->f.c = reg >> 7;
		reg <<= 1;
		break;
	case 5: // SRA
		cpu->f.c = reg & 1;
		reg = ((int8_t) reg) >> 1;
		break;
	case 6: // SWAP
		reg = (reg << 4) | (reg >> 4);
		cpu->f.c = 0;
		break;
	default: // SRL
		cpu->f.c = reg & 1;
		reg >>= 1;
		break;
	}
	cpu->f.n = 0;
	cpu->f.h = 0;
	cpu->f.z = !reg;
	return reg;
}

static inline uint8_t _cbOp(struct SM83Core* cpu, unsigned op, uint8_t reg) {
	unsigned group = op >> 6;
	uint8_t bit = 1 << ((op >> 3) & 7);
	switch (group) {
	case 0:
		return _cbAlu(cpu, (op >> 3) & 7, reg);
	case 1:
		cpu->f.n = 0;
		cpu->f.h = 1;
		cpu->f.z = !(reg & bit);
		return reg;
	case 2:
		return reg & ~bit;
	default:
		return reg | bit;
	}
}

#define DEFINE_CB(OPCODE) \
	OP(CB ## OPCODE) { \
		USE_ALL; \
		const unsigned op = 0x ## OPCODE, r = op & 7; \
		if (r != 6) { \
			uint8_t value = _cbOp(cpu, op, CB_REG(r)); \
			if ((op >> 6) != 1) { \
				CB_SET_REG(r, value); \
			} \
			cpu->bus = op; \
			return 0; \
		} \
		if ((op >> 6) != 1 && !_storableAny(cpu, cpu->hl)) { \
			return -1; \
		} \
		int loaded = _loadAny(cpu, cpu->hl, pc); \
		if (loaded < 0) { \
			return -1; \
		} \
		cpu->index = cpu->hl; \
		cpu->bus = _cbOp(cpu, op, loaded); \
		if ((op >> 6) != 1) { \
			return _storeAny(cpu, cpu->index, cpu->bus, pc, at); \
		} \
		return 0; \
	}

#define DEFINE_CB_ROW(HI) \
	DEFINE_CB(HI ## 0) DEFINE_CB(HI ## 1) DEFINE_CB(HI ## 2) DEFINE_CB(HI ## 3) DEFINE_CB(HI ## 4) DEFINE_CB(HI ## 5) DEFINE_CB(HI ## 6) DEFINE_CB(HI ## 7) \
	DEFINE_CB(HI ## 8) DEFINE_CB(HI ## 9) DEFINE_CB(HI ## A) DEFINE_CB(HI ## B) DEFINE_CB(HI ## C) DEFINE_CB(HI ## D) DEFINE_CB(HI ## E) DEFINE_CB(HI ## F)

DEFINE_CB_ROW(0) DEFINE_CB_ROW(1) DEFINE_CB_ROW(2) DEFINE_CB_ROW(3) DEFINE_CB_ROW(4) DEFINE_CB_ROW(5) DEFINE_CB_ROW(6) DEFINE_CB_ROW(7)
DEFINE_CB_ROW(8) DEFINE_CB_ROW(9) DEFINE_CB_ROW(A) DEFINE_CB_ROW(B) DEFINE_CB_ROW(C) DEFINE_CB_ROW(D) DEFINE_CB_ROW(E) DEFINE_CB_ROW(F)

#define CB_ENTRY(HI) \
	_opCB ## HI ## 0, _opCB ## HI ## 1, _opCB ## HI ## 2, _opCB ## HI ## 3, _opCB ## HI ## 4, _opCB ## HI ## 5, _opCB ## HI ## 6, _opCB ## HI ## 7, \
	_opCB ## HI ## 8, _opCB ## HI ## 9, _opCB ## HI ## A, _opCB ## HI ## B, _opCB ## HI ## C, _opCB ## HI ## D, _opCB ## HI ## E, _opCB ## HI ## F

static const GBJitOp _cbOps[0x100] = {
	CB_ENTRY(0), CB_ENTRY(1), CB_ENTRY(2), CB_ENTRY(3), CB_ENTRY(4), CB_ENTRY(5), CB_ENTRY(6), CB_ENTRY(7),
	CB_ENTRY(8), CB_ENTRY(9), CB_ENTRY(A), CB_ENTRY(B), CB_ENTRY(C), CB_ENTRY(D), CB_ENTRY(E), CB_ENTRY(F),
};

enum {
	KIND_PLAIN = 0,
	KIND_LOAD = 1,
	KIND_STORE = 2,
	KIND_BRANCH = 4,
};

struct GBJitOpInfo {
	GBJitOp op;
	uint8_t length;
	uint8_t cycles;
	uint8_t maxCycles;
	uint8_t kind;
};

#define I(NAME, LENGTH, CYCLES, KIND) { _op ## NAME, LENGTH, CYCLES, CYCLES, KIND }
#define B(NAME, LENGTH, CYCLES, MAX, KIND) { _op ## NAME, LENGTH, CYCLES, MAX, KIND | KIND_BRANCH }
#define X { NULL, 1, 1, 1, 0 }
#define L KIND_LOAD
#define S KIND_STORE
#define LS (KIND_LOAD | KIND_STORE)

// M-cycles as the interpreter's stage functions take them; branches list their longest
static const struct GBJitOpInfo _ops[0x100] = {
	/* 0x00 */ I(NOP, 1, 1, 0), I(LDWBC, 3, 3, 0), I(STABC, 1, 2, S), I(INCbc, 1, 2, 0), I(INC04, 1, 1, 0), I(DEC05, 1, 1, 0), I(LDN06, 2, 2, 0), I(RLCA, 1, 1, 0),
	/* 0x08 */ I(LDNNSP, 3, 5, S), I(ADDHLBC, 1, 2, 0), I(LDABC, 1, 2, L), I(DECbc, 1, 2, 0), I(INC0C, 1, 1, 0), I(DEC0D, 1, 1, 0), I(LDN0E, 2, 2, 0), I(RRCA, 1, 1, 0),
	/* 0x10 */ X, I(LDWDE, 3, 3, 0), I(STADE, 1, 2, S), I(INCde, 1, 2, 0), I(INC14, 1, 1, 0), I(DEC15, 1, 1, 0), I(LDN16, 2, 2, 0), I(RLA, 1, 1, 0),
	/* 0x18 */ B(JR, 2, 0, 3, 0), I(ADDHLDE, 1, 2, 0), I(LDADE, 1, 2, L), I(DECde, 1, 2, 0), I(INC1C, 1, 1, 0), I(DEC1D, 1, 1, 0), I(LDN1E, 2, 2, 0), I(RRA, 1, 1, 0),
	/* 0x20 */ B(JRNZ, 2, 0, 3, 0), I(LDWHL, 3, 3, 0), I(STAHLI, 1, 2, S), I(INChl, 1, 2, 0), I(INC24, 1, 1, 0), I(DEC25, 1, 1, 0), I(LDN26, 2, 2, 0), I(DAA, 1, 1, 0),
	/* 0x28 */ B(JRZ, 2, 0, 3, 0), I(ADDHLHL, 1, 2, 0), I(LDAHLI, 1, 2, L), I(DEChl, 1, 2, 0), I(INC2C, 1, 1, 0), I(DEC2D, 1, 1, 0), I(LDN2E, 2, 2, 0), I(CPL, 1, 1, 0),
	/* 0x30 */ B(JRNC, 2, 0, 3, 0), I(LDWSP, 3, 3, 0), I(STAHLD, 1, 2, S), I(INCsp, 1, 2, 0), I(INC_HL, 1, 3, LS), I(DEC_HL, 1, 3, LS), I(LD_HL_N, 2, 3, S), I(SCF, 1, 1, 0),
	/* 0x38 */ B(JRC, 2, 0, 3, 0), I(ADDHLSP, 1, 2, 0), I(LDAHLD, 1, 2, L), I(DECsp, 1, 2, 0), I(INC3C, 1, 1, 0), I(DEC3D, 1, 1, 0), I(LDN3E, 2, 2, 0), I(CCF, 1, 1, 0),
	/* 0x40 */ I(LD40, 1, 1, 0), I(LD41, 1, 1, 0), I(LD42, 1, 1, 0), I(LD43, 1, 1, 0), I(LD44, 1, 1, 0), I(LD45, 1, 1, 0), I(LD46, 1, 2, L), I(LD47, 1, 1, 0),
	/* 0x48 */ I(LD48, 1, 1, 0), I(LD49, 1, 1, 0), I(LD4A, 1, 1, 0), I(LD4B, 1, 1, 0), I(LD4C, 1, 1, 0), I(LD4D, 1, 1, 0), I(LD4E, 1, 2, L), I(LD4F, 1, 1, 0),
	/* 0x50 */ I(LD50, 1, 1, 0), I(LD51, 1, 1, 0), I(LD52, 1, 1, 0), I(LD53, 1, 1, 0), I(LD54, 1, 1, 0), I(LD55, 1, 1, 0), I(LD56, 1, 2, L), I(LD57, 1, 1, 0),
	/* 0x58 */ I(LD58, 1, 1, 0), I(LD59, 1, 1, 0), I(LD5A, 1, 1, 0), I(LD5B, 1, 1, 0), I(LD5C, 1, 1, 0), I(LD5D, 1, 1, 0), I(LD5E, 1, 2, L), I(LD5F, 1, 1, 0),
	/* 0x60 */ I(LD60, 1, 1, 0), I(LD61, 1, 1, 0), I(LD62, 1, 1, 0), I(LD63, 1, 1, 0), I(LD64, 1, 1, 0), I(LD65, 1, 1, 0), I(LD66, 1, 2, L), I(LD67, 1, 1, 0),
	/* 0x68 */ I(LD68, 1, 1, 0), I(LD69, 1, 1, 0), I(LD6A, 1, 1, 0), I(LD6B, 1, 1, 0), I(LD6C, 1, 1, 0), I(LD6D, 1, 1, 0), I(LD6E, 1, 2, L), I(LD6F, 1, 1, 0),
	/* 0x70 */ I(LD70, 1, 2, S), I(LD71, 1, 2, S), I(LD72, 1, 2, S), I(LD73, 1, 2, S), I(LD74, 1, 2, S), I(LD75, 1, 2, S), X, I(LD77, 1, 2, S),
	/* 0x78 */ I(LD78, 1, 1, 0), I(LD79, 1, 1, 0), I(LD7A, 1, 1, 0), I(LD7B, 1, 1, 0), I(LD7C, 1, 1, 0), I(LD7D, 1, 1, 0), I(LD7E, 1, 2, L), I(LD7F, 1, 1, 0),
	/* 0x80 */ I(ALU_ADD80, 1, 1, 0), I(ALU_ADD81, 1, 1, 0), I(ALU_ADD82, 1, 1, 0), I(ALU_ADD83, 1, 1, 0), I(ALU_ADD84, 1, 1, 0), I(ALU_ADD85, 1, 1, 0), I(ALU_ADD86, 1, 2, L), I(ALU_ADD87, 1, 1, 0),
	/* 0x88 */ I(ALU_ADC88, 1, 1, 0), I(ALU_ADC89, 1, 1, 0), I(ALU_ADC8A, 1, 1, 0), I(ALU_ADC8B, 1, 1, 0), I(ALU_ADC8C, 1, 1, 0), I(ALU_ADC8D, 1, 1, 0), I(ALU_ADC8E, 1, 2, L), I(ALU_ADC8F, 1, 1, 0),
	/* 0x90 */ I(ALU_SUB90, 1, 1, 0), I(ALU_SUB91, 1, 1, 0), I(ALU_SUB92, 1, 1, 0), I(ALU_SUB93, 1, 1, 0), I(ALU_SUB94, 1, 1, 0), I(ALU_SUB95, 1, 1, 0), I(ALU_SUB96, 1, 2, L), I(ALU_SUB97, 1, 1, 0),
	/* 0x98 */ I(ALU_SBC98, 1, 1, 0), I(ALU_SBC99, 1, 1, 0), I(ALU_SBC9A, 1, 1, 0), I(ALU_SBC9B, 1, 1, 0), I(ALU_SBC9C, 1, 1, 0), I(ALU_SBC9D, 1, 1, 0), I(ALU_SBC9E, 1, 2, L), I(ALU_SBC9F, 1, 1, 0),
	/* 0xA0 */ I(ALU_ANDA0, 1, 1, 0), I(ALU_ANDA1, 1, 1, 0), I(ALU_ANDA2, 1, 1, 0), I(ALU_ANDA3, 1, 1, 0), I(ALU_ANDA4, 1, 1, 0), I(ALU_ANDA5, 1, 1, 0), I(ALU_ANDA6, 1, 2, L), I(ALU_ANDA7, 1, 1, 0),
	/* 0xA8 */ I(ALU_XORA8, 1, 1, 0), I(ALU_XORA9, 1, 1, 0), I(ALU_XORAA, 1, 1, 0), I(ALU_XORAB, 1, 1, 0), I(ALU_XORAC, 1, 1, 0), I(ALU_XORAD, 1, 1, 0), I(ALU_XORAE, 1, 2, L), I(ALU_XORAF, 1, 1, 0),
	/* 0xB0 */ I(ALU_ORB0, 1, 1, 0), I(ALU_ORB1, 1, 1, 0), I(ALU_ORB2, 1, 1, 0), I(ALU_ORB3, 1, 1, 0), I(ALU_ORB4, 1, 1, 0), I(ALU_ORB5, 1, 1, 0), I(ALU_ORB6, 1, 2, L), I(ALU_ORB7, 1, 1, 0),
	/* 0xB8 */ I(ALU_CPB8, 1, 1, 0), I(ALU_CPB9, 1, 1, 0), I(ALU_CPBA, 1, 1, 0), I(ALU_CPBB, 1, 1, 0), I(ALU_CPBC, 1, 1, 0), I(ALU_CPBD, 1, 1, 0), I(ALU_CPBE, 1, 2, L), I(ALU_CPBF, 1, 1, 0),
	/* 0xC0 */ B(RETNZ, 1, 0, 5, L), I(POPBC, 1, 3, L), B(JPNZ, 3, 0, 4, 0), B(JP, 3, 0, 4, 0), B(CALLNZ, 3, 0, 6, S), I(PUSHBC, 1, 4, S), I(ALU_ADDC6, 2, 2, 0), B(RST00, 1, 0, 4, S),
	/* 0xC8 */ B(RETZ, 1, 0, 5, L), B(RET, 1, 0, 4, L), B(JPZ, 3, 0, 4, 0), X, B(CALLZ, 3, 0, 6, S), B(CALL, 3, 0, 6, S), I(ALU_ADCCE, 2, 2, 0), B(RST08, 1, 0, 4, S),
	/* 0xD0 */ B(RETNC, 1, 0, 5, L), I(POPDE, 1, 3, L), B(JPNC, 3, 0, 4, 0), X, B(CALLNC, 3, 0, 6, S), I(PUSHDE, 1, 4, S), I(ALU_SUBD6, 2, 2, 0), B(RST10, 1, 0, 4, S),
	/* 0xD8 */ B(RETC, 1, 0, 5, L), X, B(JPC, 3, 0, 4, 0), X, B(CALLC, 3, 0, 6, S), X, I(ALU_SBCDE, 2, 2, 0), B(RST18, 1, 0, 4, S),
	/* 0xE0 */ I(STAIO, 2, 3, S), I(POPHL, 1, 3, L), I(STAIOC, 1, 2, S), X, X, I(PUSHHL, 1, 4, S), I(ALU_ANDE6, 2, 2, 0), B(RST20, 1, 0, 4, S),
	/* 0xE8 */ I(ADDSP, 2, 4, 0), B(JPHL, 1, 0, 1, 0), I(STANN, 3, 4, S), X, X, X, I(ALU_XOREE, 2, 2, 0), B(RST28, 1, 0, 4, S),
	/* 0xF0 */ I(LDAIO, 2, 3, L), I(POPAF, 1, 3, L), I(LDAIOC, 1, 2, L), X, X, I(PUSHAF, 1, 4, S), I(ALU_ORF6, 2, 2, 0), B(RST30, 1, 0, 4, S),
	/* 0xF8 */ I(LDHLSP, 2, 3, 0), I(LDSPHL, 1, 2, 0), I(LDANN, 3, 4, L), X, X, X, I(ALU_CPFE, 2, 2, 0), B(RST38, 1, 0, 4, S),
};

#undef I
#undef B
#undef X
#undef L
#undef S
#undef LS

// Code generation
#define CODE_SIZE (16 * 1024 * 1024)
#define BLOCK_MAX 64
#define HOT_THRESHOLD 4
#define BLOCK_BYTES 384
#define INSN_BYTES 192
#define BAILED 0x10000

struct GBJitEntry {
	void* code;
	uint16_t pc;
	uint8_t hits;
	bool failed;
	bool bails;
};

struct GBJit {
	struct GB* gb;
	struct GBJitEntry* bailed;
	bool noInline;
	uint8_t* code;
	size_t used;
	const uint8_t* rom;
	size_t romSize;
	struct GBJitEntry** pages;
	size_t nPages;
};

struct Emitter {
	uint8_t* p;
};

static void _byte(struct Emitter* e, uint8_t b) {
	*e->p = b;
	++e->p;
}

static void _word32(struct Emitter* e, uint32_t v) {
	_byte(e, v);
	_byte(e, v >> 8);
	_byte(e, v >> 16);
	_byte(e, v >> 24);
}

static void _word64(struct Emitter* e, uint64_t v) {
	_word32(e, v);
	_word32(e, v >> 32);
}

static void _call(struct Emitter* e, const void* function) {
	intptr_t rel = (intptr_t) function - (intptr_t) (e->p + 5);
	if (rel == (int32_t) rel) {
		_byte(e, 0xE8);
		_word32(e, rel);
		return;
	}
	// mov rax, imm64; call rax
	_byte(e, 0x48);
	_byte(e, 0xB8);
	_word64(e, (uintptr_t) function);
	_byte(e, 0xFF);
	_byte(e, 0xD0);
}

// Field of the core at [rbx + offset]
static void _storeHalf(struct Emitter* e, size_t offset, uint16_t value) {
	_byte(e, 0x66);
	_byte(e, 0xC7);
	_byte(e, 0x83);
	_word32(e, offset);
	_byte(e, value);
	_byte(e, value >> 8);
}

// Fetch latches of the cartridge bus at [r12 + offset]
static void _storeCartBus(struct Emitter* e, uint8_t value, uint16_t pc) {
	_byte(e, 0x41);
	_byte(e, 0xC6);
	_byte(e, 0x84);
	_byte(e, 0x24);
	_word32(e, offsetof(struct GBMemory, cartBus));
	_byte(e, value);
	_byte(e, 0x66);
	_byte(e, 0x41);
	_byte(e, 0xC7);
	_byte(e, 0x84);
	_byte(e, 0x24);
	_word32(e, offsetof(struct GBMemory, cartBusPc));
	_byte(e, pc);
	_byte(e, pc >> 8);
}

static uint8_t* _jcc32(struct Emitter* e, uint8_t condition) {
	_byte(e, 0x0F);
	_byte(e, condition);
	uint8_t* patch = e->p;
	_word32(e, 0);
	return patch;
}

static uint8_t* _jmp32(struct Emitter* e) {
	_byte(e, 0xE9);
	uint8_t* patch = e->p;
	_word32(e, 0);
	return patch;
}

static void _patch(uint8_t* patch, uint8_t* target) {
	int32_t rel = target - (patch + 4);
	memcpy(patch, &rel, 4);
}

// cycles += eax M-cycles at the current speed, keeping the count in edx
static void _addCycles(struct Emitter* e) {
	// mov edx, eax; mov ecx, [rbx + tMultiplier]; imul eax, ecx; shl eax, 2; add [rbx + cycles], eax
	_byte(e, 0x89);
	_byte(e, 0xC2);
	_byte(e, 0x8B);
	_byte(e, 0x8B);
	_word32(e, offsetof(struct SM83Core, tMultiplier));
	_byte(e, 0x0F);
	_byte(e, 0xAF);
	_byte(e, 0xC1);
	_byte(e, 0xC1);
	_byte(e, 0xE0);
	_byte(e, 0x02);
	_byte(e, 0x01);
	_byte(e, 0x83);
	_word32(e, offsetof(struct SM83Core, cycles));
}

static void _return(struct Emitter* e) {
	// pop r13; pop r12; pop rbx; ret
	_byte(e, 0x41);
	_byte(e, 0x5D);
	_byte(e, 0x41);
	_byte(e, 0x5C);
	_byte(e, 0x5B);
	_byte(e, 0xC3);
}

struct Decoded {
	const struct GBJitOpInfo* info;
	uint8_t opcode;
	uint16_t pc;
	uint32_t operand;
	uint8_t lastByte;
	uint8_t length;
};

static struct GBJitOpInfo _cbInfo[0x100];

static bool _decode(const uint8_t* rom, uint16_t pc, uint16_t end, struct Decoded* out) {
	uint8_t opcode = rom[0];
	if (opcode == 0xCB) {
		if ((uint32_t) pc + 2 > end) {
			return false;
		}
		out->info = &_cbInfo[rom[1]];
		out->operand = rom[1];
	} else {
		const struct GBJitOpInfo* info = &_ops[opcode];
		if (!info->op || (uint32_t) pc + info->length > end) {
			return false;
		}
		out->info = info;
		out->operand = info->length == 3 ? rom[1] | (rom[2] << 8) : info->length == 2 ? rom[1] : opcode;
	}
	out->opcode = opcode;
	out->pc = pc;
	out->length = out->info->length;
	out->lastByte = rom[out->length - 1];
	return true;
}

static struct GBJitEntry* _entry(struct GBJit* jit, size_t offset) {
	struct GBJitEntry* page = jit->pages[offset >> 8];
	if (!page) {
		page = calloc(0x100, sizeof(*page));
		jit->pages[offset >> 8] = page;
	}
	return &page[offset & 0xFF];
}

static bool _bails(struct GBJit* jit, size_t offset) {
	struct GBJitEntry* page = jit->pages[offset >> 8];
	return page && page[offset & 0xFF].bails;
}

// Native forms of the commonest instructions, with the same registers, flags and latches the handlers leave
#define FLAG_TABLE_BYTES 256
#define REG_F offsetof(struct SM83Core, f)
#define REG_A offsetof(struct SM83Core, a)

static const size_t _regOffsets[8] = {
	offsetof(struct SM83Core, b), offsetof(struct SM83Core, c), offsetof(struct SM83Core, d), offsetof(struct SM83Core, e),
	offsetof(struct SM83Core, h), offsetof(struct SM83Core, l), 0, offsetof(struct SM83Core, a)
};

// opcode with a ModRM operand of [rbx + offset]
static void _rm(struct Emitter* e, uint8_t opcode, int reg, size_t offset) {
	_byte(e, opcode);
	if (offset < 0x80) {
		_byte(e, 0x43 | (reg << 3));
		_byte(e, offset);
	} else {
		_byte(e, 0x83 | (reg << 3));
		_word32(e, offset);
	}
}

static void _setByte(struct Emitter* e, size_t offset, uint8_t value) {
	_rm(e, 0xC6, 0, offset);
	_byte(e, value);
}

static void _setHalf(struct Emitter* e, size_t offset, uint16_t value) {
	_byte(e, 0x66);
	_rm(e, 0xC7, 0, offset);
	_byte(e, value);
	_byte(e, value >> 8);
}

// cl = Z, H and C from the host flags as SM83 flags, through the table at the start of the code buffer
static void _hostFlags(struct Emitter* e, struct GBJit* jit) {
	// lahf; movzx ecx, ah; lea rdx, [rip + table]; movzx ecx, byte [rdx + rcx]
	_byte(e, 0x9F);
	_byte(e, 0x0F);
	_byte(e, 0xB6);
	_byte(e, 0xCC);
	_byte(e, 0x48);
	_byte(e, 0x8D);
	_byte(e, 0x15);
	_word32(e, (int32_t) (jit->code - (e->p + 4)));
	_byte(e, 0x0F);
	_byte(e, 0xB6);
	_byte(e, 0x0C);
	_byte(e, 0x0A);
}

// f = cl | (f & keep)
static void _mergeFlags(struct Emitter* e, uint8_t keep) {
	// mov dl, [f]; and dl, keep; or cl, dl; mov [f], cl
	_rm(e, 0x8A, 2, REG_F);
	_byte(e, 0x80);
	_byte(e, 0xE2);
	_byte(e, keep);
	_byte(e, 0x08);
	_byte(e, 0xD1);
	_rm(e, 0x88, 1, REG_F);
}

// cl = Z << 7 from the host zero flag
static void _zeroFlag(struct Emitter* e) {
	// setz cl; shl cl, 7
	_byte(e, 0x0F);
	_byte(e, 0x94);
	_byte(e, 0xC1);
	_byte(e, 0xC0);
	_byte(e, 0xE1);
	_byte(e, 0x07);
}

static bool _inline(struct Emitter* e, struct GBJit* jit, const struct Decoded* insn) {
	if (jit->noInline) {
		return false;
	}
	uint8_t op = insn->opcode;
	if (op == 0x00) {
		_setByte(e, offsetof(struct SM83Core, bus), 0x00);
		return true;
	}
	if (op >= 0x40 && op < 0x80 && (op & 7) != 6 && (op & 0x38) != 0x30) {
		// LD r, r
		_rm(e, 0x8A, 0, _regOffsets[op & 7]);
		_rm(e, 0x88, 0, _regOffsets[(op >> 3) & 7]);
		_setByte(e, offsetof(struct SM83Core, bus), op);
		return true;
	}
	if ((op & 0xC7) == 0x06 && op != 0x36) {
		// LD r, n
		_setByte(e, _regOffsets[(op >> 3) & 7], insn->operand);
		_setByte(e, offsetof(struct SM83Core, bus), insn->operand);
		return true;
	}
	if ((op & 0xC6) == 0x04 && op != 0x34 && op != 0x35) {
		// INC r / DEC r: carry kept
		size_t reg = _regOffsets[(op >> 3) & 7];
		_rm(e, 0x8A, 0, reg);
		_byte(e, 0xFE);
		_byte(e, (op & 1) ? 0xC8 : 0xC0);
		_hostFlags(e, jit);
		// and cl, Z|H
		_byte(e, 0x80);
		_byte(e, 0xE1);
		_byte(e, 0xA0);
		if (op & 1) {
			// or cl, N
			_byte(e, 0x80);
			_byte(e, 0xC9);
			_byte(e, 0x40);
		}
		_mergeFlags(e, 0x1F);
		_rm(e, 0x88, 0, reg);
		_setByte(e, offsetof(struct SM83Core, bus), op);
		return true;
	}
	bool imm = (op & 0xC7) == 0xC6;
	if ((op >= 0x80 && op < 0xC0 && (op & 7) != 6) || imm) {
		// ALU A, r / A, n
		unsigned alu = (op >> 3) & 7;
		_rm(e, 0x8A, 0, REG_A);
		if (imm) {
			// mov cl, n
			_byte(e, 0xB1);
			_byte(e, insn->operand);
		} else {
			_rm(e, 0x8A, 1, _regOffsets[op & 7]);
		}
		if (alu == 1 || alu == 3) {
			// bt dword [f], 4: carry in
			_byte(e, 0x0F);
			_rm(e, 0xBA, 4, REG_F);
			_byte(e, 4);
		}
		// add, adc, sub, sbb, and, xor, or, cmp al, cl
		static const uint8_t hostOps[8] = { 0x00, 0x10, 0x28, 0x18, 0x20, 0x30, 0x08, 0x38 };
		_byte(e, hostOps[alu]);
		_byte(e, 0xC8);
		if (alu < 4 || alu == 7) {
			_hostFlags(e, jit);
			if (alu >= 2) {
				// or cl, N
				_byte(e, 0x80);
				_byte(e, 0xC9);
				_byte(e, 0x40);
			}
		} else {
			_zeroFlag(e);
			if (alu == 4) {
				// or cl, H
				_byte(e, 0x80);
				_byte(e, 0xC9);
				_byte(e, 0x20);
			}
		}
		_mergeFlags(e, 0x0F);
		if (alu != 7) {
			_rm(e, 0x88, 0, REG_A);
		}
		_setByte(e, offsetof(struct SM83Core, bus), imm ? insn->operand : op);
		return true;
	}
	if (op == 0xCB && (insn->operand & 0xC0) == 0x40 && (insn->operand & 7) != 6) {
		// BIT b, r: carry kept
		_rm(e, 0xF6, 0, _regOffsets[insn->operand & 7]);
		_byte(e, 1 << ((insn->operand >> 3) & 7));
		_zeroFlag(e);
		_byte(e, 0x80);
		_byte(e, 0xC9);
		_byte(e, 0x20);
		_mergeFlags(e, 0x1F);
		_setByte(e, offsetof(struct SM83Core, bus), insn->operand);
		return true;
	}
	if ((op == 0xF0 || op == 0xE0) && insn->operand >= 0x80 && insn->operand != 0xFF) {
		// LDH to or from high RAM: [r12 + hram]
		int32_t hram = offsetof(struct GBMemory, hram) + (insn->operand & 0x7F);
		if (op == 0xF0) {
			// mov al, [r12 + hram]; mov [a], al
			_byte(e, 0x41);
			_byte(e, 0x8A);
			_byte(e, 0x84);
			_byte(e, 0x24);
			_word32(e, hram);
			_rm(e, 0x88, 0, REG_A);
		} else {
			// mov al, [a]; mov [r12 + hram], al
			_rm(e, 0x8A, 0, REG_A);
			_byte(e, 0x41);
			_byte(e, 0x88);
			_byte(e, 0x84);
			_byte(e, 0x24);
			_word32(e, hram);
		}
		_rm(e, 0x88, 0, offsetof(struct SM83Core, bus));
		_setHalf(e, offsetof(struct SM83Core, index), 0xFF00 | insn->operand);
		return true;
	}
	return false;
}

// JR and JP to the region the block runs in, so the active region stays as it is
static bool _inlineBranch(const struct Decoded* insn, uint16_t pc, uint16_t end, uint16_t* target, uint8_t* mask, bool* when) {
	uint8_t op = insn->opcode;
	uint16_t next = insn->pc + insn->length;
	if (op == 0x18 || op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
		*target = next + (int8_t) insn->operand;
	} else if (op == 0xC3 || op == 0xC2 || op == 0xCA || op == 0xD2 || op == 0xDA) {
		*target = insn->operand;
	} else {
		return false;
	}
	if ((*target ^ pc) & GB_SIZE_CART_BANK0 || *target >= end) {
		return false;
	}
	*mask = 0;
	*when = true;
	if (op != 0x18 && op != 0xC3) {
		*mask = (op & 0x10) ? 0x10 : 0x80;
		*when = (op & 0x08) != 0;
	}
	return true;
}

#define PROLOGUE_BYTES 21

// A direct branch to the same ROM region continues in the target's block when it's compiled, without returning
static void _chain(struct Emitter* e, struct GBJit* jit, size_t regionOffset, uint16_t pc, uint16_t end, uint16_t target, bool compare, uint8_t** noLink, int* nNoLink) {
	if ((target ^ pc) & GB_SIZE_CART_BANK0 || target >= end) {
		return;
	}
	struct GBJitEntry* entry = _entry(jit, regionOffset + (target & (GB_SIZE_CART_BANK0 - 1)));
	uint8_t* next = NULL;
	if (compare) {
		// cmp eax, target; jne next
		_byte(e, 0x3D);
		_word32(e, target);
		_byte(e, 0x75);
		next = e->p;
		_byte(e, 0);
	}
	// mov rcx, &entry; cmp word [rcx + pc], target; jne noLink
	_byte(e, 0x48);
	_byte(e, 0xB9);
	_word64(e, (uintptr_t) entry);
	_byte(e, 0x66);
	_byte(e, 0x81);
	_byte(e, 0x79);
	_byte(e, offsetof(struct GBJitEntry, pc));
	_byte(e, target);
	_byte(e, target >> 8);
	noLink[(*nNoLink)++] = _jcc32(e, 0x85);
	// mov rcx, [rcx + code]; test rcx, rcx; jz noLink
	_byte(e, 0x48);
	_byte(e, 0x8B);
	_byte(e, 0x49);
	_byte(e, offsetof(struct GBJitEntry, code));
	_byte(e, 0x48);
	_byte(e, 0x85);
	_byte(e, 0xC9);
	noLink[(*nNoLink)++] = _jcc32(e, 0x84);
	// add rcx, PROLOGUE_BYTES; jmp rcx
	_byte(e, 0x48);
	_byte(e, 0x83);
	_byte(e, 0xC1);
	_byte(e, PROLOGUE_BYTES);
	_byte(e, 0xFF);
	_byte(e, 0xE1);
	if (next) {
		*next = e->p - (next + 1);
	}
}

// Both ways out of an inlined JR or JP: the latches, pc and cycles, then on into the next block
static uint8_t* _emitBranch(struct Emitter* e, struct GBJit* jit, const struct Decoded* insn, size_t regionOffset, uint16_t pc, uint16_t end, unsigned prefix, uint16_t target, uint8_t mask, bool when) {
	bool jp = insn->opcode >= 0xC0;
	uint16_t next = insn->pc + insn->length;
	if (jp) {
		_setHalf(e, offsetof(struct SM83Core, index), insn->operand & 0xFF);
		_setByte(e, offsetof(struct SM83Core, bus), insn->operand >> 8);
	} else {
		_setByte(e, offsetof(struct SM83Core, bus), insn->operand);
	}
	uint8_t* notTaken = NULL;
	if (mask) {
		// test byte [f], mask; setnz/setz cl; mov [condition], cl; test cl, cl; jz notTaken
		_rm(e, 0xF6, 0, REG_F);
		_byte(e, mask);
		_byte(e, 0x0F);
		_byte(e, when ? 0x95 : 0x94);
		_byte(e, 0xC1);
		_rm(e, 0x88, 1, offsetof(struct SM83Core, condition));
		_byte(e, 0x84);
		_byte(e, 0xC9);
		notTaken = _jcc32(e, 0x84);
	} else {
		_setByte(e, offsetof(struct SM83Core, condition), 1);
	}
	uint8_t* exit = NULL;
	int path;
	for (path = 0; path < (mask ? 2 : 1); ++path) {
		uint16_t to = path ? next : target;
		if (path) {
			_patch(notTaken, e->p);
		}
		_storeHalf(e, offsetof(struct SM83Core, pc), to);
		// mov eax, M-cycles
		_byte(e, 0xB8);
		_word32(e, prefix + (path ? (jp ? 3 : 2) : (jp ? 4 : 3)));
		_addCycles(e);
		// sub r13d, edx
		_byte(e, 0x41);
		_byte(e, 0x29);
		_byte(e, 0xD5);
		uint8_t* noLink[2];
		int nNoLink = 0;
		_chain(e, jit, regionOffset, pc, end, to, false, noLink, &nNoLink);
		int k;
		for (k = 0; k < nNoLink; ++k) {
			_patch(noLink[k], e->p);
		}
		// mov eax, edx
		_byte(e, 0x89);
		_byte(e, 0xD0);
		if (!exit) {
			exit = e->p;
			_return(e);
		} else {
			_patch(_jmp32(e), exit);
		}
	}
	return exit;
}

// Blocks end at a branch, at an instruction that has run through the interpreter before, or where code can't be compiled
static void* _compile(struct GBJit* jit, const uint8_t* rom, size_t offset, uint16_t pc, uint16_t end, bool* full) {
	struct Decoded insns[BLOCK_MAX];
	int n = 0;
	uint16_t at = pc;
	while (n < BLOCK_MAX) {
		if (_bails(jit, offset + (at - pc)) || !_decode(&rom[at - pc], at, end, &insns[n])) {
			break;
		}
		at += insns[n].length;
		++n;
		if (insns[n - 1].info->kind & KIND_BRANCH) {
			break;
		}
	}
	if (!n) {
		return NULL;
	}
	if (jit->used + BLOCK_BYTES + n * INSN_BYTES > CODE_SIZE) {
		*full = true;
		return NULL;
	}

	struct Emitter e = { jit->code + jit->used };
	uint8_t* start = e.p;
	uint8_t* bails[BLOCK_MAX];
	uint8_t* stops[BLOCK_MAX];
	uint8_t* ends[BLOCK_MAX];
	unsigned prefix[BLOCK_MAX];
	unsigned total = 0;
	bool anyBail = false;
	uint8_t* exit = NULL;
	int i;

	// push rbx; push r12; push r13; mov r13d, esi; mov rbx, rdi; mov r12, &memory
	_byte(&e, 0x53);
	_byte(&e, 0x41);
	_byte(&e, 0x54);
	_byte(&e, 0x41);
	_byte(&e, 0x55);
	_byte(&e, 0x41);
	_byte(&e, 0x89);
	_byte(&e, 0xF5);
	_byte(&e, 0x48);
	_byte(&e, 0x89);
	_byte(&e, 0xFB);
	_byte(&e, 0x49);
	_byte(&e, 0xBC);
	_word64(&e, (uintptr_t) &jit->gb->memory);
	if (e.p - start != PROLOGUE_BYTES) {
		abort();
	}
	for (i = 0; i < n; ++i) {
		const struct Decoded* insn = &insns[i];
		prefix[i] = total;
		// cmp r13d, M-cycles through this one; jb stop: it would reach the next event
		_byte(&e, 0x41);
		_byte(&e, 0x81);
		_byte(&e, 0xFD);
		_word32(&e, total + insn->info->maxCycles);
		stops[i] = _jcc32(&e, 0x82);
		if (insn->info->kind & (KIND_LOAD | KIND_BRANCH)) {
			// Loads from ROM and jumps out of ROM replace these, so they go first
			_storeCartBus(&e, insn->lastByte, insn->pc + insn->length - 1);
		}
		bails[i] = NULL;
		ends[i] = NULL;
		uint16_t target;
		uint8_t mask;
		bool when;
		if ((insn->info->kind & KIND_BRANCH) && !jit->noInline && _inlineBranch(insn, pc, end, &target, &mask, &when)) {
			exit = _emitBranch(&e, jit, insn, offset - (pc & (GB_SIZE_CART_BANK0 - 1)), pc, end, total, target, mask, when);
			break;
		}
		if (_inline(&e, jit, insn)) {
			total += insn->info->cycles;
			continue;
		}
		// mov rdi, rbx; mov esi, operand; mov edx, next pc
		_byte(&e, 0x48);
		_byte(&e, 0x89);
		_byte(&e, 0xDF);
		_byte(&e, 0xBE);
		_word32(&e, insn->operand);
		_byte(&e, 0xBA);
		_word32(&e, (uint16_t) (insn->pc + insn->length));
		if (insn->info->kind & KIND_STORE) {
			// mov ecx, M-cycle of the store
			_byte(&e, 0xB9);
			_word32(&e, total + insn->info->cycles - 1);
		}
		_call(&e, insn->info->op);
		if (insn->info->kind & (KIND_LOAD | KIND_STORE)) {
			// test eax, eax; js bail
			_byte(&e, 0x85);
			_byte(&e, 0xC0);
			bails[i] = _jcc32(&e, 0x88);
			anyBail = true;
		}
		if ((insn->info->kind & KIND_STORE) && !(insn->info->kind & KIND_BRANCH) && i + 1 < n) {
			// jnz: the store changed the machine, so stop before the next instruction
			ends[i] = _jcc32(&e, 0x85);
		}
		total += insn->info->cycles;
	}
	if (!exit) {
		const struct Decoded* last = &insns[n - 1];
		if (last->info->kind & KIND_BRANCH) {
			// add eax, prefix
			_byte(&e, 0x05);
			_word32(&e, total);
		} else {
			if (!(last->info->kind & KIND_LOAD)) {
				_storeCartBus(&e, last->lastByte, last->pc + last->length - 1);
			}
			_storeHalf(&e, offsetof(struct SM83Core, pc), last->pc + last->length);
			// mov eax, total
			_byte(&e, 0xB8);
			_word32(&e, total);
		}
		_addCycles(&e);
		if (last->info->kind & KIND_BRANCH) {
			// sub r13d, edx; movzx eax, word [rbx + pc]
			_byte(&e, 0x41);
			_byte(&e, 0x29);
			_byte(&e, 0xD5);
			_byte(&e, 0x0F);
			_byte(&e, 0xB7);
			_byte(&e, 0x83);
			_word32(&e, offsetof(struct SM83Core, pc));
			uint8_t* noLink[4];
			int nNoLink = 0;
			size_t regionOffset = offset - (pc & (GB_SIZE_CART_BANK0 - 1));
			uint16_t next = last->pc + last->length;
			uint8_t op = last->opcode;
			if (op == 0xC3 || op == 0xC2 || op == 0xCA || op == 0xD2 || op == 0xDA || op == 0xCD || op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC) {
				_chain(&e, jit, regionOffset, pc, end, last->operand, true, noLink, &nNoLink);
			} else if (op == 0x18 || op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
				_chain(&e, jit, regionOffset, pc, end, next + (int8_t) last->operand, true, noLink, &nNoLink);
			} else if ((op & 0xC7) == 0xC7) {
				_chain(&e, jit, regionOffset, pc, end, op & 0x38, true, noLink, &nNoLink);
			}
			if (op != 0xC3 && op != 0x18 && op != 0xCD && op != 0xC9 && op != 0xE9 && (op & 0xC7) != 0xC7) {
				_chain(&e, jit, regionOffset, pc, end, next, true, noLink, &nNoLink);
			}
			for (i = 0; i < nNoLink; ++i) {
				_patch(noLink[i], e.p);
			}
		}
		// mov eax, edx
		_byte(&e, 0x89);
		_byte(&e, 0xD0);
		exit = e.p;
		_return(&e);
	}
	uint8_t* tail = e.p;
	_addCycles(&e);
	_patch(_jmp32(&e), exit);

	// Stopping before an instruction leaves the machine as the interpreter has it at that fetch
	for (i = 0; i < n; ++i) {
		_patch(stops[i], e.p);
		if (i > 0 && ends[i - 1]) {
			_patch(ends[i - 1], e.p);
		}
		if (i > 0) {
			_storeHalf(&e, offsetof(struct SM83Core, pc), insns[i].pc);
			if (!(insns[i - 1].info->kind & KIND_LOAD)) {
				_storeCartBus(&e, insns[i - 1].lastByte, insns[i - 1].pc + insns[i - 1].length - 1);
			}
		}
		_byte(&e, 0xB8);
		_word32(&e, prefix[i]);
		_patch(_jmp32(&e), tail);
	}

	if (anyBail) {
		// The interpreter runs a bailing instruction from its first M-cycle
		uint8_t* bailExit = e.p;
		_addCycles(&e);
		// mov eax, edx; or eax, BAILED; jmp exit
		_byte(&e, 0x89);
		_byte(&e, 0xD0);
		_byte(&e, 0x0D);
		_word32(&e, BAILED);
		_patch(_jmp32(&e), exit);
		for (i = 0; i < n; ++i) {
			if (!bails[i]) {
				continue;
			}
			_patch(bails[i], e.p);
			_storeHalf(&e, offsetof(struct SM83Core, pc), insns[i].pc);
			// mov rax, &entry; mov [&jit->bailed], rax
			_byte(&e, 0x48);
			_byte(&e, 0xB8);
			_word64(&e, (uintptr_t) _entry(jit, offset));
			_byte(&e, 0x48);
			_byte(&e, 0xA3);
			_word64(&e, (uintptr_t) &jit->bailed);
			if (i > 0 && !(insns[i - 1].info->kind & KIND_LOAD)) {
				_storeCartBus(&e, insns[i - 1].lastByte, insns[i - 1].pc + insns[i - 1].length - 1);
			}
			_byte(&e, 0xB8);
			_word32(&e, prefix[i]);
			_patch(_jmp32(&e), bailExit);
		}
	}
	jit->used += e.p - start;
	jit->used = (jit->used + 15) & ~15;
	return start;
}

void GBJitFlush(struct GBJit* jit) {
	size_t i;
	for (i = 0; i < jit->nPages; ++i) {
		free(jit->pages[i]);
	}
	free(jit->pages);
	jit->pages = NULL;
	jit->nPages = 0;
	jit->used = FLAG_TABLE_BYTES;
	jit->rom = NULL;
	jit->romSize = 0;
}

// Every inlined instruction against its handler, from random machine states
static void _selfTest(struct GBJit* jit, unsigned iterations) {
	struct SM83Core* cpu = jit->gb->cpu;
	struct GBMemory* memory = &jit->gb->memory;
	struct SM83Core saved = *cpu;
	uint8_t savedHram[GB_SIZE_HRAM + 1];
	memcpy(savedHram, memory->hram, sizeof(savedHram));
	uint8_t savedBus = memory->cartBus;
	uint16_t savedBusPc = memory->cartBusPc;
	static uint8_t rom[GB_SIZE_CART_BANK0];
	jit->nPages = GB_SIZE_CART_BANK0 >> 8;
	jit->pages = calloc(jit->nPages, sizeof(*jit->pages));
	uint32_t seed = 1;
	unsigned tested = 0, failures = 0;
	int op;
	for (op = 0; op < 0x200; ++op) {
		uint8_t scratch[1024];
		struct Emitter probe = { scratch };
		struct Decoded insn;
		rom[0x100] = op < 0x100 ? op : 0xCB;
		rom[0x101] = op < 0x100 ? 0x90 : op;
		rom[0x102] = 0x01;
		if (!_decode(&rom[0x100], 0x100, GB_SIZE_CART_BANK0, &insn)) {
			continue;
		}
		// HALT ends the block after the one instruction
		rom[0x100 + insn.length] = 0x76;
		uint16_t target;
		uint8_t mask;
		bool when;
		if (!_inline(&probe, jit, &insn) && !((insn.info->kind & KIND_BRANCH) && _inlineBranch(&insn, 0x100, GB_SIZE_CART_BANK0, &target, &mask, &when))) {
			continue;
		}
		unsigned k;
		for (k = 0; k < iterations; ++k) {
			struct SM83Core states[2];
			uint8_t hram[2][GB_SIZE_HRAM + 1];
			uint8_t bus[2];
			uint16_t busPc[2];
			uint8_t random[16];
			unsigned b;
			for (b = 0; b < sizeof(random); ++b) {
				seed = seed * 1103515245 + 12345;
				random[b] = seed >> 16;
			}
			if (op < 0x100 && insn.length > 1) {
				rom[0x101] = random[12];
			}
			if (insn.length > 2) {
				rom[0x102] = random[13] & 0x3F;
			}
			int which;
			for (which = 0; which < 2; ++which) {
				bool full = false;
				jit->used = FLAG_TABLE_BYTES;
				jit->noInline = !which;
				void* block = _compile(jit, &rom[0x100], 0x100, 0x100, GB_SIZE_CART_BANK0, &full);
				jit->noInline = false;
				*cpu = saved;
				memcpy(&cpu->regs, random, 8);
				cpu->sp = random[8] | (random[9] << 8);
				cpu->pc = 0x100;
				cpu->bus = random[10];
				cpu->index = random[11] | (random[14] << 8);
				cpu->condition = random[15] & 1;
				cpu->cycles = 0;
				cpu->nextEvent = 1000;
				memcpy(memory->hram, random, 16);
				memory->hram[0x7E] = random[3];
				((int (*)(struct SM83Core*, int)) block)(cpu, 100);
				states[which] = *cpu;
				memcpy(hram[which], memory->hram, sizeof(hram[which]));
				bus[which] = memory->cartBus;
				busPc[which] = memory->cartBusPc;
			}
			++tested;
			if (memcmp(&states[0].regs, &states[1].regs, sizeof(states[0].regs)) || states[0].index != states[1].index ||
			    states[0].bus != states[1].bus || states[0].condition != states[1].condition || states[0].cycles != states[1].cycles ||
			    memcmp(hram[0], hram[1], sizeof(hram[0])) || bus[0] != bus[1] || busPc[0] != busPc[1]) {
				if (failures++ < 8) {
					fprintf(stderr, "GB JIT self-test: %s%02X differs (af %04X/%04X bc %04X/%04X hl %04X/%04X pc %04X/%04X bus %02X/%02X)\n",
					        op < 0x100 ? "" : "CB ", op & 0xFF, states[0].af, states[1].af, states[0].bc, states[1].bc, states[0].hl, states[1].hl,
					        states[0].pc, states[1].pc, states[0].bus, states[1].bus);
				}
			}
		}
	}
	*cpu = saved;
	memcpy(memory->hram, savedHram, sizeof(savedHram));
	memory->cartBus = savedBus;
	memory->cartBusPc = savedBusPc;
	GBJitFlush(jit);
	fprintf(stderr, "GB JIT self-test: %u instructions, %u failures\n", tested, failures);
}

struct GBJit* GBJitCreate(struct GB* gb) {
	int i;
	for (i = 0; i < 0x100; ++i) {
		struct GBJitOpInfo* info = &_cbInfo[i];
		info->op = _cbOps[i];
		info->length = 2;
		if ((i & 7) != 6) {
			info->cycles = 2;
			info->kind = KIND_PLAIN;
		} else if ((i >> 6) == 1) {
			info->cycles = 3;
			info->kind = KIND_LOAD;
		} else {
			info->cycles = 4;
			info->kind = KIND_LOAD | KIND_STORE;
		}
		info->maxCycles = info->cycles;
	}
	struct GBJit* jit = calloc(1, sizeof(*jit));
	jit->gb = gb;
	// Close to the handlers, so blocks can call them directly
	uintptr_t near = ((uintptr_t) _ops & ~(uintptr_t) 0xFFFFF) - (256 << 20);
	jit->code = mmap((void*) near, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (jit->code == MAP_FAILED) {
		free(jit);
		return NULL;
	}
	// SM83 Z, H and C for each value of the host flags as LAHF leaves them
	for (i = 0; i < FLAG_TABLE_BYTES; ++i) {
		jit->code[i] = ((i & 0x40) << 1) | ((i & 0x10) << 1) | ((i & 0x01) << 4);
	}
	jit->used = FLAG_TABLE_BYTES;
	if (getenv("MGBA_JIT_SELFTEST")) {
		_selfTest(jit, atoi(getenv("MGBA_JIT_SELFTEST")));
	}
	return jit;
}

void GBJitDestroy(struct GBJit* jit) {
	GBJitFlush(jit);
	munmap(jit->code, CODE_SIZE);
	free(jit);
}

static bool _runBlock(struct SM83Core* cpu, struct GBJit* jit) {
	struct GBMemory* memory = &jit->gb->memory;
	if (cpu->irqPending || cpu->halted || memory->dmaRemaining || cpu->memory.cpuLoad8 != GBCartLoad8 ||
	    cpu->memory.load8 != GBLoad8 || cpu->memory.store8 != GBStore8 || cpu->memory.activeMask != GB_SIZE_CART_BANK0 - 1) {
		return false;
	}
	uint16_t pc = cpu->pc;
	uint16_t end = cpu->memory.activeRegionEnd;
	if (pc >= end || ((pc ^ (end - 1)) & GB_SIZE_CART_BANK0)) {
		return false;
	}
	const uint8_t* code = &cpu->memory.activeRegion[pc & (GB_SIZE_CART_BANK0 - 1)];
	if (code < memory->rom || code >= memory->rom + memory->romSize) {
		return false;
	}
	if ((size_t) (end - pc) > (size_t) (memory->rom + memory->romSize - code)) {
		end = pc + (memory->rom + memory->romSize - code);
	}
	if (jit->rom != memory->rom || jit->romSize != memory->romSize) {
		GBJitFlush(jit);
		jit->rom = memory->rom;
		jit->romSize = memory->romSize;
		jit->nPages = (memory->romSize + 0xFF) >> 8;
		jit->pages = calloc(jit->nPages, sizeof(*jit->pages));
	}
	size_t offset = code - memory->rom;
	struct GBJitEntry* entry = _entry(jit, offset);
	if (!entry->code || entry->pc != pc) {
		if (entry->failed) {
			return false;
		}
		if (entry->hits < HOT_THRESHOLD) {
			++entry->hits;
			return false;
		}
		bool full = false;
		void* block = _compile(jit, code, offset, pc, end, &full);
		if (full) {
			GBJitFlush(jit);
			return false;
		}
		if (!block) {
			entry->failed = true;
			return false;
		}
		entry->code = block;
		entry->pc = pc;
	}
	int t = cpu->tMultiplier;
	// Instructions run while all their M-cycles end before the next event, as the interpreter checks after each one
	int budget = (cpu->nextEvent - cpu->cycles + t - 1) >> (t + 1);
	if (budget <= 0) {
		return false;
	}
	int32_t cycles = cpu->cycles;
	int ran = ((int (*)(struct SM83Core*, int)) entry->code)(cpu, budget);
	if (ran & BAILED) {
		// Later blocks stop short of this instruction, and the block that reached it is rebuilt to
		size_t at = cpu->memory.activeRegion + (cpu->pc & (GB_SIZE_CART_BANK0 - 1)) - memory->rom;
		_entry(jit, at)->bails = true;
		if (jit->bailed->pc == cpu->pc) {
			jit->bailed->failed = true;
		}
		jit->bailed->code = NULL;
	}
	return cpu->cycles != cycles;
}

bool GBJitRun(struct SM83Core* cpu) {
	struct GBJit* jit = ((struct GB*) cpu->master)->jit;
	bool ran = false;
	while (_runBlock(cpu, jit)) {
		ran = true;
	}
	return ran;
}

#else

struct GBJit* GBJitCreate(struct GB* gb) {
	UNUSED(gb);
	return NULL;
}

void GBJitDestroy(struct GBJit* jit) {
	UNUSED(jit);
}

void GBJitFlush(struct GBJit* jit) {
	UNUSED(jit);
}

bool GBJitRun(struct SM83Core* cpu) {
	UNUSED(cpu);
	return false;
}

#endif
