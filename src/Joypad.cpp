/*
 * Joypad.cpp — standard 2-button PCE pad
 *
 * CONFIRMED via Hardware Manual §2.6: Port K (input)/Port O (output) live
 * at physical $1FF000-$1FF3FF, i.e. offset $1000 within the hardware
 * page — matches what Bus.cpp already routes here.
 *
 * NOT covered by the CPU hardware manual (it documents generic Port K/O,
 * not the joypad-specific SEL/CLR read protocol layered on top by the
 * controller hardware itself) — SEL/CLR bit assignment below is still
 * best-effort, sourced from general PCE community documentation rather
 * than an Anthropic-verifiable primary source. Multitap not modeled;
 * single pad on port 0 only.
 *
 * Assumed button index layout (matches DingInputDescriptor order we'll
 * declare once ding_core_pce.cpp exposes real descriptors):
 *   0=Up 1=Down 2=Left 3=Right 4=Button I 5=Button II 6=Select 7=Run
 */

#include "pce_core.h"

void Joypad::reset() {
    buttonState = 0xFFFF;   // active-low: all released
    selectHigh = false;
}

void Joypad::setButton(uint8_t index, bool pressed) {
    if (index > 15) return;
    if (pressed) buttonState &= static_cast<uint16_t>(~(1u << index));
    else         buttonState |= static_cast<uint16_t>(1u << index);
}

uint8_t Joypad::readRegister(u16 offset) {
    (void)offset;
    // Low nibble = currently selected 4 buttons, active-low.
    uint8_t nibble = selectHigh
        ? static_cast<uint8_t>((buttonState >> 4) & 0x0F)   // Button I/II/Select/Run
        : static_cast<uint8_t>(buttonState & 0x0F);         // D-Pad
    return static_cast<uint8_t>(0xF0 | nibble);
}

void Joypad::writeRegister(u16 offset, uint8_t val) {
    (void)offset;
    selectHigh = (val & 0x02) != 0;
    // bit0 (CLR) — multitap counter reset, no-op until multitap exists
}
