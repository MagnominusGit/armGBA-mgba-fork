/* Copyright (c) 2015-2022 Lior Halphon
 * Copyright (c) 2026 Magnominus
 *
 * Ported from SameBoy's APU (MIT License); see src/gb/apu.c for the full notice. */
#ifndef GB_APU_H
#define GB_APU_H

#include <mgba-util/common.h>

CXX_GUARD_START

struct GB;
struct GBSerializedState;

struct GBApuEnvelopeClock {
	bool locked;
	bool clock;
};

struct GBApuSquare {
	uint16_t pulseLength;
	uint8_t currentVolume;
	uint8_t volumeCountdown;
	uint8_t currentSampleIndex;
	bool sampleSuppressed;
	uint16_t sampleCountdown;
	uint16_t sampleLength;
	bool lengthEnabled;
	struct GBApuEnvelopeClock envelopeClock;
	uint8_t delay;
	bool didTick;
};

struct GBApuWave {
	bool enable;
	uint16_t pulseLength;
	uint8_t shift;
	uint16_t sampleLength;
	bool lengthEnabled;
	uint16_t sampleCountdown;
	uint8_t currentSampleIndex;
	uint8_t currentSampleByte;
	bool waveFormJustRead;
	bool pulsed;
	uint8_t buggedReadCountdown;
};

struct GBApuNoise {
	uint16_t pulseLength;
	uint8_t currentVolume;
	uint8_t volumeCountdown;
	uint16_t lfsr;
	bool narrow;
	uint8_t counterCountdown;
	uint16_t counter;
	bool lengthEnabled;
	uint8_t alignment;
	bool currentLfsrSample;
	int8_t delta;
	bool countdownReloaded;
	uint8_t dmgDelayedStart;
	struct GBApuEnvelopeClock envelopeClock;
};

struct GBApu {
	bool globalEnable;
	uint8_t samples[4];
	bool isActive[4];
	uint8_t divDivider;
	uint8_t lfDiv;
	uint8_t squareSweepCountdown;
	uint8_t squareSweepCalculateCountdown;
	uint16_t sweepLengthAddend;
	uint16_t shadowSweepSampleLength;
	bool unshiftedSweep;
	bool enableZombieCalculateStepping;
	uint8_t channel1RestartHold;
	uint16_t channel1CompletedAddend;
	struct GBApuSquare square[2];
	struct GBApuWave wave;
	struct GBApuNoise noise;
	uint8_t skipDivEvent;

	// Not cleared by power cycling: the time of the last counter step the channels have run through
	int32_t lastStep;
};

void GBApuReset(struct GB* gb);
void GBApuSkipBIOS(struct GB* gb);
void GBApuWrite(struct GB* gb, unsigned reg, uint8_t value);
uint8_t GBApuRead(struct GB* gb, unsigned reg);
void GBApuDivEvent(struct GB* gb, int32_t when);
void GBApuDivSecondaryEvent(struct GB* gb, int32_t when);
void GBApuSample(struct GB* gb, int32_t when, int16_t* left, int16_t* right);
void GBApuSerialize(const struct GB* gb, struct GBSerializedState* state);
void GBApuDeserialize(struct GB* gb, const struct GBSerializedState* state);

CXX_GUARD_END

#endif
