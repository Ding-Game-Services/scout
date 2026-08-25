/*
 * Joypad.cpp — standard 2-button PCE pad
 *
 * CONFIRMED via Hardware Manual §2.6: Port K (input)/Port O (output) live
 * at physical $1FF000-$1FF3FF, i.e. offset $1000 within the hardware
 * page — matches what Bus.cpp already routes here.
 *
 * CONFIRMED via PCE_CPU_Hardware_Documentation.htm (project file) —
 * this resolves what was previously flagged as an unverified community
 * guess, and the guess turned out backwards:
 *   write ($1000): bit1 = CLR line, bit0 = SEL line
 *     BUG FIX: this file previously read SEL from bit1 — swapped.
 *   read ($1000):  bit6 = country bit (1=JPN, 0=USA), bits5-4 unused,
 *     bits3-0 = 4-bit gamepad data, bit7 unused
 *     Country bit wasn't modeled at all before this pass — added below,
 *     hardcoded to 0 (USA/international) since there's no region-config
 *     plumbing yet (ding_set_region is still a no-op stub).
 *
 * Multitap (up to 5 pads) not modeled — single pad on port 0 only.
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

    // bit6 = country bit (1=JPN, 0=USA) — hardcoded USA/international
    // for now. bit7 and bits5-4 documented unused; left high along with
    // country=0 to match typical unconnected-input pull-up behavior.
    constexpr uint8_t kCountryBit = 0;   // TODO: wire to real region config
    return static_cast<uint8_t>(0x80 | (kCountryBit << 6) | 0x30 | nibble);
}

void Joypad::writeRegister(u16 offset, uint8_t val) {
    (void)offset;
    // CONFIRMED: bit0 = SEL, bit1 = CLR (was swapped before this pass).
    selectHigh = (val & 0x01) != 0;
    // bit1 (CLR) — multitap counter reset, no-op until multitap exists
}
