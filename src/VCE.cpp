/*
 * VCE.cpp — HuC6260 register access + color table (palette) RAM
 *
 * CONFIRMED via HuC6260 Video Color Encoder Manual (project file),
 * independently re-confirmed via PCE_VCE_Hardware_Documentation.htm
 * (project file — matches exactly: $0400=CR, $0402=CTA, $0404=color
 * data, same G:R:B bit layout, same auto-increment-on-high-byte-write
 * behavior). No discrepancies found between the two sources.
 *   - §2.1.1: register decode is A2 selects CR/CTA group (A2=0) vs
 *     CTW/CTR group (A2=1); within A2=0, A1 selects CR(A1=0)/CTA(A1=1).
 *     CTW/CTR share address bits, direction-selected like VDC's VWR/VRR.
 *   - §2.1.2(2): CTA auto-increments when CTW's *high* byte is written
 *     (not when CTA itself is written) — the write commits the 9-bit
 *     color and advances to the next entry.
 *   - §2.2.2(3)(b): reading CTR's high byte also auto-increments CTA.
 *   - §2.2.1 Fig 2-2-1: color entry bit layout is G(3 bits):R(3):B(3),
 *     with G's top bit spilling into the "high byte" position (bit 8
 *     overall) while G's other 2 bits, all of R, and all of B live in
 *     the low byte.
 *
 * NOT implemented this pass:
 *   - CR's DCC field (clock divider select) is stored but has no actual
 *     effect — it governs analog output timing details (fCLOCK/fCK
 *     ratios) that don't matter for a digital framebuffer target.
 *   - resolveFramebuffer() now does real palette lookup + 3-to-8-bit
 *     channel expansion, called with VDC's actual video codes as of the
 *     background-rendering pass (see VDC.cpp). writeFramebuffer() is
 *     kept only as a black-screen fallback for before any rendering
 *     has happened.
 */

#include "pce_core.h"
#include <cstring>

void VCE::reset() {
    std::memset(colorTable, 0, sizeof(colorTable));
    cr = 0;
    cta = 0;
    ctwLowLatch = 0;
}

void VCE::writeFramebuffer(uint8_t* fb, uint32_t width, uint32_t height) {
    // Fallback path for when there's no VDC frame to resolve yet (e.g.
    // no ROM loaded). Real output goes through resolveFramebuffer() now.
    if (!fb) return;
    std::memset(fb, 0, static_cast<size_t>(width) * height * 4);
}

void VCE::resolveFramebuffer(const u16* videoCodes, uint8_t* fb, int width, int height) {
    if (!fb || !videoCodes) return;

    for (int i = 0; i < width * height; i++) {
        u16 entry = resolveColor(videoCodes[i]);
        u8 g3 = static_cast<u8>((entry >> 6) & 0x07);
        u8 r3 = static_cast<u8>((entry >> 3) & 0x07);
        u8 b3 = static_cast<u8>(entry & 0x07);

        // 3-bit -> 8-bit channel expansion via bit replication, standard
        // technique for even spread across 0-255 (not manual-specified —
        // the HuC6260 outputs analog RGB directly, so this 8-bit mapping
        // is our own choice for a digital framebuffer target, not a
        // hardware-confirmed value).
        auto expand3to8 = [](u8 v) -> u8 {
            return static_cast<u8>((v << 5) | (v << 2) | (v >> 1));
        };

        u8* px = &fb[i * 4];
        px[0] = expand3to8(r3);
        px[1] = expand3to8(g3);
        px[2] = expand3to8(b3);
        px[3] = 0xFF;
    }
}

uint8_t VCE::readRegister(u16 offset) {
    bool ctwCtrGroup = (offset & 0x04) != 0;   // A2
    bool highByte = (offset & 0x01) != 0;       // A0

    if (!ctwCtrGroup) {
        bool isCta = (offset & 0x02) != 0;      // A1
        if (isCta) {
            return highByte ? static_cast<uint8_t>((cta >> 8) & 0x01)
                             : static_cast<uint8_t>(cta & 0xFF);
        }
        // CR is write-only per the register list (only a W entry exists) —
        // reading it isn't documented; open bus.
        return 0xFF;
    }

    // CTR (read path of the CTW/CTR pair)
    u16 entry = colorTable[cta & (kColorTableSize - 1)];
    if (!highByte) {
        // Low byte: G[1:0] in bits 7-6, R in bits 5-3, B in bits 2-0.
        u8 g_low2 = static_cast<u8>((entry >> 6) & 0x03);
        u8 r = static_cast<u8>((entry >> 3) & 0x07);
        u8 b = static_cast<u8>(entry & 0x07);
        return static_cast<uint8_t>((g_low2 << 6) | (r << 3) | b);
    } else {
        // High byte: only bit 0 meaningful — G's MSB (bit 8 of the entry).
        u8 g_high1 = static_cast<u8>((entry >> 8) & 0x01);
        // Reading the high byte auto-increments CTA — confirmed §2.2.2(3)(b).
        cta = static_cast<u16>((cta + 1) & 0x1FF);
        return g_high1;
    }
}

void VCE::writeRegister(u16 offset, uint8_t val) {
    bool ctwCtrGroup = (offset & 0x04) != 0;   // A2
    bool highByte = (offset & 0x01) != 0;       // A0

    if (!ctwCtrGroup) {
        bool isCta = (offset & 0x02) != 0;      // A1
        if (isCta) {
            if (!highByte) {
                cta = static_cast<u16>((cta & 0x0100) | val);
            } else {
                cta = static_cast<u16>((cta & 0x00FF) | (static_cast<u16>(val & 0x01) << 8));
            }
        } else {
            // CR — only low byte meaningful (DCC field, bits 0-1).
            if (!highByte) cr = val & 0x03;
        }
        return;
    }

    // CTW (write path)
    if (!highByte) {
        ctwLowLatch = val;
    } else {
        commitColorWrite(val);
    }
}

void VCE::commitColorWrite(u8 highByte) {
    u8 g_low2 = static_cast<u8>((ctwLowLatch >> 6) & 0x03);
    u8 r      = static_cast<u8>((ctwLowLatch >> 3) & 0x07);
    u8 b      = static_cast<u8>(ctwLowLatch & 0x07);
    u8 g_high1 = highByte & 0x01;

    u16 entry = static_cast<u16>((g_high1 << 8) | (g_low2 << 6) | (r << 3) | b);
    colorTable[cta & (kColorTableSize - 1)] = entry;

    // Confirmed §2.1.2(2): CTA auto-increments after the high byte of
    // color data is transferred.
    cta = static_cast<u16>((cta + 1) & 0x1FF);
}
