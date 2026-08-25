/*
 * PSG.cpp — HuC6280 Programmable Sound Generator register layer
 *
 * CONFIRMED via HuC6280 CMOS Programmable Sound Generator Manual
 * (project file), cross-checked against PCE_PSG_Hardware_Documentation.htm
 * (Paul Clifford, project file) — the two agree on every register offset,
 * the full DDA mode table, and (once unit-converted) the frequency
 * formulas. See pce_core.h's PSG class comment for the confirmed
 * formulas and one unresolved conflict (LF CTL shift amounts) flagged
 * for whenever synthesis actually gets implemented.
 *
 * CONFIRMED via HuC6280 CMOS Programmable Sound Generator Manual
 * (project file):
 *   - §2.1.1: R0 selects the active channel (0-5 = ch1-ch6); R2-R7
 *     accesses then target that channel's banked copy of the register.
 *     R0/R1/R8/R9 are singletons unaffected by channel select.
 *   - §2.1.5(1): R4's chON/DDA bits select one of four modes:
 *       (0,0) write mode      — R6 writes store sequentially, address
 *                                auto-increments after each write
 *       (0,1) reset-counter   — address counter resets to 0 immediately
 *       (1,0) mixing mode     — normal playback; address advances via
 *                                the frequency register during synthesis
 *                                (not via CPU writes to R6)
 *       (1,1) direct D/A mode — address counter also resets; each R6
 *                                write instead latches straight to the
 *                                D/A output, waveData[] left unchanged
 *   - §2.1.7 "Specifying the start address": setting DDA=1 then DDA=0
 *     is the documented way to zero the write address before a bulk
 *     wave-data upload — matches resetting waveAddr on any DDA=1 write.
 *
 * NOT implemented this pass: actual audio synthesis. See pce_core.h's
 * class comment for the full list (waveform playback, LFO modulation,
 * noise, final mixing). Register state is real; sample generation isn't.
 *
 * Register read-back: the manual never documents a read path for any
 * PSG register (unlike VDC/VCE, which explicitly define VRR/CTR read
 * registers) — treated as write-only, reads return open bus (0xFF).
 */

#include "pce_core.h"
#include <cstring>

void PSG::reset() {
    for (auto& ch : channels) {
        ch = Channel{};
    }
    channelSelect = 0;
    mainAmpLeft = mainAmpRight = 0;
    lfoFreq = 0;
    lfTrg = false;
    lfCtl = 0;
}

void PSG::runFrame() {
    // TODO: real sample synthesis — see pce_core.h class comment.
}

uint32_t PSG::readSamples(float* buf, uint32_t count) {
    if (!buf) return 0;
    std::memset(buf, 0, static_cast<size_t>(count) * sizeof(float));
    return 0;
}

uint8_t PSG::readRegister(u16 offset) {
    (void)offset;
    return 0xFF;   // write-only per the manual — see file header note
}

void PSG::writeRegister(u16 offset, uint8_t val) {
    u8 reg = static_cast<u8>(offset & 0x0F);

    // R0/R1/R8/R9 are singletons, not affected by channel select.
    switch (reg) {
        case 0x0:
            channelSelect = val & 0x07;   // values 6-7 unused per the manual's table
            return;
        case 0x1:
            mainAmpLeft  = static_cast<u8>((val >> 4) & 0x0F);
            mainAmpRight = static_cast<u8>(val & 0x0F);
            return;
        case 0x8:
            lfoFreq = val;
            return;
        case 0x9:
            lfTrg = (val & 0x80) != 0;
            lfCtl = val & 0x03;
            return;
        default:
            break;
    }

    // Everything else banks against the selected channel.
    if (channelSelect >= kChannelCount) return;   // 6/7 reserved, no-op
    Channel& ch = channels[channelSelect];

    switch (reg) {
        case 0x2:   // FRQ LOW
            ch.freq = static_cast<u16>((ch.freq & 0x0F00) | val);
            break;
        case 0x3:   // FRQ HIGH (low 4 bits only)
            ch.freq = static_cast<u16>((ch.freq & 0x00FF) | (static_cast<u16>(val & 0x0F) << 8));
            break;
        case 0x4:   // ch ON, DDA, AL
            ch.chOn = (val & 0x80) != 0;
            ch.dda  = (val & 0x40) != 0;
            ch.al   = val & 0x1F;
            onR4Written(ch);
            break;
        case 0x5:   // LAL, RAL
            ch.lal = static_cast<u8>((val >> 4) & 0x0F);
            ch.ral = static_cast<u8>(val & 0x0F);
            break;
        case 0x6:   // WAVE DATA
            onR6Written(ch, val);
            break;
        case 0x7:   // NE, NOISE FRQ — channels 5/6 only (index 4/5)
            if (channelSelect == 4 || channelSelect == 5) {
                ch.noiseEnable = (val & 0x80) != 0;
                ch.noiseFreq = val & 0x1F;
            }
            break;
        default:
            break;
    }
}

void PSG::onR4Written(Channel& ch) {
    // Confirmed §2.1.5(1): both (0,1) reset-counter mode and (1,1)
    // direct-D/A mode reset the waveform address counter to 0 — the
    // table describes both as "address counter is reset."
    if (ch.dda) {
        ch.waveAddr = 0;
    }
}

void PSG::onR6Written(Channel& ch, u8 val) {
    if (!ch.chOn && !ch.dda) {
        // Write mode: store sequentially, auto-increment after write —
        // confirmed §2.1.5(1) table row (0,0).
        ch.waveData[ch.waveAddr & 0x1F] = val & 0x1F;
        ch.waveAddr = static_cast<u8>((ch.waveAddr + 1) & 0x1F);
    } else if (ch.chOn && ch.dda) {
        // Direct D/A mode: latch straight to output, waveData untouched —
        // confirmed §2.1.5(1) table row (1,1) and §2.1.7(3).
        ch.ddaLatch = val & 0x1F;
    }
    // (0,1) reset-counter and (1,0) mixing mode: R6 writes aren't
    // meaningful in these modes per the manual — no-op.
}
