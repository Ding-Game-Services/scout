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
 *   - BUG FOUND AND FIXED (harness testing): statusVblank was never set
 *     anywhere, causing standard vsync-wait boot loops to spin forever.
 *     Now pulses once per frame in runLine() — see the fix there for
 *     the full explanation and its remaining timing imprecision.
 *
 * NOT implemented / not confirmed (see also the sprites section comment
 * further down for confidence caveats on SPBG/flip/CGX/CGY):
 *   - Collision interrupt isn't wired — no collision *detection* exists
 *     yet (would need per-pixel sprite-vs-sprite-0 overlap tracking
 *     during compositing, not just drawing). Scanline-match interrupt
 *     also isn't wired — needs RCR compared against a real scanline
 *     counter, which runLine() doesn't track yet (it just counts visible
 *     lines 0-223, not a real counter starting at 64 per the confirmed
 *     Tech Notes convention).
 *   - 4-color BG mode, SCREEN sizes other than 32x32, and CGX/CGY sprite
 *     combining are all unimplemented — standard 16-color/32x32-BG/
 *     16x16-sprite mode only.
 *   - Sprite flip (X̄/Ȳ) not wired — the attribute word's bit positions
 *     for this weren't confirmable from the manual's OCR'd diagram, so
 *     rather than guess, sprites always render unflipped.
 *   - No 16-sprites-per-scanline cap enforcement.
 *   - VRAM-VRAM transfer and VRAM-SATB transfer are NOT cycle-accurate —
 *     VRAM-VRAM still executes atomically on the LENR high-byte write
 *     (manual says it should span a vblank/burst-mode period); VRAM-SATB
 *     is now correctly deferred to the vblank *boundary* per the manual,
 *     but doesn't model the real per-transfer duration within that
 *     window either.
 *   - BSY (status bit 6) never asserts — VRAM access is instant/atomic
 *     in this implementation, so there's no window where BSY would
 *     meaningfully read as set.
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
    for (auto& sp : satb) sp = Sprite{};
    satbTransferPending = false;
    currentLine = 0;
    std::memset(videoCodes, 0, sizeof(videoCodes));
    std::memset(regLog, 0, sizeof(regLog));
    regLogHead = 0;
    regLogCount = 0;
}

void VDC::runLine() {
    renderBackgroundLine(currentLine);
    renderSpriteLine(currentLine);
    currentLine++;
    if (currentLine >= kVisibleHeight) {
        currentLine = 0;

        // BUG FIX: nothing was ever setting statusVblank, so any ROM
        // doing the standard "wait for VSYNC" boot pattern (poll SR bit
        // 5, confirmed as the real-world convention via the HuC62 Tech
        // Notes' vsync_wait example) would spin forever — this is what
        // was causing PC to freeze at a fixed address across many
        // frames instead of ever progressing past boot.
        //
        // Pulses once per frame at the visible-area boundary rather than
        // tracking the real non-visible scanline count/duration (see
        // PCEngine::runFrame's TODO about CPU/VDC not being interleaved
        // per-scanline yet) — close enough to unblock polling loops, not
        // yet accurate for anything timing-sensitive within vblank.
        statusVblank = true;

        // CR bit3 = VC (vertical blanking period detect enable),
        // confirmed via the Software Manual's IE field table.
        if ((regs[REG_CR] & 0x08) != 0 && irqController) {
            irqController->setLine(1, true);
        }

        // VRAM-SATB transfer, deferred to the vblank boundary — confirmed
        // §2.1.3(21) NOTE (b). DCR bit4 (DSR) makes it auto-repeat every
        // vblank; otherwise it only fires once per DVSSR high-byte write.
        bool autoRepeat = (regs[REG_DCR] & 0x10) != 0;
        if (satbTransferPending || autoRepeat) {
            doVramToSatbTransfer();
            satbTransferPending = false;
        }
    }
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
// NOT implemented this pass: 4-color BG mode, SCREEN sizes other than
// 32x32, and the CG COLOR block correctly offsetting into VCE's
// *background* half of the color table (currently assumed to start at
// color-table index 0 — matches the background half per VCE manual
// §2.2.1, so this one should already be correct).
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

// ── sprites ──────────────────────────────────────────────────────────────

void VDC::doVramToSatbTransfer() {
    u16 src = regs[REG_DVSSR];
    for (int i = 0; i < 64; i++) {
        satb[i].y       = vram[static_cast<u16>(src + i * 4 + 0) & (kVramWords - 1)];
        satb[i].x       = vram[static_cast<u16>(src + i * 4 + 1) & (kVramWords - 1)];
        satb[i].pattern = vram[static_cast<u16>(src + i * 4 + 2) & (kVramWords - 1)];
        satb[i].attr    = vram[static_cast<u16>(src + i * 4 + 3) & (kVramWords - 1)];
    }

    statusSatbEnd = true;
    // DCR bit0 = DSC (SATB transfer complete IRQ enable), confirmed both
    // in the official manual and independently in PCE_Hardware_Documentation.htm.
    if ((regs[REG_DCR] & 0x01) != 0 && irqController) {
        irqController->setLine(1, true);
    }
}

// CONFIRMED tile layout via VDC Manual §2.4.2/2.4.3/2.4.4:
//   - Sprite screen position = SAT (Y,X) minus the documented coordinate
//     origin offset (32,64) — Fig 2-2-1 explicitly labels sprite
//     coordinate (32,64) as screen position (0,0).
//   - Pattern base address = pattern code with its low 6 bits zeroed
//     ("SG0 should align at address X...X000000 binary" — the code's own
//     top 10 bits become the address's top 10 bits directly).
//   - SG0/SG1/SG2/SG3 each occupy 16 consecutive words after that base
//     (64 words total for one 16x16 sprite, standard non-CGX/CGY mode).
//   - Pixel color = SG3:SG2:SG1:SG0 (SG3 is the MSB), per §2.4.4's
//     "VD3-VD0 = SG3-SG0" video-output table — same bit-order convention
//     as the background's CH3:CH2:CH1:CH0.
//   - Sprite priority: "the priority of sprites follows that of
//     addresses" (§2.4.2) — sprite 0 highest. Implemented by iterating
//     63→0 so lower-index sprites are drawn last and end up on top.
//   - SPBG (bg-vs-sprite priority) tested against a per-line snapshot of
//     which pixels the background left transparent, taken before any
//     sprite compositing — see class comment for confidence caveats on
//     SPBG's bit position and the explicitly-unimplemented flip/CGX/CGY.
void VDC::renderSpriteLine(int line) {
    if (!bus) return;

    bool bgTransparent[kVisibleWidth];
    for (int x = 0; x < kVisibleWidth; x++) {
        bgTransparent[x] = (videoCodes[line * kVisibleWidth + x] == 0);
    }

    constexpr int kSpriteHeight = 16;   // standard mode only — no CGY combining this pass
    constexpr int kSpriteWidth = 16;    // standard mode only — no CGX combining this pass

    for (int i = 63; i >= 0; i--) {
        const Sprite& sp = satb[i];

        int screenY = static_cast<int>(sp.y) - 64;
        int screenX = static_cast<int>(sp.x) - 32;
        if (line < screenY || line >= screenY + kSpriteHeight) continue;
        int row = line - screenY;

        u16 patternBase = sp.pattern & 0xFFC0;
        u16 sg0 = vram[static_cast<u16>(patternBase + row) & (kVramWords - 1)];
        u16 sg1 = vram[static_cast<u16>(patternBase + 16 + row) & (kVramWords - 1)];
        u16 sg2 = vram[static_cast<u16>(patternBase + 32 + row) & (kVramWords - 1)];
        u16 sg3 = vram[static_cast<u16>(patternBase + 48 + row) & (kVramWords - 1)];

        u8 spriteColor = static_cast<u8>(sp.attr & 0x0F);
        bool spbg = (sp.attr & 0x80) != 0;

        for (int col = 0; col < kSpriteWidth; col++) {
            int x = screenX + col;
            if (x < 0 || x >= kVisibleWidth) continue;

            int bitPos = 15 - col;   // MSB = leftmost, same convention as background
            u8 pixel = static_cast<u8>(
                ((sg0 >> bitPos) & 1)       |
                (((sg1 >> bitPos) & 1) << 1) |
                (((sg2 >> bitPos) & 1) << 2) |
                (((sg3 >> bitPos) & 1) << 3));
            if (pixel == 0) continue;   // transparent

            if (!spbg && !bgTransparent[x]) continue;   // background wins per SPBG=0

            // Video code per §2.4.4: VD8=1(sprite), VD7-4=SP COLOR, VD3-0=pattern.
            videoCodes[line * kVisibleWidth + x] =
                static_cast<u16>(0x100 | (spriteColor << 4) | pixel);
        }
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

void VDC::writeRegister(u16 offset, uint8_t val, u16 debugPC) {
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
        logRegWrite(debugPC, ar, regs[ar]);
        onHighByteWritten(ar);
    }
}

void VDC::logRegWrite(u16 pc, u8 regIndex, u16 value) {
    regLog[regLogHead] = { pc, regIndex, value };
    regLogHead = (regLogHead + 1) % kRegLogSize;
    if (regLogCount < kRegLogSize) regLogCount++;
}

size_t VDC::getRegLog(RegLogEntry* out, size_t maxEntries) const {
    if (!out || maxEntries == 0) return 0;
    size_t n = regLogCount < maxEntries ? regLogCount : maxEntries;
    size_t start = (regLogCount < kRegLogSize) ? 0 : regLogHead;
    for (size_t i = 0; i < n; i++) {
        out[i] = regLog[(start + i) % kRegLogSize];
    }
    return n;
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
            // hardware defers this to the *next vertical sync*. Now that
            // SATB storage is real (see sprites section, this pass), the
            // transfer actually happens in runLine()'s vblank boundary
            // via doVramToSatbTransfer() — this just arms it.
            satbTransferPending = true;
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
