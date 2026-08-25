/*
 * Timer.cpp — HuC6280 built-in timer
 *
 * CONFIRMED via Hardware Manual §2.7-2.9, independently re-confirmed
 * via PCE_CPU_Hardware_Documentation.htm (project file: identical
 * $0C00/$0C01 register split and "interrupt raised on carry, i.e. when
 * about to decrement from zero" behavior):
 *   - $0C00 (even offset): reload value (write) / live downcounter (read)
 *   - $0C01 (odd offset): control register, bit0 = start(1)/stop(0)
 *   - Divider: downcounter decrements at OSC1/3/1024. At 7.16MHz (CSH),
 *     that works out to ~1024 CPU cycles/decrement — matches kDivider.
 *     NOTE: this hasn't been re-derived for 1.79MHz (CSL) yet; the fixed
 *     1024-cycle divider is likely wrong at low speed and needs revisiting
 *     once CSH/CSL actually drives real timing instead of just setting
 *     the speed register with no effect.
 */

#include "pce_core.h"

void Timer::reset() {
    reloadValue = 0;
    counter = 0;
    running = false;
    cycleAccum = 0;
}

bool Timer::tick(u32 cpuCycles) {
    if (!running) return false;

    cycleAccum += cpuCycles;
    bool fired = false;

    while (cycleAccum >= kDivider) {
        cycleAccum -= kDivider;
        if (counter == 0) {
            counter = reloadValue & 0x7F;
            fired = true;
        } else {
            counter--;
        }
    }

    return fired;
}

uint8_t Timer::readRegister(u16 offset) {
    if (offset == 0x00) return counter & 0x7F;
    return 0xFF;
}

void Timer::writeRegister(u16 offset, uint8_t val) {
    if (offset == 0x00) {
        reloadValue = val & 0x7F;
    } else if (offset == 0x01) {
        bool wasRunning = running;
        running = (val & 0x01) != 0;
        if (running && !wasRunning) {
            counter = reloadValue;   // reload on start, matches most documented behavior
            cycleAccum = 0;
        }
    }
}
