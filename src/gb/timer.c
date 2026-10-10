/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/timer.h>

#include <mgba/internal/sm83/sm83.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/io.h>
#include <mgba/internal/gb/serialize.h>

void _GBTimerIRQ(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(timing);
	UNUSED(cyclesLate);
	struct GBTimer* timer = context;
	timer->p->memory.io[GB_REG_TIMA] = timer->p->memory.io[GB_REG_TMA];
	timer->p->memory.io[GB_REG_IF] |= (1 << GB_IRQ_TIMER);
	GBUpdateIRQs(timer->p);
}

static void _GBTimerDivIncrement(struct GBTimer* timer, uint32_t cyclesLate) {
	int tMultiplier = 2 - timer->p->doubleSpeed;
	while (timer->nextDiv >= GB_DMG_DIV_PERIOD * tMultiplier) {
		timer->nextDiv -= GB_DMG_DIV_PERIOD * tMultiplier;

		// Make sure to trigger when the correct bit is a falling edge
		if (timer->timaPeriod > 0 && (timer->internalDiv & (timer->timaPeriod - 1)) == timer->timaPeriod - 1) {
			++timer->p->memory.io[GB_REG_TIMA];
			if (!timer->p->memory.io[GB_REG_TIMA]) {
				mTimingSchedule(&timer->p->timing, &timer->irq, 7 * tMultiplier - ((timer->p->cpu->executionState * tMultiplier - cyclesLate) & (3 * tMultiplier)));
			}
		}
		// The APU steps on the falling edge of counter bit 12 (13 in double speed), envelopes reload on the rising edge
		unsigned apuMask = (0x200 << timer->p->doubleSpeed) - 1;
		unsigned apuBits = timer->internalDiv & apuMask;
		if (apuBits == apuMask) {
			GBApuDivEvent(timer->p, mTimingCurrentTime(&timer->p->timing) - timer->nextDiv - 1);
		} else if (apuBits == apuMask >> 1) {
			GBApuDivSecondaryEvent(timer->p, mTimingCurrentTime(&timer->p->timing) - timer->nextDiv - 1);
		}
		++timer->internalDiv;
		timer->p->memory.io[GB_REG_DIV] = timer->internalDiv >> 4;
	}
}

void _GBTimerUpdate(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBTimer* timer = context;
	timer->nextDiv += cyclesLate;
	_GBTimerDivIncrement(timer, cyclesLate);
	// Batch div increments
	int divsToGo = 16 - (timer->internalDiv & 15);
	int timaToGo = INT_MAX;
	if (timer->timaPeriod) {
		timaToGo = timer->timaPeriod - (timer->internalDiv & (timer->timaPeriod - 1));
	}
	if (timaToGo < divsToGo) {
		divsToGo = timaToGo;
	}
	timer->nextDiv = GB_DMG_DIV_PERIOD * divsToGo * (2 - timer->p->doubleSpeed);
	mTimingSchedule(timing, &timer->event, timer->nextDiv - cyclesLate);
}

void GBTimerReset(struct GBTimer* timer) {
	timer->event.context = timer;
	timer->event.name = "GB Timer";
	timer->event.callback = _GBTimerUpdate;
	timer->event.priority = 0x21;
	timer->irq.context = timer;
	timer->irq.name = "GB Timer IRQ";
	timer->irq.callback = _GBTimerIRQ;
	timer->irq.priority = 0x20;

	timer->nextDiv = GB_DMG_DIV_PERIOD * 2;
	timer->timaPeriod = 1024 >> 4;
}

void GBTimerDivReset(struct GBTimer* timer) {
	timer->nextDiv -= mTimingUntil(&timer->p->timing, &timer->event);
	mTimingDeschedule(&timer->p->timing, &timer->event);
	_GBTimerDivIncrement(timer, 0);
	int tMultiplier = 2 - timer->p->doubleSpeed;
	if (((timer->internalDiv << 1) | ((timer->nextDiv >> (4 - timer->p->doubleSpeed)) & 1)) & timer->timaPeriod) {
		++timer->p->memory.io[GB_REG_TIMA];
		if (!timer->p->memory.io[GB_REG_TIMA]) {
			mTimingSchedule(&timer->p->timing, &timer->irq, (7 - (timer->p->cpu->executionState & 3)) * tMultiplier);
		}
	}
	if (timer->internalDiv & (0x100 << timer->p->doubleSpeed)) {
		GBApuDivEvent(timer->p, mTimingCurrentTime(&timer->p->timing));
	}
	timer->p->memory.io[GB_REG_DIV] = 0;
	timer->internalDiv = 0;
	timer->nextDiv = GB_DMG_DIV_PERIOD * (2 - timer->p->doubleSpeed);
	mTimingSchedule(&timer->p->timing, &timer->event, timer->nextDiv - ((timer->p->cpu->executionState + 1) & 3) * tMultiplier);
}

// The system counter now, in ticks of the CPU clock; internalDiv lags it while steps are batched
uint16_t GBTimerCounter(const struct GBTimer* timer) {
	int32_t elapsed = timer->nextDiv - mTimingUntil(&timer->p->timing, &timer->event);
	return (timer->internalDiv << 4) + elapsed / (2 - timer->p->doubleSpeed);
}

uint8_t GBTimerUpdateTAC(struct GBTimer* timer, GBRegisterTAC tac) {
	int tMultiplier = 2 - timer->p->doubleSpeed;
	timer->nextDiv -= mTimingUntil(&timer->p->timing, &timer->event);
	mTimingDeschedule(&timer->p->timing, &timer->event);
	_GBTimerDivIncrement(timer, ((timer->p->cpu->executionState + 2) & 3) * tMultiplier);

	int timaPeriod = 0;
	if (GBRegisterTACIsRun(tac)) {
		switch (GBRegisterTACGetClock(tac)) {
		case 0:
			timaPeriod = 1024 >> 4;
			break;
		case 1:
			timaPeriod = 16 >> 4;
			break;
		case 2:
			timaPeriod = 64 >> 4;
			break;
		case 3:
			timaPeriod = 256 >> 4;
			break;
		}
	}
	// TIMA counts falling edges of the selected bit, so stopping it or selecting a clear bit can tick it
	int counter = (timer->internalDiv << 1) | ((timer->nextDiv >> (4 - timer->p->doubleSpeed)) & 1);
	if ((counter & timer->timaPeriod) && !(counter & timaPeriod)) {
		++timer->p->memory.io[GB_REG_TIMA];
		if (!timer->p->memory.io[GB_REG_TIMA]) {
			// The write lands a T-cycle into its M-cycle, and TIMA reloads at the end of it
			mTimingSchedule(&timer->p->timing, &timer->irq, 3 * tMultiplier);
		}
	}
	timer->timaPeriod = timaPeriod;

	// nextDiv is the time since the last DIV step, so the next one is a period after that step
	int32_t untilStep = GB_DMG_DIV_PERIOD * tMultiplier - timer->nextDiv;
	timer->nextDiv = GB_DMG_DIV_PERIOD * tMultiplier;
	mTimingSchedule(&timer->p->timing, &timer->event, untilStep);
	return tac;
}

void GBTimerSerialize(const struct GBTimer* timer, struct GBSerializedState* state) {
	STORE_32LE(timer->nextDiv, 0, &state->timer.nextDiv);
	STORE_32LE(timer->internalDiv, 0, &state->timer.internalDiv);
	STORE_32LE(timer->timaPeriod, 0, &state->timer.timaPeriod);
	STORE_32LE(timer->event.when - mTimingCurrentTime(&timer->p->timing), 0, &state->timer.nextEvent);
	STORE_32LE(timer->irq.when - mTimingCurrentTime(&timer->p->timing), 0, &state->timer.nextIRQ);
	GBSerializedTimerFlags flags = GBSerializedTimerFlagsSetIrqPending(0, mTimingIsScheduled(&timer->p->timing, &timer->irq));
	state->timer.flags = flags;
}

void GBTimerDeserialize(struct GBTimer* timer, const struct GBSerializedState* state) {
	LOAD_32LE(timer->nextDiv, 0, &state->timer.nextDiv);
	LOAD_32LE(timer->internalDiv, 0, &state->timer.internalDiv);
	LOAD_32LE(timer->timaPeriod, 0, &state->timer.timaPeriod);

	uint32_t when;
	LOAD_32LE(when, 0, &state->timer.nextEvent);
	mTimingSchedule(&timer->p->timing, &timer->event, when);

	GBSerializedTimerFlags flags = state->timer.flags;

	LOAD_32LE(when, 0, &state->timer.nextIRQ);
	if (GBSerializedTimerFlagsIsIrqPending(flags)) {
		mTimingSchedule(&timer->p->timing, &timer->irq, when);
	} else {
		timer->irq.when = when + mTimingCurrentTime(&timer->p->timing);
	}
}
