/*
 * Cartridge.cpp — ROM storage, with copier-header stripping
 *
 * BUG FOUND (harness testing): ROMs loading with PC=0 or landing on
 * garbage after reset — caused by a 512-byte copier header prepended to
 * the dump (a well-known, common artifact on PCE/HuCard ROM dumps from
 * old backup-copier hardware, same idea as the iNES header on NES
 * dumps). If present and not stripped, every address calculation is
 * offset by 512 bytes from where it should be, including the reset
 * vector fetch.
 *
 * Detection formula CONFIRMED via RetroAchievements' PC Engine hashing
 * spec (the precise, authoritative version — an earlier pass here used
 * a looser approximation, size%1024==512, which agrees with this on
 * common cases but risked a false-positive strip on a ROM that doesn't
 * actually have a header): a header is present if and only if the file
 * size is exactly 512 bytes more than a multiple of 128KB (131072
 * bytes). Anything else gets hashed/loaded as-is.
 */

#include "pce_core.h"

bool Cartridge::load(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;

    constexpr size_t kHeaderSize = 512;
    constexpr size_t kBankSize = 131072;   // 128KB
    size_t offset = 0;
    if (len > kHeaderSize && (len - kHeaderSize) % kBankSize == 0) {
        offset = kHeaderSize;
    }

    rom.assign(data + offset, data + len);
    return true;
}

uint8_t Cartridge::read(u32 addr) const {
    if (rom.empty()) return 0xFF;
    return rom[addr % rom.size()];   // placeholder — real HuCard mirroring TBD
}
