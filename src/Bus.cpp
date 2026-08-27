/*
 * Bus.cpp — MPR banking + physical memory dispatch
 *
 * Hardware page sub-region boundaries (VDC $0000, VCE $0400, PSG $0800,
 * Timer $0C00, Joypad $1000, IRQ $1400) are CONFIRMED via Hardware
 * Manual §2.9's Device/Register Address table.
 *
 * NOT yet implemented:
 *   - ROM mirroring for HuCards smaller than the full 0x00-0x7F bank range
 *     (currently just masks into whatever the cartridge has, same as the
 *     Cartridge::read placeholder — needs real size-based wrap logic)
 *   - Battery-backed RAM (physical bank $F7)
 */

#include "pce_core.h"
#include <cstring>

void Bus::connect(Cartridge* cart)     { cartridge = cart; }
void Bus::connect(VDC* v)              { vdc = v; }
void Bus::connect(VCE* v)              { vce = v; }
void Bus::connect(PSG* p)              { psg = p; }
void Bus::connect(Timer* t)            { timer = t; }
void Bus::connect(Joypad* j)           { joypad = j; }
void Bus::connect(IrqController* irq)  { irqController = irq; }

void Bus::tickTimer(u32 cpuCycles) {
    if (!timer || !irqController) return;
    if (timer->tick(cpuCycles)) {
        irqController->setLine(2, true);   // TIQ
    }
}

void Bus::reset() {
    std::memset(wram, 0, sizeof(wram));
    std::memset(mpr, 0, sizeof(mpr));

    // Real hardware: MPR7 (top 8KB page, $E000-$FFFF) must map to ROM bank
    // 0 out of reset so the CPU can fetch its reset vector from $FFFC —
    // everything else is don't-care until software sets it via TAM.
    mpr[7] = 0x00;
}

uint8_t Bus::readMPR(uint8_t index) const {
    return mpr[index & 0x07];
}

void Bus::writeMPR(uint8_t index, uint8_t bank, u16 debugPC) {
    index &= 0x07;
    mpr[index] = bank;

    mprLog[mprLogHead] = { debugPC, index, bank };
    mprLogHead = (mprLogHead + 1) % kMprLogSize;
    if (mprLogCount < kMprLogSize) mprLogCount++;
}

size_t Bus::getMprLog(MprLogEntry* out, size_t maxEntries) const {
    if (!out || maxEntries == 0) return 0;
    size_t n = mprLogCount < maxEntries ? maxEntries : mprLogCount;
    n = mprLogCount < maxEntries ? mprLogCount : maxEntries;
    size_t start = (mprLogCount < kMprLogSize) ? 0 : mprLogHead;
    for (size_t i = 0; i < n; i++) {
        out[i] = mprLog[(start + i) % kMprLogSize];
    }
    return n;
}

Bus::PhysAddr Bus::resolve(u32 cpuAddr) const {
    uint8_t page = static_cast<uint8_t>((cpuAddr >> 13) & 0x07);
    return { mpr[page], static_cast<u16>(cpuAddr & 0x1FFF) };
}

uint8_t Bus::read(u32 addr) {
    PhysAddr p = resolve(addr);

    if (p.bank <= 0x7F) {
        return cartridge ? cartridge->read((static_cast<u32>(p.bank) << 13) | p.offset) : 0xFF;
    }
    if (p.bank == 0xF8) {
        return wram[p.offset];
    }
    if (p.bank == 0xFF) {
        return readHardwarePage(p.offset);
    }

    // $F7 (backup RAM) and anything else: open bus for now.
    return 0xFF;
}

void Bus::write(u32 addr, uint8_t val, u16 debugPC) {
    // Watchpoints check the CPU-visible address (pre-bank-resolve) since
    // that's what a human debugging boot code actually reasons about
    // ("zero-page $04"), not the physical bank+offset it happens to
    // resolve to under current MPR mapping.
    checkWatchpoint(static_cast<u16>(addr), val, debugPC);

    PhysAddr p = resolve(addr);

    if (p.bank <= 0x7F) {
        return;   // ROM — writes ignored
    }
    if (p.bank == 0xF8) {
        wram[p.offset] = val;
        return;
    }
    if (p.bank == 0xFF) {
        writeHardwarePage(p.offset, val, debugPC);
        return;
    }

    // $F7 backup RAM write support TODO; everything else no-op.
}

void Bus::addWatchpoint(u16 addr) {
    for (size_t i = 0; i < watchCount; i++) {
        if (watchAddrs[i] == addr) return;   // already watched
    }
    if (watchCount < kMaxWatchpoints) {
        watchAddrs[watchCount++] = addr;
    }
}

void Bus::clearWatchpoints() {
    watchCount = 0;
    watchLogHead = 0;
    watchLogCount = 0;
    std::memset(watchLog, 0, sizeof(watchLog));
}

void Bus::checkWatchpoint(u16 addr, u8 val, u16 debugPC) {
    for (size_t i = 0; i < watchCount; i++) {
        if (watchAddrs[i] == addr) {
            watchLog[watchLogHead] = { debugPC, addr, val };
            watchLogHead = (watchLogHead + 1) % kWatchLogSize;
            if (watchLogCount < kWatchLogSize) watchLogCount++;
            return;
        }
    }
}

size_t Bus::getWatchLog(WatchEntry* out, size_t maxEntries) const {
    if (!out || maxEntries == 0) return 0;
    size_t n = watchLogCount < maxEntries ? watchLogCount : maxEntries;
    size_t start = (watchLogCount < kWatchLogSize) ? 0 : watchLogHead;
    for (size_t i = 0; i < n; i++) {
        out[i] = watchLog[(start + i) % kWatchLogSize];
    }
    return n;
}

uint8_t Bus::readHardwarePage(u16 offset) {
    // Hardware page sub-regions, decoded on the low bits of the 8KB
    // window (only the low ~11 bits are meaningfully decoded by real
    // hardware; each region mirrors across its slice). Range boundaries
    // for timer/joypad/IRQ are best-effort pending doc cross-check.
    if (offset < 0x0400) return vdc ? vdc->readRegister(offset) : 0xFF;
    if (offset < 0x0800) return vce ? vce->readRegister(offset - 0x0400) : 0xFF;
    if (offset < 0x0C00) return psg ? psg->readRegister(offset - 0x0800) : 0xFF;
    if (offset < 0x1000) return timer ? timer->readRegister(offset - 0x0C00) : 0xFF;
    if (offset < 0x1400) return joypad ? joypad->readRegister(offset - 0x1000) : 0xFF;
    if (offset < 0x1800) return irqController ? irqController->readRegister(offset - 0x1400) : 0xFF;
    return 0xFF;
}

void Bus::writeHardwarePage(u16 offset, uint8_t val, u16 debugPC) {
    if (offset < 0x0400) { if (vdc) vdc->writeRegister(offset, val, debugPC); return; }
    if (offset < 0x0800) { if (vce) vce->writeRegister(offset - 0x0400, val); return; }
    if (offset < 0x0C00) { if (psg) psg->writeRegister(offset - 0x0800, val); return; }
    if (offset < 0x1000) { if (timer) timer->writeRegister(offset - 0x0C00, val); return; }
    if (offset < 0x1400) { if (joypad) joypad->writeRegister(offset - 0x1000, val); return; }
    if (offset < 0x1800) { if (irqController) irqController->writeRegister(offset - 0x1400, val); return; }
}
