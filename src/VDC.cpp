/*
 * VDC.cpp — HuC6270 register access + VRAM interface
 *
 * CONFIRMED via HuC6270 Video Display Controller Manual (project file)
 * and cross-checked against PCE_Hardware_Documentation.htm (community
 * reference, project file — matched on every point of overlap: register
 * numbering, auto-increment table, DCR bit layout, LENR-high-byte DMA
 * trigger, SCREEN size table):
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
 *   - VDC interrupts route through IRQ1 — confirmed via HuC62 Tech
 *     Notes' irq1_handle example. Now wired: VRAM-VRAM and VRAM-SATB
 *     transfer completion assert IRQ1 through IrqController, gated by
 *     DCR's DVC/DSC enable bits (confirmed identically in both the
 *     official manual and PCE_Hardware_Documentation.htm). Reading SR
 *     drops the line again, matching its clear-on-read behavior.
 *
 * NOT implemented this pass (flagged in pce_core.h):
 *   - Actual background/sprite pixel generation (runLine is a no-op)
 *   - Collision and scanline-match interrupts aren't wired yet — they
 *     depend on real sprite/scanline logic that doesn't exist until the
 *     rendering pass lands. Only the two DMA-completion interrupts are
 *     live right now, since those are the only status conditions
 *     anything currently sets.
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

void VDC::connect(IrqController* irq) {
    irqController = irq;
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
    renderBackgroundLine(currentLine);
    currentLine++;
    if (currentLine >= kVisibleHeight) currentLine = 0;
}

// ── background rendering ────────────────────────────────────────────────
// CONFIRMED tile layout via VDC Manual §2.3.2/2.3.3:
//   - BAT entry (one word per character cell, 32x32-cell virtual screen,
//     top-left = VRAM address 0): bits 15-12 = CG COLOR (4-bit block),
//     bits 11-0 = character code.
//   - Character data lives at VRAM word address (character_code * 16).
//     Per scanline row (0-7) within the 8x8 character:
//       word at (charBase + row)     holds CH0 in the low byte, CH1 in
//                                     the high byte (confirmed via the
//                                     manual's Fig ② "CH1,CH0" fetch)
//       word at (charBase + 8 + row) holds CH2 in the low byte, CH3 in
//                                     the high byte (Fig ③ "CH3,CH2")
//     Each pixel's 4-bit color = CH3:CH2:CH1:CH0 (CH3 is the MSB), per
//     §2.3.5's "VD3-VD0 = each bit of CH3-CH0" video-output table.
//   - Bit-within-byte -> x-position mapping (MSB = leftmost pixel) is
//     the conventional interpretation but not spelled out pixel-by-pixel
//     in the manual's diagrams — flagging this specific detail as the
//     one part of this function that could be backwards pending a real
//     test-ROM comparison.
//
// NOT implemented this pass: sprites, 4-color BG mode, SCREEN sizes
// other than 32x32, and the CG COLOR block correctly offsetting into
// VCE's *background* half of the color table (currently assumed to
// start at color-table index 0 — matches the background half per VCE
// manual §2.2.1, so this one should already be correct).
void VDC::renderBackgroundLine(int line) {
    if (!bus) return;

    // Virtual screen is fixed at 32x32 characters (SCREEN=0) for now.
    constexpr int kVirtualCharsX = 32;
    constexpr int kVirtualPixelsX = kVirtualCharsX * 8;   // 256
    constexpr int kVirtualPixelsY = kVirtualCharsX * 8;   // 256 (32 rows too)

    int scrollX = regs[REG_BXR] % kVirtualPixelsX;
    int scrollY = regs[REG_BYR] % kVirtualPixelsY;
    int srcY = (line + scrollY) % kVirtualPixelsY;
    int charRow = srcY / 8;
    int rowInChar = srcY % 8;

    for (int x = 0; x < kVisibleWidth; x++) {
        int srcX = (x + scrollX) % kVirtualPixelsX;
        int charCol = srcX / 8;
        int colInChar = srcX % 8;

        u16 batAddr = static_cast<u16>(charRow * kVirtualCharsX + charCol);
        u16 batEntry = vram[batAddr & (kVramWords - 1)];
        u16 charCode = batEntry & 0x0FFF;
        u8 cgColor = static_cast<u8>((batEntry >> 12) & 0x0F);

        u16 charBase = static_cast<u16>(charCode * 16);
        u16 word01 = vram[(charBase + rowInChar) & (kVramWords - 1)];
        u16 word23 = vram[(charBase + 8 + rowInChar) & (kVramWords - 1)];
        u8 ch0 = static_cast<u8>(word01 & 0xFF);
        u8 ch1 = static_cast<u8>((word01 >> 8) & 0xFF);
        u8 ch2 = static_cast<u8>(word23 & 0xFF);
        u8 ch3 = static_cast<u8>((word23 >> 8) & 0xFF);

        int bitPos = 7 - colInChar;   // MSB = leftmost pixel (flagged above)
        u8 pixel = static_cast<u8>(
            ((ch0 >> bitPos) & 1)       |
            (((ch1 >> bitPos) & 1) << 1) |
            (((ch2 >> bitPos) & 1) << 2) |
            (((ch3 >> bitPos) & 1) << 3));

        // Video code per §2.3.5: VD8=0(background), VD7-4=CG COLOR,
        // VD3-0=pixel pattern bits. CONFIRMED special case: when the
        // pattern is 0, VD7-4 are forced to 0 regardless of CG COLOR —
        // this is what makes pattern-0 always resolve to the universal
        // background color (block 0, color 0) instead of "block N,
        // color 0", which would otherwise be a different clear color
        // per tile.
        u16 videoCode = (pixel == 0) ? 0 : static_cast<u16>((cgColor << 4) | pixel);
        videoCodes[line * kVisibleWidth + x] = videoCode;
    }
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

        // CONFIRMED via HuC62 Tech Notes ("irq1_handle" routine): VDC
        // interrupts route through IRQ1. All status bits clear on SR
        // read, so drop the line too — matches the "reading SR clears
        // status" behavior extending to the interrupt request itself.
        if (irqController) irqController->setLine(1, false);

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
            // VRAM-SATB transfer trigger — confirmed §2.1.3(21) NOTE (b)
            // and independently by PCE_Hardware_Documentation.htm: real
            // hardware defers this to the *next vertical sync* rather
            // than firing immediately, and auto-repeats every vsync if
            // DCR bit4 (DSR) is set. This still fires immediately since
            // SATB storage itself isn't implemented yet (see file
            // header) — the timing gap doesn't matter until real sprite
            // data actually needs to land at the right moment.
            statusSatbEnd = true;
            if ((regs[REG_DCR] & 0x01) != 0 && irqController) {   // DSC bit
                irqController->setLine(1, true);
            }
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

    // CONFIRMED: DCR bit1 (DVC) gates whether transfer-complete should
    // interrupt — same bit both official manual and PCE_Hardware_Doc
    // agree on. Fires IRQ1 per the Tech Notes' irq1_handle convention.
    if ((regs[REG_DCR] & 0x02) != 0 && irqController) {
        irqController->setLine(1, true);
    }
}

size_t VDC::dumpState(char* buf, size_t buf_size) const {
    return static_cast<size_t>(std::snprintf(buf, buf_size,
        "AR=%02X MAWR=%04X MARR=%04X CR=%04X SOUR=%04X DESR=%04X LENR=%04X\n",
        ar, regs[REG_MAWR], regs[REG_MARR], regs[REG_CR],
        regs[REG_SOUR], regs[REG_DESR], regs[REG_LENR]));
}
