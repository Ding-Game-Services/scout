/*
 * VDC.cpp — HuC6270 register access + VRAM interface
 *
 * CONFIRMED via HuC6270 Video Display Controller Manual (project file):
 *   - §2.1.1/2.5: AR/SR at A1=0; data registers at A1=1, low byte A0=0,
 *     high byte A0=1, high-byte write commits and triggers side effects.
 *   - §2.1.3(1): AR=0x04 is explicitly documented as invalid — don't set it.
 *   - §2.1.3(3-6): MAWR/VWR write path and MARR/VRR read path, both with
 *     auto-increment on completion of the transfer (i.e. on high-byte
 *     write for MAWR/VWR, high-byte write for MARR triggering the read).
 *   - §2.1.3(7g): IW field (CR bits 11-12) selects increment amount:
 *     +1, +0x20, +0x40, +0x80.
 *   - §2.1.3(17-21): Block Transfer Control Register (DCR) and the
 *     VRAM-VRAM transfer registers (SOUR/DESR/LENR) — direction per
 *     SI/D and DI/D bits, length "M-1" encoding.
 *   - §2.1.3(2): Status register bit layout and read-clears-except-BSY
 *     behavior.
 *
 * NOT implemented this pass (flagged in pce_core.h):
 *   - Actual background/sprite pixel generation (runLine is a no-op)
 *   - VRAM-SATB block transfer (DVSSR) — register write is accepted and
 *     sets statusSatbEnd on the *next* call for now, but doesn't actually
 *     move data anywhere since SATB storage doesn't exist yet (that's
 *     part of the sprite rendering work, not yet built)
 *   - VRAM-VRAM transfer is NOT deferred to vblank like real hardware
 *     (manual: "can be performed during a vertical blanking period or in
 *     the burst mode") — this executes it immediately/atomically on the
 *     LENR high-byte write. Fine for correctness, wrong for timing-
 *     sensitive games.
 *   - BSY (status bit 6) never asserts — this implementation's VRAM
 *     access is instant/atomic rather than taking real bus cycles, so
 *     there's no window where BSY would meaningfully read as set. Only
 *     matters if a game busy-polls BSY expecting it to eventually clear
 *     after starting non-instant.
 */

#include "pce_core.h"
#include <cstdio>
#include <cstring>

void VDC::connect(Bus* b) {
    bus = b;
}

void VDC::reset() {
    std::memset(vram, 0, sizeof(vram));
    std::memset(regs, 0, sizeof(regs));
    vrrValue = 0;
    ar = 0;
    statusCollision = statusOver = statusScanlineMatch = false;
    statusSatbEnd = statusVramEnd = statusVblank = statusBusy = false;
}

void VDC::runLine() {
    // TODO: BG plane + sprite rendering per scanline — see class comment.
}

// ── register access ────────────────────────────────────────────────────

uint8_t VDC::readRegister(u16 offset) {
    bool isDataArea = (offset & 0x02) != 0;

    if (!isDataArea) {
        // SR read — clears all status bits except BSY per the manual.
        uint8_t sr = 0;
        if (statusCollision)      sr |= 0x01;
        if (statusOver)           sr |= 0x02;
        if (statusScanlineMatch)  sr |= 0x04;
        if (statusSatbEnd)        sr |= 0x08;
        if (statusVramEnd)        sr |= 0x10;
        if (statusVblank)         sr |= 0x20;
        if (statusBusy)           sr |= 0x40;

        statusCollision = statusOver = statusScanlineMatch = false;
        statusSatbEnd = statusVramEnd = statusVblank = false;
        // statusBusy intentionally left alone.

        return sr;
    }

    bool highByte = (offset & 0x01) != 0;

    // Only R02 (VRR) is meaningfully readable from the data area per the
    // register list's R/W column — everything else is write-only there.
    if (ar == REG_VWR_VRR) {
        uint8_t result = highByte ? static_cast<uint8_t>(vrrValue >> 8)
                                   : static_cast<uint8_t>(vrrValue & 0xFF);
        if (highByte) {
            // "Reading the high byte of the VRR register triggers reading
            // the next word of the VRAM" — confirmed §2.1.3(6).
            incrementAddress(regs[REG_MARR]);
            vrrValue = vram[regs[REG_MARR] & (kVramWords - 1)];
        }
        return result;
    }

    return 0xFF;   // write-only register read — open bus behavior assumed
}

void VDC::writeRegister(u16 offset, uint8_t val) {
    bool isDataArea = (offset & 0x02) != 0;

    if (!isDataArea) {
        // AR write. Manual explicitly warns AR=0x04 is invalid — mask to
        // the valid 0x00-0x13 range but don't special-case 0x04 beyond
        // that; the warning is about *hardware* behavior being undefined,
        // nothing we can meaningfully emulate differently here.
        ar = val & 0x1F;
        return;
    }

    bool highByte = (offset & 0x01) != 0;
    if (ar >= 0x14 || ar == 0x03 || ar == 0x04) return;   // reserved/invalid

    if (!highByte) {
        regs[ar] = static_cast<u16>((regs[ar] & 0xFF00) | val);
    } else {
        regs[ar] = static_cast<u16>((regs[ar] & 0x00FF) | (static_cast<u16>(val) << 8));
        onHighByteWritten(ar);
    }
}

// ── side effects triggered by high-byte writes ──────────────────────────

void VDC::incrementAddress(u16& addr) {
    // IW field: CR bits 11-12 (bits 3-4 of the low byte pairing... per
    // the manual's bit numbering, IW occupies CR bits 11 and 12 directly).
    u8 iw = static_cast<u8>((regs[REG_CR] >> 11) & 0x03);
    switch (iw) {
        case 0: addr = static_cast<u16>(addr + 1);    break;
        case 1: addr = static_cast<u16>(addr + 0x20); break;
        case 2: addr = static_cast<u16>(addr + 0x40); break;
        case 3: addr = static_cast<u16>(addr + 0x80); break;
    }
}

void VDC::onHighByteWritten(u8 regIndex) {
    switch (regIndex) {
        case REG_MARR:
            // High byte of MARR write triggers an immediate VRAM read
            // into VRR, then auto-increments MARR — confirmed §2.1.3(4).
            vrrValue = vram[regs[REG_MARR] & (kVramWords - 1)];
            incrementAddress(regs[REG_MARR]);
            break;

        case REG_VWR_VRR:
            // High byte of VWR write triggers an immediate VRAM write,
            // then auto-increments MAWR — confirmed §2.1.3(5).
            vram[regs[REG_MAWR] & (kVramWords - 1)] = regs[REG_VWR_VRR];
            incrementAddress(regs[REG_MAWR]);
            break;

        case REG_LENR:
            // High byte of LENR write triggers the VRAM-VRAM block
            // transfer — confirmed §2.1.3(21) NOTE (a): "triggered by
            // access to the high byte of the block length register."
            doVramToVramBlockTransfer();
            break;

        case REG_DVSSR:
            // VRAM-SATB transfer trigger — confirmed §2.1.3(21) NOTE (b),
            // but SATB storage itself isn't implemented yet (see file
            // header). Just flag completion so status polling doesn't
            // hang indefinitely; no data actually moves.
            statusSatbEnd = true;
            break;

        default:
            break;   // MAWR, CR, RCR, BXR, BYR, MWR, HSR, HDR, VPR, VDR,
                      // VCR, DCR, SOUR, DESR — just hold their value,
                      // consumed by runLine()'s rendering (not yet built)
                      // or by the block-transfer trigger paths above.
    }
}

void VDC::doVramToVramBlockTransfer() {
    u16 src = regs[REG_SOUR];
    u16 dst = regs[REG_DESR];
    u32 len = static_cast<u32>(regs[REG_LENR]) + 1;   // "M-1" encoding, confirmed §2.1.3(20)

    bool srcDec = (regs[REG_DCR] & 0x04) != 0;   // SI/D, bit 2
    bool dstDec = (regs[REG_DCR] & 0x08) != 0;   // DI/D, bit 3

    for (u32 i = 0; i < len; i++) {
        vram[dst & (kVramWords - 1)] = vram[src & (kVramWords - 1)];
        src = static_cast<u16>(srcDec ? src - 1 : src + 1);
        dst = static_cast<u16>(dstDec ? dst - 1 : dst + 1);
    }

    statusVramEnd = true;
}

size_t VDC::dumpState(char* buf, size_t buf_size) const {
    return static_cast<size_t>(std::snprintf(buf, buf_size,
        "AR=%02X MAWR=%04X MARR=%04X CR=%04X SOUR=%04X DESR=%04X LENR=%04X\n",
        ar, regs[REG_MAWR], regs[REG_MARR], regs[REG_CR],
        regs[REG_SOUR], regs[REG_DESR], regs[REG_LENR]));
}
