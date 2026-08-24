/*
 * pce_core.h — internal component interfaces for ding_pce_core
 *
 * This is NOT the public contract (that's sdk/ding_core.h, implemented by
 * ding_core_pce.cpp). This header wires together the HuC6280 CPU, VDC, VCE,
 * and PSG into one PCEngine system object.
 *
 * Stock cartridge PCE/TG16 only — no CD-ROM² handling. PC Engine CD will be
 * a separate core built on top of this one later.
 */

#ifndef PCE_CORE_H
#define PCE_CORE_H

#include "ding_types.h"
#include <vector>
#include <cstdint>
#include <cstddef>

// ── Cartridge ────────────────────────────────────────────────────────────
// Owns raw ROM bytes + HuCard mapping info (most PCE carts are a flat
// 21-bit address space, no bank-switching chips like MMC on NES — a few
// oddball titles use simple mirroring which we handle here later).
class Cartridge {
public:
    bool load(const uint8_t* data, size_t len);
    uint8_t read(u32 addr) const;

    const uint8_t* romData() const { return rom.data(); }
    size_t romSize() const { return rom.size(); }

private:
    std::vector<uint8_t> rom;
};

// ── Bus ──────────────────────────────────────────────────────────────────
// Central memory bus: routes CPU reads/writes across ROM, 8KB work RAM,
// and MMIO windows (VDC/VCE/PSG/timer/IRQ/joypad ports) via the HuC6280's
// MPR (memory mapping register) banking — 8 windows of 8KB each mapping
// into a 21-bit address space.
class Bus {
public:
    void connect(Cartridge* cart);

    uint8_t read(u32 addr);
    void write(u32 addr, uint8_t val);

    void reset();

private:
    Cartridge* cartridge = nullptr;
    uint8_t wram[0x2000] = {};   // 8KB internal work RAM
    uint8_t mpr[8] = {};         // memory mapping registers (bank select)
};

// ── HuC6280 ──────────────────────────────────────────────────────────────
// CPU core. 65C02-derived: adds block transfer ops (TAI/TIA/TII/TIN/TDD),
// extra zero-page addressing modes, a built-in programmable timer, and the
// interrupt controller (IRQ1/IRQ2/TIQ priority + mask register).
class HuC6280 {
public:
    void connect(Bus* bus);
    void reset();
    void step();   // execute one instruction
    void runFrame();

    // diagnostics
    size_t dumpState(char* buf, size_t buf_size) const;

private:
    Bus* bus = nullptr;

    u8  a = 0, x = 0, y = 0, s = 0xFF;
    u16 pc = 0;
    u8  p = 0;      // status flags
    u8  speed = 0;  // HuC6280 has a CPU speed register (1.79MHz / 7.16MHz)
};

// ── VDC ──────────────────────────────────────────────────────────────────
// Video Display Controller (HuC6270). Handles BG plane, sprites, VRAM
// (separate 64KB address space from CPU-visible memory), and generates
// the raw pixel stream that VCE turns into color output.
class VDC {
public:
    void connect(Bus* bus);
    void reset();
    void runLine();   // render one scanline's worth of state

    size_t dumpState(char* buf, size_t buf_size) const;

private:
    Bus* bus = nullptr;
    uint8_t vram[0x10000] = {};  // 64KB VRAM, VDC-local
};

// ── VCE ──────────────────────────────────────────────────────────────────
// Video Color Encoder (HuC6260). Palette RAM (512 entries, 9-bit color) →
// RGB output. Sits between VDC and the framebuffer.
class VCE {
public:
    void reset();
    void writeFramebuffer(uint8_t* fb, uint32_t width, uint32_t height);

private:
    uint16_t palette[512] = {};  // 9-bit color values
};

// ── PSG ──────────────────────────────────────────────────────────────────
// Programmable Sound Generator built into the HuC6280 — 6 wavetable
// channels, 2 of which support noise/LFO modes.
class PSG {
public:
    void reset();
    void runFrame();
    uint32_t readSamples(float* buf, uint32_t count);

private:
    uint32_t sampleCount = 0;
};

// ── PCEngine ─────────────────────────────────────────────────────────────
// Top-level system object. Owns and wires together every component.
// This is what ding_core_pce.cpp drives from the ding_core.h entry points.
class PCEngine {
public:
    void init();
    void destroy();
    void reset();
    void runFrame();

    bool loadRom(const uint8_t* data, size_t len);

    Cartridge cartridge;
    Bus       bus;
    HuC6280   cpu;
    VDC       vdc;
    VCE       vce;
    PSG       psg;
};

#endif // PCE_CORE_H
