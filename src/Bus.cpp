#include "pce_core.h"
#include <cstring>

void Bus::connect(Cartridge* cart) {
    cartridge = cart;
}

void Bus::reset() {
    std::memset(wram, 0, sizeof(wram));
    std::memset(mpr, 0, sizeof(mpr));
}

uint8_t Bus::read(u32 addr) {
    // TODO: real MPR bank decode (8x 8KB windows -> 21-bit space).
    // Placeholder just reads through to cartridge for now.
    if (cartridge) return cartridge->read(addr);
    return 0xFF;
}

void Bus::write(u32 addr, uint8_t val) {
    // TODO: MMIO dispatch to VDC/VCE/PSG/timer/IRQ/joypad + MPR writes.
    (void)addr;
    (void)val;
}
