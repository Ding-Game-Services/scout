#include "pce_core.h"

bool Cartridge::load(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    rom.assign(data, data + len);
    return true;
}

uint8_t Cartridge::read(u32 addr) const {
    if (rom.empty()) return 0xFF;
    return rom[addr % rom.size()];   // placeholder — real HuCard mirroring TBD
}
