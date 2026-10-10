/* Copyright (c) 2015-2022 Lior Halphon
 * Copyright (c) 2026 Magnominus
 *
 * Game Boy APU ported from SameBoy (Core/apu.c), which is under the MIT License:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */
#include <mgba/internal/gb/apu.h>

#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/io.h>
#include <mgba/internal/gb/serialize.h>
#include <mgba/internal/gb/timer.h>
#include <mgba/internal/sm83/sm83.h>

enum {
	CH_SQUARE_1,
	CH_SQUARE_2,
	CH_WAVE,
	CH_NOISE,
};

enum {
	SKIP_DIV_INACTIVE,
	SKIP_DIV_SKIPPED,
	SKIP_DIV_SKIP,
};

// CGBs are CGB-E unless a revision is set; DMG, MGB and SGB share the behavior of CGB-C and older
#define IS_CGB(GB) ((GB)->model >= GB_MODEL_CGB)
#define IS_AGB(GB) ((GB)->model >= GB_MODEL_AGB)
#define CGB_REVISION(GB) ((GB)->revision ? (GB)->revision : 'E')
#define IS_CGB_DE(GB) (IS_CGB(GB) && !IS_AGB(GB) && CGB_REVISION(GB) >= 'D')
#define IS_PRE_CGB_D(GB) (!IS_CGB(GB) || (!IS_AGB(GB) && CGB_REVISION(GB) <= 'C'))
#define IS_CGB_B_OR_OLDER(GB) (IS_CGB(GB) && !IS_AGB(GB) && CGB_REVISION(GB) <= 'B')

#define APU_STATE_MAGIC 0x31555041

static const uint8_t _duties[] = {
	0, 0, 0, 0, 0, 0, 0, 1,
	1, 0, 0, 0, 0, 0, 0, 1,
	1, 0, 0, 0, 0, 1, 1, 1,
	0, 1, 1, 1, 1, 1, 1, 0,
};

static void _write(struct GB* gb, unsigned reg, uint8_t value);

static bool _dacEnabled(struct GB* gb, unsigned index) {
	if (IS_AGB(gb)) {
		return true;
	}
	switch (index) {
	case CH_SQUARE_1:
		return gb->memory.io[GB_REG_NR12] & 0xF8;
	case CH_SQUARE_2:
		return gb->memory.io[GB_REG_NR22] & 0xF8;
	case CH_WAVE:
		return gb->apu.wave.enable;
	default:
		return gb->memory.io[GB_REG_NR42] & 0xF8;
	}
}

static void _updateSample(struct GB* gb, unsigned index, uint8_t value) {
	struct GBApu* apu = &gb->apu;
	if (IS_AGB(gb)) {
		apu->samples[index] = value;
		return;
	}
	if (value == 0 && apu->samples[index] == 0) {
		return;
	}
	if (_dacEnabled(gb, index)) {
		apu->samples[index] = value;
	}
}

static void _updateSquareSample(struct GB* gb, unsigned index) {
	struct GBApuSquare* ch = &gb->apu.square[index];
	if (ch->sampleSuppressed) {
		if (IS_AGB(gb)) {
			_updateSample(gb, index, gb->apu.samples[index]);
		}
		return;
	}
	uint8_t duty = gb->memory.io[index ? GB_REG_NR21 : GB_REG_NR11] >> 6;
	_updateSample(gb, index, _duties[ch->currentSampleIndex + duty * 8] ? ch->currentVolume : 0);
}

static void _updateWaveSample(struct GB* gb) {
	struct GBApuWave* ch = &gb->apu.wave;
	if (ch->currentSampleIndex & 1) {
		_updateSample(gb, CH_WAVE, (ch->currentSampleByte & 0xF) >> ch->shift);
	} else {
		_updateSample(gb, CH_WAVE, (ch->currentSampleByte >> 4) >> ch->shift);
	}
}

static void _updateNoiseSample(struct GB* gb) {
	_updateSample(gb, CH_NOISE, gb->apu.noise.currentLfsrSample ? gb->apu.noise.currentVolume : 0);
}

static void _nrx2GlitchStep(uint8_t* volume, uint8_t value, uint8_t oldValue, uint8_t* countdown, struct GBApuEnvelopeClock* lock) {
	if (lock->clock) {
		*countdown = value & 7;
	}
	bool shouldTick = (value & 7) && !(oldValue & 7) && !lock->locked;
	bool shouldInvert = (value & 8) ^ (oldValue & 8);

	if ((value & 0xF) == 8 && (oldValue & 0xF) == 8 && !lock->locked) {
		shouldTick = true;
	}

	if (shouldInvert) {
		if (value & 8) {
			if (!(oldValue & 7) && !lock->locked) {
				*volume ^= 0xF;
			} else {
				*volume = 0xE - *volume;
				*volume &= 0xF;
			}
			shouldTick = false;
		} else {
			*volume = 0x10 - *volume;
			*volume &= 0xF;
		}
	}
	if (shouldTick) {
		if (value & 8) {
			++*volume;
		} else {
			--*volume;
		}
		*volume &= 0xF;
	} else if (!(value & 7) && lock->clock) {
		if (!shouldInvert) {
			if (*volume == 0xF && (value & 8)) {
				lock->locked = true;
			} else if (*volume == 0 && !(value & 8)) {
				lock->locked = true;
			}
		} else if (*volume == 1 && !(value & 8)) {
			lock->locked = true;
		} else if (*volume == 0xE && (value & 8)) {
			lock->locked = true;
		}
		lock->clock = false;
	}
}

static void _nrx2Glitch(struct GB* gb, uint8_t* volume, uint8_t value, uint8_t oldValue, uint8_t* countdown, struct GBApuEnvelopeClock* lock) {
	if (IS_PRE_CGB_D(gb)) {
		_nrx2GlitchStep(volume, 0xFF, oldValue, countdown, lock);
		_nrx2GlitchStep(volume, value, 0xFF, countdown, lock);
	} else {
		_nrx2GlitchStep(volume, value, oldValue, countdown, lock);
	}
}

static void _tickEnvelope(uint8_t nrx2, uint8_t* volume, struct GBApuEnvelopeClock* clock) {
	if (nrx2 & 8) {
		if (*volume < 0xF) {
			++*volume;
		} else {
			clock->locked = true;
		}
	} else {
		if (*volume > 0) {
			--*volume;
		} else {
			clock->locked = true;
		}
	}
}

static void _tickSquareEnvelope(struct GB* gb, unsigned index) {
	struct GBApuSquare* ch = &gb->apu.square[index];
	uint8_t nrx2 = gb->memory.io[index ? GB_REG_NR22 : GB_REG_NR12];
	if (ch->envelopeClock.locked || !(nrx2 & 7)) {
		return;
	}
	_tickEnvelope(nrx2, &ch->currentVolume, &ch->envelopeClock);
	if (gb->apu.isActive[index]) {
		_updateSquareSample(gb, index);
	}
}

static void _tickNoiseEnvelope(struct GB* gb) {
	struct GBApuNoise* ch = &gb->apu.noise;
	uint8_t nr42 = gb->memory.io[GB_REG_NR42];
	if (ch->envelopeClock.locked || !(nr42 & 7)) {
		return;
	}
	_tickEnvelope(nr42, &ch->currentVolume, &ch->envelopeClock);
	if (gb->apu.isActive[CH_NOISE]) {
		_updateNoiseSample(gb);
	}
}

static void _triggerSweepCalculation(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	uint8_t nr10 = gb->memory.io[GB_REG_NR10];
	if ((nr10 & 0x70) && apu->squareSweepCountdown == 7) {
		if (nr10 & 0x07) {
			apu->square[CH_SQUARE_1].sampleLength = (apu->sweepLengthAddend + apu->shadowSweepSampleLength + !!(nr10 & 0x8)) & 0x7FF;
		}
		if (apu->channel1RestartHold == 0) {
			apu->sweepLengthAddend = apu->square[CH_SQUARE_1].sampleLength >> (nr10 & 7);
		}
		apu->squareSweepCalculateCountdown = (nr10 & 0x7) * 2 + 5 - apu->lfDiv;
		apu->enableZombieCalculateStepping = false;
		apu->unshiftedSweep = !(nr10 & 0x7);
		apu->squareSweepCountdown = ((nr10 >> 4) & 7) ^ 7;
	}
}

static void _stepLfsr(struct GB* gb) {
	struct GBApuNoise* ch = &gb->apu.noise;
	unsigned highBitMask = ch->narrow ? 0x4040 : 0x4000;
	bool newHighBit = (ch->lfsr ^ (ch->lfsr >> 1) ^ 1) & 1;
	ch->lfsr >>= 1;
	if (newHighBit) {
		ch->lfsr |= highBitMask;
	} else {
		ch->lfsr &= ~highBitMask;
	}
	ch->currentLfsrSample = ch->lfsr & 1;
	if (gb->apu.isActive[CH_NOISE]) {
		_updateNoiseSample(gb);
	}
}

static void _run(struct GB* gb, unsigned cycles) {
	struct GBApu* apu = &gb->apu;
	uint8_t* io = gb->memory.io;
	uint8_t* wave = gb->audio.ch3.wavedata8;

	if (apu->wave.buggedReadCountdown) {
		unsigned cyclesLeft = cycles;
		while (cyclesLeft) {
			--cyclesLeft;
			if (--apu->wave.buggedReadCountdown == 0) {
				apu->wave.currentSampleByte = wave[gb->cpu->pc & 0xF];
				if (apu->isActive[CH_WAVE]) {
					_updateWaveSample(gb);
				}
				break;
			}
		}
	}

	bool startCh4 = false;
	if (apu->noise.dmgDelayedStart) {
		if (apu->noise.dmgDelayedStart == cycles) {
			apu->noise.dmgDelayedStart = 0;
			startCh4 = true;
		} else if (apu->noise.dmgDelayedStart > cycles) {
			apu->noise.dmgDelayedStart -= cycles;
		} else {
			unsigned first = apu->noise.dmgDelayedStart;
			cycles -= first;
			_run(gb, first);
		}
	}
	apu->lfDiv ^= cycles & 1;
	apu->noise.alignment += cycles;

	if (apu->squareSweepCalculateCountdown && ((io[GB_REG_NR10] & 7) || apu->unshiftedSweep || apu->squareSweepCalculateCountdown <= 3)) {
		if (apu->squareSweepCalculateCountdown > cycles) {
			apu->squareSweepCalculateCountdown -= cycles;
		} else {
			// The overflow check sees the sweep delta added twice
			if (apu->channel1RestartHold == 0) {
				apu->shadowSweepSampleLength = apu->square[CH_SQUARE_1].sampleLength;
			}
			if (io[GB_REG_NR10] & 8) {
				apu->sweepLengthAddend ^= 0x7FF;
			}
			if (apu->shadowSweepSampleLength + apu->sweepLengthAddend > 0x7FF && !(io[GB_REG_NR10] & 8)) {
				apu->isActive[CH_SQUARE_1] = false;
				_updateSample(gb, CH_SQUARE_1, 0);
			}
			apu->channel1CompletedAddend = apu->sweepLengthAddend;
			apu->squareSweepCalculateCountdown = 0;
		}
	}

	if (apu->channel1RestartHold) {
		if (apu->channel1RestartHold > cycles) {
			apu->channel1RestartHold -= cycles;
		} else {
			apu->channel1RestartHold = 0;
		}
	}

	unsigned i;
	for (i = CH_SQUARE_1; i <= CH_SQUARE_2; ++i) {
		struct GBApuSquare* ch = &apu->square[i];
		if (!apu->isActive[i]) {
			continue;
		}
		unsigned cyclesLeft = cycles;
		if (ch->delay) {
			if (ch->delay < cyclesLeft) {
				ch->delay = 0;
			} else {
				ch->delay -= cyclesLeft;
			}
		}
		while (cyclesLeft > ch->sampleCountdown) {
			cyclesLeft -= ch->sampleCountdown + 1;
			ch->sampleCountdown = (ch->sampleLength ^ 0x7FF) * 2 + 1;
			ch->currentSampleIndex = (ch->currentSampleIndex + 1) & 7;
			ch->sampleSuppressed = false;
			ch->didTick = true;
			_updateSquareSample(gb, i);
		}
		ch->sampleCountdown -= cyclesLeft;
	}

	apu->wave.waveFormJustRead = false;
	if (apu->isActive[CH_WAVE]) {
		struct GBApuWave* ch = &apu->wave;
		unsigned cyclesLeft = cycles;
		while (cyclesLeft > ch->sampleCountdown) {
			cyclesLeft -= ch->sampleCountdown + 1;
			ch->sampleCountdown = ch->sampleLength ^ 0x7FF;
			ch->currentSampleIndex = (ch->currentSampleIndex + 1) & 0x1F;
			ch->currentSampleByte = wave[ch->currentSampleIndex >> 1];
			_updateWaveSample(gb);
			ch->waveFormJustRead = true;
		}
		if (cyclesLeft) {
			ch->sampleCountdown -= cyclesLeft;
			ch->waveFormJustRead = false;
		}
	} else if (apu->wave.enable && apu->wave.pulsed && !IS_AGB(gb)) {
		struct GBApuWave* ch = &apu->wave;
		unsigned cyclesLeft = cycles;
		while (cyclesLeft > ch->sampleCountdown) {
			cyclesLeft -= ch->sampleCountdown + 1;
			ch->sampleCountdown = ch->sampleLength ^ 0x7FF;
			if (cyclesLeft) {
				ch->currentSampleByte = wave[gb->cpu->pc & 0xF];
			} else {
				ch->buggedReadCountdown = 1;
			}
		}
		ch->sampleCountdown -= cyclesLeft;
		if (ch->sampleCountdown == 0) {
			ch->buggedReadCountdown = 2;
		}
	}

	// The noise channel can step even if inactive on the DMG
	if (apu->isActive[CH_NOISE] || !IS_CGB(gb)) {
		struct GBApuNoise* ch = &apu->noise;
		unsigned cyclesLeft = cycles;
		unsigned divisor = (io[GB_REG_NR43] & 0x07) << 2;
		if (!divisor) {
			divisor = 2;
		}
		if (ch->counterCountdown == 0) {
			ch->counterCountdown = divisor;
		}
		while (cyclesLeft >= ch->counterCountdown) {
			cyclesLeft -= ch->counterCountdown;
			ch->counterCountdown = divisor + ch->delta;
			ch->delta = 0;
			bool oldBit = (ch->counter >> (io[GB_REG_NR43] >> 4)) & 1;
			ch->counter = (ch->counter + 1) & 0x3FFF;
			bool newBit = (ch->counter >> (io[GB_REG_NR43] >> 4)) & 1;
			if (newBit && !oldBit) {
				_stepLfsr(gb);
			}
		}
		if (cyclesLeft) {
			ch->counterCountdown -= cyclesLeft;
			ch->countdownReloaded = false;
		} else {
			ch->countdownReloaded = true;
		}
	}

	if (startCh4) {
		_write(gb, GB_REG_NR44, io[GB_REG_NR44] | 0x80);
	}
}

// Channels advance two 2 MHz ticks (one in double speed) with each step of the system counter
static void _sync(struct GB* gb, int32_t when) {
	struct GBApu* apu = &gb->apu;
	int32_t stepLength = 4 << !gb->doubleSpeed;
	int32_t elapsed = when - apu->lastStep;
	if (elapsed < stepLength) {
		return;
	}
	if (!apu->globalEnable) {
		apu->lastStep = when;
		return;
	}
	int32_t steps = elapsed / stepLength;
	apu->lastStep += steps * stepLength;
	unsigned stepTicks = stepLength / 4;
	// Pending sweep calculations and glitched wave reads depend on running every M-cycle
	while (steps && (apu->squareSweepCalculateCountdown || apu->channel1RestartHold || apu->wave.buggedReadCountdown ||
	                 (apu->wave.enable && apu->wave.pulsed && !apu->isActive[CH_WAVE] && !IS_AGB(gb)))) {
		_run(gb, stepTicks);
		--steps;
	}
	unsigned ticks = steps * stepTicks;
	while (ticks > 0x800) {
		_run(gb, 0x800);
		ticks -= 0x800;
	}
	if (ticks) {
		_run(gb, ticks);
	}
}

static void _init(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	memset(apu, 0, offsetof(struct GBApu, lastStep));
	apu->lfDiv = 1;
	apu->wave.shift = 4;
	// Powering on while the counter's APU bit is set skips the first DIV event
	if ((GBTimerCounter(&gb->timer) & ~3) & (gb->doubleSpeed ? 0x2000 : 0x1000)) {
		apu->skipDivEvent = SKIP_DIV_SKIP;
		apu->divDivider = 1;
	}
	apu->square[CH_SQUARE_1].sampleCountdown = 0xFFFF;
	apu->square[CH_SQUARE_2].sampleCountdown = 0xFFFF;
}

static void _lengthGlitch(struct GB* gb, uint8_t value, bool lengthEnabled, uint16_t* pulseLength, unsigned index, uint16_t reload) {
	// CGB-B and older tick it whatever the new value, except on channel 4
	if (((value & 0x40) || (index != CH_NOISE && IS_CGB_B_OR_OLDER(gb))) && !lengthEnabled && (gb->apu.divDivider & 1) && *pulseLength) {
		--*pulseLength;
		if (*pulseLength == 0) {
			if (value & 0x80) {
				*pulseLength = reload;
			} else {
				gb->apu.isActive[index] = false;
				_updateSample(gb, index, 0);
			}
		}
	}
}

static void _writeSquareControl(struct GB* gb, unsigned index, unsigned reg, uint8_t value) {
	struct GBApu* apu = &gb->apu;
	struct GBApuSquare* ch = &apu->square[index];
	uint8_t* io = gb->memory.io;
	uint8_t nrx2 = io[index ? GB_REG_NR22 : GB_REG_NR12];
	bool wasActive = apu->isActive[index];

	// Lowering the length below $700 right before a reload steps back the sample index
	if ((value & 0x80) == 0 && apu->isActive[index] && (io[reg] & 0x7) == 7 && (value & 7) != 7) {
		if (IS_CGB_DE(gb) || (ch->sampleCountdown & 1)) {
			if (ch->didTick && ch->sampleCountdown >> 1 == (ch->sampleLength ^ 0x7FF)) {
				ch->currentSampleIndex = (ch->currentSampleIndex - 1) & 7;
				ch->sampleSuppressed = false;
			}
		}
	}

	uint16_t oldSampleLength = ch->sampleLength;
	ch->sampleLength = (ch->sampleLength & 0xFF) | ((value & 7) << 8);
	if (value & 0x80) {
		ch->envelopeClock.locked = false;
		ch->envelopeClock.clock = false;
		ch->didTick = false;
		bool forceUnsuppressed = false;
		if (!apu->isActive[index]) {
			if (IS_CGB_DE(gb)) {
				if (!(value & 4) && !(((ch->sampleCountdown - ch->delay) / 2) & 0x400)) {
					ch->currentSampleIndex = (ch->currentSampleIndex + 1) & 7;
					forceUnsuppressed = true;
				}
			}
			ch->delay = 6 - apu->lfDiv;
			ch->sampleCountdown = (ch->sampleLength ^ 0x7FF) * 2 + ch->delay;
			if (IS_PRE_CGB_D(gb) && apu->lfDiv) {
				ch->sampleCountdown += 2;
				ch->delay += 2;
			}
		} else {
			unsigned extraDelay = 0;
			if (IS_CGB_DE(gb)) {
				if (!(value & 4) && !(((ch->sampleCountdown - 1 - ch->delay) / 2) & 0x400)) {
					ch->currentSampleIndex = (ch->currentSampleIndex + 1) & 7;
					ch->sampleSuppressed = false;
				} else if (ch->sampleLength == 0x7FF && oldSampleLength != 0x7FF && ch->sampleSuppressed) {
					extraDelay += 2;
				}
			}
			// Retriggering an active channel starts it two ticks sooner
			ch->delay = 4 - apu->lfDiv + extraDelay;
			ch->sampleCountdown = (ch->sampleLength ^ 0x7FF) * 2 + ch->delay;
			if (IS_PRE_CGB_D(gb) && apu->lfDiv) {
				ch->sampleCountdown += 2;
				ch->delay += 2;
			}
		}
		ch->currentVolume = nrx2 >> 4;
		if (apu->isActive[index]) {
			_updateSquareSample(gb, index);
		}
		ch->volumeCountdown = nrx2 & 7;

		if ((nrx2 & 0xF8) != 0 && !apu->isActive[index]) {
			apu->isActive[index] = true;
			_updateSample(gb, index, 0);
			ch->sampleSuppressed = !forceUnsuppressed;
		}
		if (ch->pulseLength == 0) {
			ch->pulseLength = 0x40;
			ch->lengthEnabled = false;
		}

		if (index == CH_SQUARE_1) {
			uint8_t nr10 = io[GB_REG_NR10];
			apu->shadowSweepSampleLength = 0;
			apu->channel1CompletedAddend = 0;
			if (nr10 & 7) {
				// A nonzero shift also checks for overflow on trigger
				apu->squareSweepCalculateCountdown = (nr10 & 0x7) * 2 + 5 - apu->lfDiv;
				apu->enableZombieCalculateStepping = false;
				apu->unshiftedSweep = false;
				if (!wasActive) {
					apu->squareSweepCalculateCountdown += 2;
				}
				apu->sweepLengthAddend = ch->sampleLength >> (nr10 & 7);
			} else {
				apu->sweepLengthAddend = 0;
			}
			apu->channel1RestartHold = 2 - apu->lfDiv + (IS_CGB(gb) && CGB_REVISION(gb) != 'D') * 2;
			apu->squareSweepCountdown = ((nr10 >> 4) & 7) ^ 7;
		}
	}

	// Enabling the length while the DIV divider's low bit is set ticks it once
	_lengthGlitch(gb, value, ch->lengthEnabled, &ch->pulseLength, index, 0x3F);
	ch->lengthEnabled = value & 0x40;
}

static void _writeNoiseControl(struct GB* gb, uint8_t value) {
	struct GBApu* apu = &gb->apu;
	struct GBApuNoise* ch = &apu->noise;
	uint8_t* io = gb->memory.io;
	if (value & 0x80) {
		ch->envelopeClock.locked = false;
		ch->envelopeClock.clock = false;
		if (!IS_CGB(gb) && (ch->alignment & 3) != 0) {
			ch->dmgDelayedStart = 6;
		} else {
			unsigned divisor = (io[GB_REG_NR43] & 0x07) << 2;
			if (!divisor) {
				divisor = 2;
			}
			ch->delta = 0;
			ch->counterCountdown = divisor + 4;
			if (divisor == 2) {
				if (IS_PRE_CGB_D(gb)) {
					ch->counterCountdown += apu->lfDiv;
					if (!gb->doubleSpeed) {
						ch->counterCountdown -= 1;
					}
				} else {
					ch->counterCountdown += 1 - apu->lfDiv;
				}
			} else {
				if (IS_PRE_CGB_D(gb)) {
					ch->counterCountdown += (const uint8_t[]) { 2, 1, 4, 3 }[ch->alignment & 3];
				} else {
					ch->counterCountdown += (const uint8_t[]) { 2, 1, 0, 3 }[ch->alignment & 3];
				}
				if (((ch->alignment + 1) & 3) < 2) {
					if ((io[GB_REG_NR43] & 0x07) == 1) {
						ch->counterCountdown -= 2;
						ch->delta = 2;
					} else {
						ch->counterCountdown -= 4;
					}
				}
			}

			if (IS_PRE_CGB_D(gb)) {
				if (gb->doubleSpeed) {
					if (!(io[GB_REG_NR43] & 0xF0) && (io[GB_REG_NR43] & 0x07)) {
						ch->counterCountdown -= 1;
					} else if ((io[GB_REG_NR43] & 0xF0) && !(io[GB_REG_NR43] & 0x07)) {
						ch->counterCountdown += 1;
					}
				} else {
					ch->counterCountdown -= 2;
				}
			}

			ch->currentVolume = io[GB_REG_NR42] >> 4;
			if (apu->isActive[CH_NOISE]) {
				_updateNoiseSample(gb);
			}
			ch->lfsr = 0;
			ch->currentLfsrSample = false;
			ch->volumeCountdown = io[GB_REG_NR42] & 7;

			if (!apu->isActive[CH_NOISE] && (io[GB_REG_NR42] & 0xF8) != 0) {
				apu->isActive[CH_NOISE] = true;
				_updateSample(gb, CH_NOISE, 0);
			}
			if (ch->pulseLength == 0) {
				ch->pulseLength = 0x40;
				ch->lengthEnabled = false;
			}
		}
	}
	_lengthGlitch(gb, value, ch->lengthEnabled, &ch->pulseLength, CH_NOISE, 0x3F);
	ch->lengthEnabled = value & 0x40;
}

static uint16_t _effectiveNoiseCounter(struct GB* gb) {
	uint16_t counter = gb->apu.noise.counter;
	if (IS_AGB(gb)) {
		return counter;
	}
	if (IS_CGB(gb) && CGB_REVISION(gb) == 'E') {
		if (counter & ((gb->memory.io[GB_REG_NR43] & 8) ? 0x40 : 0x80)) {
			counter |= 0xFF;
		}
		if (counter & 0x1000) {
			counter |= 0x10;
		}
		return counter;
	}
	if (IS_CGB(gb) && CGB_REVISION(gb) == 'D') {
		if (counter & ((gb->memory.io[GB_REG_NR43] & 8) ? 0x40 : 0x80)) {
			counter |= 0xFF;
		}
		if (counter & 0x100) {
			counter |= 0x1;
		}
		if (counter & 0x200) {
			counter |= 0x2;
		}
		if (counter & 0x400) {
			counter |= 0x4;
		}
		if (counter & 0x800) {
			counter |= 0x8;
		}
		if (counter & 0x1000) {
			counter |= 0x10;
		}
		return counter;
	}
	if (counter & 8) {
		counter |= 0xE;
	}
	if (counter & 0x80) {
		counter |= 0xFF;
	}
	if (counter & 0x100) {
		counter |= 0x1;
	}
	if (counter & 0x200) {
		counter |= 0x2;
	}
	if (counter & 0x400) {
		counter |= 0x4;
	}
	if (counter & 0x800) {
		if ((gb->memory.io[GB_REG_NR43] & 8) || (IS_CGB(gb) && CGB_REVISION(gb) == 'B')) {
			counter |= 0x400;
		}
		counter |= 0x8;
	}
	if (counter & 0x1000) {
		counter |= 0x10;
	}
	if (counter & 0x2000) {
		counter |= 0x20;
	}
	return counter;
}

static void _write(struct GB* gb, unsigned reg, uint8_t value) {
	struct GBApu* apu = &gb->apu;
	uint8_t* io = gb->memory.io;
	uint8_t* wave = gb->audio.ch3.wavedata8;

	if (!apu->globalEnable && reg != GB_REG_NR52 && reg < GB_REG_WAVE_0 &&
	    (IS_CGB(gb) || (reg != GB_REG_NR11 && reg != GB_REG_NR21 && reg != GB_REG_NR31 && reg != GB_REG_NR41))) {
		return;
	}

	if (reg >= GB_REG_WAVE_0) {
		if (apu->isActive[CH_WAVE]) {
			if ((!IS_CGB(gb) && !apu->wave.waveFormJustRead) || IS_AGB(gb)) {
				return;
			}
			reg = GB_REG_WAVE_0 + apu->wave.currentSampleIndex / 2;
		}
		wave[reg - GB_REG_WAVE_0] = value;
		return;
	}

	switch (reg) {
	case GB_REG_NR50:
	case GB_REG_NR51:
		break;

	case GB_REG_NR52: {
		uint16_t oldLengths[] = {
			apu->square[0].pulseLength,
			apu->square[1].pulseLength,
			apu->wave.pulseLength,
			apu->noise.pulseLength,
		};
		if ((value & 0x80) && !apu->globalEnable) {
			_init(gb);
			apu->globalEnable = true;
		} else if (!(value & 0x80) && apu->globalEnable) {
			memset(apu, 0, offsetof(struct GBApu, lastStep));
			memset(&io[GB_REG_NR10], 0, GB_REG_WAVE_0 - GB_REG_NR10);
		}
		if (!IS_CGB(gb) && (value & 0x80)) {
			apu->square[0].pulseLength = oldLengths[0];
			apu->square[1].pulseLength = oldLengths[1];
			apu->wave.pulseLength = oldLengths[2];
			apu->noise.pulseLength = oldLengths[3];
		}
		break;
	}

	case GB_REG_NR10: {
		bool oldNegate = io[GB_REG_NR10] & 8;
		io[GB_REG_NR10] = value;
		if (apu->shadowSweepSampleLength + apu->channel1CompletedAddend + oldNegate > 0x7FF && !(value & 8)) {
			apu->isActive[CH_SQUARE_1] = false;
			_updateSample(gb, CH_SQUARE_1, 0);
		}
		_triggerSweepCalculation(gb);
		break;
	}

	case GB_REG_NR11:
	case GB_REG_NR21: {
		unsigned index = reg == GB_REG_NR21;
		apu->square[index].pulseLength = 0x40 - (value & 0x3F);
		if (!apu->globalEnable) {
			value &= 0x3F;
		}
		break;
	}

	case GB_REG_NR12:
	case GB_REG_NR22: {
		unsigned index = reg == GB_REG_NR22;
		struct GBApuSquare* ch = &apu->square[index];
		if ((value & 0xF8) == 0) {
			// This disables the DAC
			io[reg] = value;
			apu->isActive[index] = false;
			_updateSample(gb, index, 0);
		} else if (apu->isActive[index]) {
			_nrx2Glitch(gb, &ch->currentVolume, value, io[reg], &ch->volumeCountdown, &ch->envelopeClock);
			_updateSquareSample(gb, index);
		}
		break;
	}

	case GB_REG_NR13:
	case GB_REG_NR23: {
		struct GBApuSquare* ch = &apu->square[reg == GB_REG_NR23];
		ch->sampleLength = (ch->sampleLength & ~0xFF) | value;
		break;
	}

	case GB_REG_NR14:
	case GB_REG_NR24:
		_writeSquareControl(gb, reg == GB_REG_NR24, reg, value);
		break;

	case GB_REG_NR30:
		apu->wave.enable = value & 0x80;
		if (!apu->wave.enable) {
			apu->wave.pulsed = false;
			if (apu->isActive[CH_WAVE]) {
				if (apu->wave.sampleCountdown == 0 && !IS_AGB(gb)) {
					apu->wave.currentSampleByte = wave[gb->cpu->pc & 0xF];
				} else if (apu->wave.waveFormJustRead && IS_PRE_CGB_D(gb)) {
					apu->wave.currentSampleByte = wave[GB_REG_NR30 & 0xF];
				}
			}
			apu->isActive[CH_WAVE] = false;
			_updateSample(gb, CH_WAVE, 0);
		}
		break;

	case GB_REG_NR31:
		apu->wave.pulseLength = 0x100 - value;
		break;

	case GB_REG_NR32:
		apu->wave.shift = (const uint8_t[]) { 4, 0, 1, 2 }[(value >> 5) & 3];
		if (apu->isActive[CH_WAVE]) {
			_updateWaveSample(gb);
		}
		break;

	case GB_REG_NR33:
		apu->wave.sampleLength = (apu->wave.sampleLength & ~0xFF) | value;
		break;

	case GB_REG_NR34: {
		struct GBApuWave* ch = &apu->wave;
		ch->sampleLength = (ch->sampleLength & 0xFF) | ((value & 7) << 8);
		if (value & 0x80) {
			ch->pulsed = true;
			// Retriggering on the DMG right before a wave read corrupts wave RAM
			if (!IS_CGB(gb) && apu->isActive[CH_WAVE] && ch->sampleCountdown == 0) {
				unsigned offset = ((ch->currentSampleIndex + 1) >> 1) & 0xF;
				if (offset < 4 && gb->model != GB_MODEL_MGB) {
					wave[0] = wave[offset];
				} else {
					memmove(wave, &wave[offset & ~3], 4);
				}
			}
			ch->currentSampleIndex = 0;
			if (apu->isActive[CH_WAVE] && ch->sampleCountdown == 0) {
				ch->currentSampleByte = wave[0];
			}
			if (ch->enable) {
				apu->isActive[CH_WAVE] = true;
				_updateSample(gb, CH_WAVE, (ch->currentSampleByte >> 4) >> ch->shift);
			}
			ch->sampleCountdown = (ch->sampleLength ^ 0x7FF) + 3;
			if (ch->pulseLength == 0) {
				ch->pulseLength = 0x100;
				ch->lengthEnabled = false;
			}
		}
		_lengthGlitch(gb, value, ch->lengthEnabled, &ch->pulseLength, CH_WAVE, 0xFF);
		ch->lengthEnabled = value & 0x40;
		break;
	}

	case GB_REG_NR41:
		apu->noise.pulseLength = 0x40 - (value & 0x3F);
		break;

	case GB_REG_NR42: {
		struct GBApuNoise* ch = &apu->noise;
		if ((value & 0xF8) == 0) {
			io[reg] = value;
			apu->isActive[CH_NOISE] = false;
			_updateSample(gb, CH_NOISE, 0);
		} else if (apu->isActive[CH_NOISE]) {
			_nrx2Glitch(gb, &ch->currentVolume, value, io[reg], &ch->volumeCountdown, &ch->envelopeClock);
			_updateNoiseSample(gb);
		}
		break;
	}

	case GB_REG_NR43: {
		struct GBApuNoise* ch = &apu->noise;
		ch->narrow = value & 8;
		uint16_t effectiveCounter = _effectiveNoiseCounter(gb);
		bool oldBit = (effectiveCounter >> (io[GB_REG_NR43] >> 4)) & 1;
		io[GB_REG_NR43] = value;
		bool newBit = (effectiveCounter >> (value >> 4)) & 1;
		if (ch->countdownReloaded) {
			unsigned divisor = (value & 0x07) << 2;
			if (!divisor) {
				divisor = 2;
			}
			if (!IS_PRE_CGB_D(gb)) {
				ch->counterCountdown = divisor + (divisor == 2 ? 0 : (const uint8_t[]) { 2, 1, 0, 3 }[ch->alignment & 3]);
			} else {
				ch->counterCountdown = divisor + (divisor == 2 ? 0 : (const uint8_t[]) { 2, 1, 4, 3 }[ch->alignment & 3]);
			}
			ch->delta = 0;
		}
		if (newBit && (!oldBit || IS_PRE_CGB_D(gb))) {
			if (IS_PRE_CGB_D(gb)) {
				bool previousNarrow = ch->narrow;
				ch->narrow = true;
				_stepLfsr(gb);
				ch->narrow = previousNarrow;
			} else {
				_stepLfsr(gb);
			}
		}
		break;
	}

	case GB_REG_NR44:
		_writeNoiseControl(gb, value);
		break;
	}
	io[reg] = value;
}

void GBApuReset(struct GB* gb) {
	memset(&gb->apu, 0, sizeof(gb->apu));
	gb->apu.lastStep = mTimingCurrentTime(&gb->timing);
}

void GBApuSkipBIOS(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	// The boot ROM's chime has faded out on channel 1, which is left running
	if (!(gb->model & GB_MODEL_SGB)) {
		apu->isActive[CH_SQUARE_1] = true;
		apu->square[CH_SQUARE_1].envelopeClock.locked = true;
		apu->square[CH_SQUARE_1].sampleCountdown = (apu->square[CH_SQUARE_1].sampleLength ^ 0x7FF) * 2;
	}
}

void GBApuWrite(struct GB* gb, unsigned reg, uint8_t value) {
	struct GBApu* apu = &gb->apu;
	int32_t now = mTimingCurrentTime(&gb->timing);
	_sync(gb, now);
	if (reg == GB_REG_NR52 && (value & 0x80) && !apu->globalEnable) {
		// Line the channels up with the steps of the system counter
		apu->lastStep = now - (GBTimerCounter(&gb->timer) & 3) * (2 - gb->doubleSpeed);
	}
	if (reg == GB_REG_NR10 && IS_PRE_CGB_D(gb) && apu->globalEnable) {
		// Pre-CGB-D sweep calculations keep stepping on NR10 writes
		if (apu->squareSweepCalculateCountdown > 3 && apu->enableZombieCalculateStepping) {
			apu->squareSweepCalculateCountdown -= 2;
		}
		apu->enableZombieCalculateStepping = true;
	}
	_write(gb, reg, value);
}

uint8_t GBApuRead(struct GB* gb, unsigned reg) {
	struct GBApu* apu = &gb->apu;
	_sync(gb, mTimingCurrentTime(&gb->timing));
	switch (reg) {
	case GB_REG_NR52: {
			uint8_t value = 0x70;
		unsigned i;
		for (i = 0; i < 4; ++i) {
			value |= apu->isActive[i] << i;
		}
		return value | (apu->globalEnable << 7);
	}
	case GB_REG_PCM12:
		return (apu->isActive[CH_SQUARE_2] ? apu->samples[CH_SQUARE_2] << 4 : 0) | (apu->isActive[CH_SQUARE_1] ? apu->samples[CH_SQUARE_1] : 0);
	case GB_REG_PCM34:
		return (apu->isActive[CH_NOISE] ? apu->samples[CH_NOISE] << 4 : 0) | (apu->isActive[CH_WAVE] ? apu->samples[CH_WAVE] : 0);
	default:
		if (apu->isActive[CH_WAVE]) {
			if ((!IS_CGB(gb) && !apu->wave.waveFormJustRead) || IS_AGB(gb)) {
				return 0xFF;
			}
			reg = GB_REG_WAVE_0 + apu->wave.currentSampleIndex / 2;
		}
		return gb->audio.ch3.wavedata8[reg - GB_REG_WAVE_0];
	}
}

void GBApuDivEvent(struct GB* gb, int32_t when) {
	struct GBApu* apu = &gb->apu;
	_sync(gb, when);
	if (!apu->globalEnable) {
		return;
	}
	if (apu->skipDivEvent == SKIP_DIV_SKIP) {
		apu->skipDivEvent = SKIP_DIV_SKIPPED;
		return;
	}
	if (apu->skipDivEvent == SKIP_DIV_SKIPPED) {
		apu->skipDivEvent = SKIP_DIV_INACTIVE;
	} else {
		++apu->divDivider;
	}

	unsigned i;
	if ((apu->divDivider & 7) == 7) {
		for (i = CH_SQUARE_1; i <= CH_SQUARE_2; ++i) {
			if (!apu->square[i].envelopeClock.clock) {
				apu->square[i].volumeCountdown = (apu->square[i].volumeCountdown - 1) & 7;
			}
		}
		if (!apu->noise.envelopeClock.clock) {
			apu->noise.volumeCountdown = (apu->noise.volumeCountdown - 1) & 7;
		}
	}

	for (i = CH_SQUARE_1; i <= CH_SQUARE_2; ++i) {
		if (apu->square[i].envelopeClock.clock) {
			_tickSquareEnvelope(gb, i);
			apu->square[i].envelopeClock.clock = false;
		}
	}
	if (apu->noise.envelopeClock.clock) {
		_tickNoiseEnvelope(gb);
		apu->noise.envelopeClock.clock = false;
	}

	if (apu->divDivider & 1) {
		for (i = CH_SQUARE_1; i <= CH_SQUARE_2; ++i) {
			struct GBApuSquare* ch = &apu->square[i];
			if (ch->lengthEnabled && ch->pulseLength && !--ch->pulseLength) {
				apu->isActive[i] = false;
				_updateSample(gb, i, 0);
			}
		}

		struct GBApuWave* wave = &apu->wave;
		if (wave->lengthEnabled && wave->pulseLength && !--wave->pulseLength) {
			if (apu->isActive[CH_WAVE] && IS_AGB(gb)) {
				if (wave->sampleCountdown == 0) {
					wave->currentSampleByte = gb->audio.ch3.wavedata8[((wave->currentSampleIndex + 1) & 0xF) >> 1];
				} else if (wave->sampleCountdown == 9) {
					wave->currentSampleByte = gb->audio.ch3.wavedata8[0];
				}
			}
			apu->isActive[CH_WAVE] = false;
			_updateSample(gb, CH_WAVE, 0);
		}

		struct GBApuNoise* noise = &apu->noise;
		if (noise->lengthEnabled && noise->pulseLength && !--noise->pulseLength) {
			apu->isActive[CH_NOISE] = false;
			_updateSample(gb, CH_NOISE, 0);
		}
	}

	if ((apu->divDivider & 3) == 3) {
		apu->squareSweepCountdown = (apu->squareSweepCountdown + 1) & 7;
		_triggerSweepCalculation(gb);
	}
}

void GBApuDivSecondaryEvent(struct GB* gb, int32_t when) {
	struct GBApu* apu = &gb->apu;
	_sync(gb, when);
	if (!apu->globalEnable) {
		return;
	}
	unsigned i;
	for (i = CH_SQUARE_1; i <= CH_SQUARE_2; ++i) {
		struct GBApuSquare* ch = &apu->square[i];
		if (apu->isActive[i] && ch->volumeCountdown == 0) {
			ch->volumeCountdown = gb->memory.io[i ? GB_REG_NR22 : GB_REG_NR12] & 7;
			ch->envelopeClock.clock = ch->volumeCountdown;
		}
	}
	struct GBApuNoise* ch = &apu->noise;
	if (apu->isActive[CH_NOISE] && ch->volumeCountdown == 0) {
		ch->volumeCountdown = gb->memory.io[GB_REG_NR42] & 7;
		ch->envelopeClock.clock = ch->volumeCountdown;
	}
}

void GBApuSample(struct GB* gb, int32_t when, int16_t* left, int16_t* right) {
	struct GBApu* apu = &gb->apu;
	_sync(gb, when);
	uint8_t nr51 = gb->memory.io[GB_REG_NR51];
	uint8_t nr50 = gb->memory.io[GB_REG_NR50];
	int sampleLeft = -0x8;
	int sampleRight = -0x8;
	unsigned i;
	for (i = 0; i < 4; ++i) {
		if (gb->audio.forceDisableCh[i] || !_dacEnabled(gb, i)) {
			continue;
		}
		if (nr51 & (0x10 << i)) {
			sampleLeft += apu->samples[i];
		}
		if (nr51 & (1 << i)) {
			sampleRight += apu->samples[i];
		}
	}
	*left = (sampleLeft << 3) * (1 + ((nr50 >> 4) & 7));
	*right = (sampleRight << 3) * (1 + (nr50 & 7));
}

#define APU_FIELDS \
	F8(apu->globalEnable) \
	F8(apu->samples[0]) F8(apu->samples[1]) F8(apu->samples[2]) F8(apu->samples[3]) \
	F8(apu->isActive[0]) F8(apu->isActive[1]) F8(apu->isActive[2]) F8(apu->isActive[3]) \
	F8(apu->divDivider) F8(apu->lfDiv) F8(apu->squareSweepCountdown) F8(apu->squareSweepCalculateCountdown) \
	F16(apu->sweepLengthAddend) F16(apu->shadowSweepSampleLength) \
	F8(apu->unshiftedSweep) F8(apu->enableZombieCalculateStepping) \
	F8(apu->channel1RestartHold) F16(apu->channel1CompletedAddend) \
	SQUARE(0) SQUARE(1) \
	F8(apu->wave.enable) F16(apu->wave.pulseLength) F8(apu->wave.shift) F16(apu->wave.sampleLength) \
	F8(apu->wave.lengthEnabled) F16(apu->wave.sampleCountdown) F8(apu->wave.currentSampleIndex) \
	F8(apu->wave.currentSampleByte) F8(apu->wave.waveFormJustRead) F8(apu->wave.pulsed) F8(apu->wave.buggedReadCountdown) \
	F16(apu->noise.pulseLength) F8(apu->noise.currentVolume) F8(apu->noise.volumeCountdown) F16(apu->noise.lfsr) \
	F8(apu->noise.narrow) F8(apu->noise.counterCountdown) F16(apu->noise.counter) F8(apu->noise.lengthEnabled) \
	F8(apu->noise.alignment) F8(apu->noise.currentLfsrSample) F8(apu->noise.delta) F8(apu->noise.countdownReloaded) \
	F8(apu->noise.dmgDelayedStart) F8(apu->noise.envelopeClock.locked) F8(apu->noise.envelopeClock.clock) \
	F8(apu->skipDivEvent)

#define SQUARE(N) \
	F16(apu->square[N].pulseLength) F8(apu->square[N].currentVolume) F8(apu->square[N].volumeCountdown) \
	F8(apu->square[N].currentSampleIndex) F8(apu->square[N].sampleSuppressed) F16(apu->square[N].sampleCountdown) \
	F16(apu->square[N].sampleLength) F8(apu->square[N].lengthEnabled) F8(apu->square[N].envelopeClock.locked) \
	F8(apu->square[N].envelopeClock.clock) F8(apu->square[N].delay) F8(apu->square[N].didTick)

void GBApuSerialize(const struct GB* gb, struct GBSerializedState* state) {
	const struct GBApu* apu = &gb->apu;
	uint8_t* p = (uint8_t*) state->reserved2;
	STORE_32LE(APU_STATE_MAGIC, 0, p);
	STORE_32LE(apu->lastStep - mTimingCurrentTime(&gb->timing), 4, p);
	p += 8;
#define F8(X) *p++ = (X);
#define F16(X) STORE_16LE(X, 0, p); p += 2;
	APU_FIELDS
#undef F8
#undef F16
}

void GBApuDeserialize(struct GB* gb, const struct GBSerializedState* state) {
	struct GBApu* apu = &gb->apu;
	const uint8_t* p = (const uint8_t*) state->reserved2;
	uint32_t magic;
	int32_t lastStep;
	LOAD_32LE(magic, 0, p);
	LOAD_32LE(lastStep, 4, p);
	p += 8;
	memset(apu, 0, sizeof(*apu));
	if (magic != APU_STATE_MAGIC) {
		// Older states only have the registers; restart the playing channels from them
		const uint8_t* io = state->io;
		apu->lastStep = mTimingCurrentTime(&gb->timing);
		apu->globalEnable = io[GB_REG_NR52] & 0x80;
		apu->lfDiv = 1;
		apu->wave.shift = (const uint8_t[]) { 4, 0, 1, 2 }[(io[GB_REG_NR32] >> 5) & 3];
		apu->wave.enable = io[GB_REG_NR30] & 0x80;
		apu->square[0].sampleLength = io[GB_REG_NR13] | ((io[GB_REG_NR14] & 7) << 8);
		apu->square[1].sampleLength = io[GB_REG_NR23] | ((io[GB_REG_NR24] & 7) << 8);
		apu->wave.sampleLength = io[GB_REG_NR33] | ((io[GB_REG_NR34] & 7) << 8);
		apu->square[0].currentVolume = io[GB_REG_NR12] >> 4;
		apu->square[1].currentVolume = io[GB_REG_NR22] >> 4;
		apu->noise.currentVolume = io[GB_REG_NR42] >> 4;
		unsigned i;
		for (i = 0; i < 4; ++i) {
			apu->isActive[i] = apu->globalEnable && (io[GB_REG_NR52] & (1 << i));
		}
		return;
	}
	apu->lastStep = mTimingCurrentTime(&gb->timing) + lastStep;
#define F8(X) (X) = *p++;
#define F16(X) LOAD_16LE(X, 0, p); p += 2;
	APU_FIELDS
#undef F8
#undef F16
}
