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

// ── Timer ────────────────────────────────────────────────────────────────
// Built into the HuC6280. One 7-bit down-counter clocked at a fixed
// divider off the CPU clock; reload value and start/stop are the only two
// registers. On underflow it reloads and requests an IRQ (routed through
// IrqController). Register layout below is best-effort pending doc
// cross-check — flagging rather than asserting confidence:
//   $0C00 write: reload value (7-bit)   read: live counter value (7-bit)
//   $0C01 write: bit0 = start(1)/stop(0)
class Timer {
public:
    void reset();
    // advance by `cpuCycles` CPU cycles; returns true the instant it
    // underflows (caller wires that into IrqController)
    bool tick(u32 cpuCycles);

    uint8_t readRegister(u16 offset);
    void writeRegister(u16 offset, uint8_t val);

private:
    uint8_t reloadValue = 0;
    uint8_t counter = 0;
    bool running = false;
    u32 cycleAccum = 0;
    static constexpr u32 kDivider = 1024;   // TODO: verify against docs
};

// ── Joypad ───────────────────────────────────────────────────────────────
// Standard 2-button PCE pad, read through a single latched register.
//   $1000 write: bit1 = SEL (selects which 4-bit nibble is read next),
//                bit0 = CLR (resets multitap device counter)
//        read:  low nibble = button state for the currently selected half
// Multitap (up to 5 pads) not modeled yet — single pad on port 0 only.
class Joypad {
public:
    void reset();
    void setButton(uint8_t index, bool pressed);   // index matches DingInputDescriptor

    uint8_t readRegister(u16 offset);
    void writeRegister(u16 offset, uint8_t val);

private:
    uint16_t buttonState = 0xFFFF;   // active-low, all released
    bool selectHigh = false;
};

// ── IrqController ────────────────────────────────────────────────────────
// Interrupt disable + request registers. Priority order TIQ > IRQ1 > IRQ2
// per HuC6280 convention (matches HuC6280::handleIrqIfPending's vector
// selection, though those vector addresses themselves are still flagged
// unverified there).
//   $1402: interrupt disable — bit0=IRQ2 bit1=IRQ1 bit2=TIQ (1 = masked)
//   $1403: interrupt request — read: pending flags; write: acks timer IRQ
class IrqController {
public:
 void reset();
    void setLine(int which, bool asserted);   // 0=IRQ2 1=IRQ1 2=TIQ
    bool pending(int which) const;

    // diagnostics
    u32 getAssertCount(int which) const { return (which >= 0 && which < 3) ? assertCount[which] : 0; }
    u32 getServiceCount(int which) const { return (which >= 0 && which < 3) ? serviceCount[which] : 0; }
    void noteServiced(int which) { if (which >= 0 && which < 3) serviceCount[which]++; }

    uint8_t readRegister(u16 offset);
    void writeRegister(u16 offset, uint8_t val);

private:
 uint8_t disableMask = 0;
    bool lines[3] = {};   // raw asserted state per line, pre-mask

    // ── debug counters (diagnostics only, not part of real hardware) ──
    u32 assertCount[3] = {};   // times setLine(n, true) was called
    u32 serviceCount[3] = {};  // times pending(n) caused actual IRQ service
};

class VDC;
class VCE;
class PSG;

// ── Bus ──────────────────────────────────────────────────────────────────
// Central memory bus. The HuC6280 sees a 16-bit address space (64KB) split
// into 8 pages of 8KB each — the top 3 bits of the address (bits 13-15)
// select an MPR (Memory Mapping Register), and each MPR holds an 8-bit
// bank number that selects an 8KB window into the real 21-bit (2MB)
// physical address space. CPU sets MPRs via TAM/TMA (not yet implemented
// on the CPU side — Bus exposes readMPR/writeMPR for when that lands).
//
// Physical bank map (standard PCE, no CD/System Card):
//   $00-$7F  ROM (HuCard) — mirrored/wrapped if the cart is smaller than
//            the full 1MB (0x80 banks) this range implies
//   $F8      Work RAM (8KB) — only bank that's actually backed by real RAM
//   $F7      Battery-backed RAM on carts that have it (not yet modeled)
//   $FF      Hardware page — VDC/VCE/PSG/timer/joypad/IRQ registers
//   anything else: open bus, reads as 0xFF for now
class Bus {
public:
    void connect(Cartridge* cart);
    void connect(VDC* vdc);
    void connect(VCE* vce);
    void connect(PSG* psg);
    void connect(Timer* timer);
    void connect(Joypad* joypad);
    void connect(IrqController* irq);

 uint8_t read(u32 addr);
    // debugPC is forwarded to VDC's diagnostic register log only — no
    // functional effect. Defaults to 0 for any caller that doesn't have
    // a meaningful PC (e.g. non-CPU-initiated writes, if any ever exist).
    void write(u32 addr, uint8_t val, u16 debugPC = 0);

    void reset();

    // Advance timer by cpuCycles and route an underflow into the IRQ
    // controller as a TIQ. Called from HuC6280::runFrame() per step.
    void tickTimer(u32 cpuCycles);

 // MPR access — CPU's TAM/TMA opcodes will call these once implemented.
    // index is 0-7 (one per 8KB page of CPU address space).
    uint8_t readMPR(uint8_t index) const;
    void writeMPR(uint8_t index, uint8_t bank, u16 debugPC = 0);

    // ── MPR write log (diagnostics only) ──
    static constexpr size_t kMprLogSize = 32;
    struct MprLogEntry { u16 pc; u8 index; u8 bank; };
    size_t getMprLog(MprLogEntry* out, size_t maxEntries) const;

    // ── general write watchpoints (diagnostics only) ──────────────────
    // Watch a small set of CPU addresses; every write to a watched
    // address gets logged with the PC that made it. General-purpose
    // replacement for one-off hunts like "who's writing zero-page $04".
    static constexpr size_t kMaxWatchpoints = 8;
    static constexpr size_t kWatchLogSize = 64;
    struct WatchEntry { u16 pc; u16 addr; u8 val; };

    void addWatchpoint(u16 addr);       // no-op if already watched or table full
    void clearWatchpoints();
    size_t getWatchLog(WatchEntry* out, size_t maxEntries) const;

private:
    Cartridge*     cartridge = nullptr;
    VDC*           vdc = nullptr;
    VCE*           vce = nullptr;
    PSG*           psg = nullptr;
    Timer*         timer = nullptr;
    Joypad*        joypad = nullptr;
    IrqController* irqController = nullptr;

    uint8_t wram[0x2000] = {};   // 8KB internal work RAM (physical bank $F8)
    uint8_t mpr[8] = {};         // memory mapping registers (bank select)

    // ── watchpoint state ──
    u16 watchAddrs[kMaxWatchpoints] = {};
    size_t watchCount = 0;
    WatchEntry watchLog[kWatchLogSize] = {};
    size_t watchLogHead = 0;
    size_t watchLogCount = 0;
void checkWatchpoint(u16 addr, u8 val, u16 debugPC);

    // ── MPR write log state ──
    MprLogEntry mprLog[kMprLogSize] = {};
    size_t mprLogHead = 0;
    size_t mprLogCount = 0;

    // Physical bank + 13-bit offset a CPU address resolves to under the
    // current MPR mapping.
    struct PhysAddr { uint8_t bank; u16 offset; };
    PhysAddr resolve(u32 cpuAddr) const;

    uint8_t readHardwarePage(u16 offset);
    void writeHardwarePage(u16 offset, uint8_t val, u16 debugPC);
};

// ── HuC6280 ──────────────────────────────────────────────────────────────
// CPU core. 65C02-derived: adds block transfer ops (TAI/TIA/TII/TIN/TDD),
// extra zero-page addressing modes, a built-in programmable timer, and the
// interrupt controller (IRQ1/IRQ2/TIQ priority + mask register).
class HuC6280 {
public:
    // Status flag bits (standard 6502 layout)
    enum Flag : u8 {
        FLAG_C = 0x01,  // carry
        FLAG_Z = 0x02,  // zero
        FLAG_I = 0x04,  // IRQ disable
        FLAG_D = 0x08,  // decimal mode
        FLAG_B = 0x10,  // break
        FLAG_T = 0x20,  // unused on NMOS 6502; HuC6280 reserves as 1
        FLAG_V = 0x40,  // overflow
        FLAG_N = 0x80,  // negative
    };

// ── instruction trace ring buffer (diagnostics only) ──
static constexpr size_t kTraceSize = 256;
    static constexpr size_t kBootTraceSize = 4096;   // bumped from 512 — needed to see past early init loops
    struct TraceEntry { u16 pc; u8 opcode; };

void connect(Bus* bus);
    void connect(IrqController* irq);
    void reset();
    void step();   // execute one instruction, returns via cycles accumulator
    void runFrame();
    void runFor(u64 targetCycles);   // run until `cycles` has advanced by targetCycles

    void nmi();

 // diagnostics
    size_t dumpState(char* buf, size_t buf_size) const;
    u32 getCliCount() const { return cliCount; }

// Returns entries oldest-to-newest into out[], up to maxEntries.
    // Returns how many were written.
    size_t getTrace(TraceEntry* out, size_t maxEntries) const;
    size_t getBootTrace(TraceEntry* out, size_t maxEntries) const;

private:
    Bus* bus = nullptr;
    IrqController* irqController = nullptr;

    u8  a = 0, x = 0, y = 0, s = 0xFF;
    u16 pc = 0;
    u8  p = FLAG_T | FLAG_I;   // status flags — IRQ disabled + reserved bit set on reset
    u8  speed = 0;             // CPU speed register: 0 = 1.79MHz, 1 = 7.16MHz

 u64 cycles = 0;

// ── debug counters (diagnostics only, not part of real hardware) ──
    u32 cliCount = 0;   // times CLI (0x58) has executed

 // ── instruction trace ring buffer (diagnostics only) ──
    TraceEntry trace[kTraceSize] = {};
    size_t traceHead = 0;   // next write index
    size_t traceCount = 0;  // entries filled so far, caps at kTraceSize

// ── boot trace: first N instructions after reset, non-overwriting ──
    // Unlike the rolling ring buffer above, this captures the actual boot
    // path once and stops — useful when a wait loop runs for thousands of
    // instructions and would otherwise drown out the lead-up in `trace`.
    TraceEntry bootTrace[kBootTraceSize] = {};
    size_t bootTraceCount = 0;   // stays put once it hits kBootTraceSize

    // ── memory helpers ──
    u8  fetch8();
    u16 fetch16();
    u8  read8(u16 addr);
    void write8(u16 addr, u8 val);
    void push8(u8 val);
    u8  pop8();
    void push16(u16 val);
    u16 pop16();

    // ── flag helpers ──
    void setZN(u8 val);
    void setFlag(Flag f, bool on);
    bool getFlag(Flag f) const { return (p & f) != 0; }

    // ── addressing modes ──
    // Each returns the effective address; accumulator/implied modes are
    // handled directly in the opcode body since they need no address.
    u16 addrZeroPage();
    u16 addrZeroPageX();
    u16 addrZeroPageY();
    u16 addrAbsolute();
    u16 addrAbsoluteX();
    u16 addrAbsoluteY();
    u16 addrIndirect();
    u16 addrIndexedIndirect();   // (zp,X)
    u16 addrIndirectIndexed();   // (zp),Y
    u16 addrZeroPageIndirect();  // (zp) — HuC6280 addition, no 6502 equivalent
    s8  addrRelative();

    // ── dispatch ──
    void execute(u8 opcode);
    void handleIrqIfPending();

    // HuC6280-specific block transfer instructions (TAI/TIA/TII/TIN/TDD).
    // All six take a 6-byte operand (src lo/hi, dst lo/hi, len lo/hi) and,
    // in this implementation, execute the whole transfer atomically rather
    // than being interruptible mid-copy like real hardware — fine for
    // correctness, will matter if a game relies on IRQs firing mid-block.
    enum class BlockMode { IncInc, DecDec, IncFixed, AltInc, IncAlt };
    void blockTransfer(BlockMode mode);

    // TODO(next step): TST immediate/zp/abs variants — opcode encodings
    // need cross-checking against a real HuC6280 reference before wiring,
    // didn't want to guess and risk silently wrong flag behavior.
};

// ── VDC ──────────────────────────────────────────────────────────────────
// Video Display Controller (HuC6270). Register access protocol and full
// register set CONFIRMED via HuC6270 Video Display Controller Manual
// (project file):
//   - AR (address register) / SR (status register) live at A1=0 (offset
//     bit1==0): AR is write-only (selects R00-R13), SR is read-only.
//   - All other registers (R00-R13) live at A1=1, split into low byte
//     (A0=0) and high byte (A0=1). Writing the high byte commits the
//     16-bit value and triggers that register's side effect (VRAM
//     read/write, block transfer start, etc).
//   - VRAM is word-addressable (16-bit words), up to 64K words per the
//     block transfer length register's stated max — allocated in full
//     here rather than guessing a smaller real hardware size, since a
//     too-small buffer risks silent wraparound bugs.
class VDC {
public:
    void connect(Bus* bus);
    void connect(IrqController* irq);
    void reset();
    void runLine();   // render one scanline's worth of state

 // CPU-facing register access — see class comment for A0/A1 protocol.
    // writeRegister takes the CPU's current PC purely for diagnostics
    // (regLog attribution) — no functional effect on VDC behavior.
    uint8_t readRegister(u16 offset);
    void writeRegister(u16 offset, uint8_t val, u16 debugPC = 0);

    size_t dumpState(char* buf, size_t buf_size) const;

private:
    Bus* bus = nullptr;
    IrqController* irqController = nullptr;

    static constexpr size_t kVramWords = 0x10000;
    u16 vram[kVramWords] = {};

    // Internal register file. Indices match the R00-R13 numbering from
    // the manual's register list (R03/R04 reserved, unused).
    enum RegIndex {
        REG_MAWR = 0x00, REG_MARR = 0x01, REG_VWR_VRR = 0x02,
        REG_CR   = 0x05, REG_RCR  = 0x06, REG_BXR = 0x07, REG_BYR = 0x08,
        REG_MWR  = 0x09, REG_HSR  = 0x0A, REG_HDR = 0x0B, REG_VPR = 0x0C,
        REG_VDR  = 0x0D, REG_VCR  = 0x0E, REG_DCR = 0x0F, REG_SOUR = 0x10,
        REG_DESR = 0x11, REG_LENR = 0x12, REG_DVSSR = 0x13,
    };
    u16 regs[0x14] = {};
    u16 vrrValue = 0;   // separate from regs[REG_VWR_VRR] since VWR (write)
                         // and VRR (read) share a register number but are
                         // logically distinct per the manual

    u8 ar = 0;   // address register — selects which of regs[]/vrrValue
                 // the next data-area access targets

    // Status register bits (SR) — CONFIRMED bit layout: bit0=CR(collision),
    // bit1=OR(over), bit2=RR(scanline match), bit3=DS(SATB transfer end),
    // bit4=DV(VRAM-VRAM transfer end), bit5=VD(vblank), bit6=BSY.
    // Cleared on read except BSY, per the manual.
    bool statusCollision = false;
    bool statusOver = false;
    bool statusScanlineMatch = false;
    bool statusSatbEnd = false;
    bool statusVramEnd = false;
    bool statusVblank = false;
    bool statusBusy = false;

void onHighByteWritten(u8 regIndex);
    void incrementAddress(u16& addr);   // applies IW field from CR
    void doVramToVramBlockTransfer();

    // ── register-write log (diagnostics only) ──
    // Records every committed (high-byte-write) register update, so we can
    // answer "did CR ever get written, and to what" without hand-decoding
    // opcode traces.
public:
    static constexpr size_t kRegLogSize = 128;
    struct RegLogEntry { u16 pc; u8 regIndex; u16 value; };
private:
    RegLogEntry regLog[kRegLogSize] = {};
    size_t regLogHead = 0;
    size_t regLogCount = 0;
    void logRegWrite(u16 pc, u8 regIndex, u16 value);

public:
    size_t getRegLog(RegLogEntry* out, size_t maxEntries) const;

private:

    // ── background rendering (this pass) ──
    // Fixed 256x224 visible area (32x28 characters), 32x32-character
    // virtual screen (SCREEN=0 mode only), standard 16-color BG mode.
    // Produces one row of 9-bit VDC video codes (VD0-VD8) per scanline,
    // ready for VCE::resolveColor(). See file header for what's not
    // covered yet (other SCREEN sizes, 4-color mode, sprites).
    static constexpr int kVisibleWidth = 256;
    static constexpr int kVisibleHeight = 224;
    u16 videoCodes[kVisibleWidth * kVisibleHeight] = {};
    int currentLine = 0;

    void renderBackgroundLine(int line);

    // ── sprites (this pass) ──
    // CONFIRMED via VDC Manual §2.4.2/2.4.3: SAT entry is 4 words
    // (Y coord, X coord, pattern code, attribute word). SATB storage
    // is real now — VRAM-SATB block transfer (triggered by DVSSR high
    // byte, per §2.1.3(21) NOTE b) is deferred to the next vblank
    // boundary rather than firing immediately, matching confirmed
    // hardware timing now that runLine() has a real frame-boundary hook.
    //
    // NOT confirmed with confidence: the attribute word's exact bit
    // layout for SPBG/flip/CGX/CGY comes from a diagram that OCR'd too
    // ambiguously to trust. SPRITE COLOR (bits 3-0) and SPBG (bit 7) are
    // implemented at medium-high confidence (consistent with every other
    // "COLOR nibble + flag bit" field in this document family), but
    // flip (X̄/Ȳ) and CGX/CGY sprite-combining are explicitly NOT wired
    // this pass — defaulting to no-flip, no-combine — rather than risk
    // silently-wrong orientation that's hard to notice when testing.
    struct Sprite { u16 y = 0, x = 0, pattern = 0, attr = 0; };
    Sprite satb[64];
    bool satbTransferPending = false;

    void renderSpriteLine(int line);
    void doVramToSatbTransfer();

public:
    // Read-only access for VCE/framebuffer assembly once a full frame's
    // worth of lines have run.
    const u16* getVideoCodes() const { return videoCodes; }
    int getVisibleWidth() const { return kVisibleWidth; }
    int getVisibleHeight() const { return kVisibleHeight; }

private:
};

// ── VCE ──────────────────────────────────────────────────────────────────
// Video Color Encoder (HuC6260). Register access protocol and color table
// layout CONFIRMED via HuC6260 Video Color Encoder Manual (project file):
//   - CR/CTA live at A2=0 (A1 selects CR vs CTA); CTW/CTR live at A2=1
//     (direction-based, same address bits, write vs read like VDC's
//     VWR/VRR pair). All registers use the same A0=0(low)/A0=1(high)
//     byte-split convention as the VDC.
//   - Color table is 512 entries x 9 bits (G:3 R:3 B:3). Each entry's
//     low byte holds G[1:0]/R[2:0]/B[2:0], high byte holds only G[2]
//     in bit 0 — the rest of the high byte is unused per the manual's
//     shaded-region diagram.
//   - CTA is a 9-bit address into the color table; writing CTW's high
//     byte commits the entry and auto-increments CTA. Reading CTR's
//     high byte likewise auto-increments CTA (confirmed §2.2.2(3)(b)).
//   - Palette addressing: VD8 selects background(0)/sprite(1) half
//     (256 entries each), VD7-VD4 select a 16-entry color block, VD3-VD0
//     select the pattern color within that block — this is how VDC pixel
//     output will eventually index into this table once VDC rendering
//     exists.
class VCE {
public:
    void reset();
    void writeFramebuffer(uint8_t* fb, uint32_t width, uint32_t height);

    // Resolves a full VDC frame of video codes into an RGBA8 framebuffer.
    // This is the real path now that VDC's background renderer produces
    // actual VD0-VD8 codes; writeFramebuffer() above is kept as a
    // fallback for whenever VDC hasn't run yet (e.g. before any ROM is
    // loaded) and still just clears to black.
    void resolveFramebuffer(const u16* videoCodes, uint8_t* fb, int width, int height);

    // CPU-facing register access — see class comment for A0/A1/A2 protocol.
    uint8_t readRegister(u16 offset);
    void writeRegister(u16 offset, uint8_t val);

    // Resolves a 9-bit VDC video code (VD0-VD8, see manual §2.2.2(2)) to
    // a 9-bit GRB color table entry. Used once VDC rendering exists.
    u16 resolveColor(u16 vd0to8) const { return colorTable[vd0to8 & 0x1FF]; }

private:
    static constexpr size_t kColorTableSize = 512;
    u16 colorTable[kColorTableSize] = {};   // each entry: bits 8-6=G, 5-3=R, 2-0=B

    u16 cr = 0;    // control register — DCC field (bits 0-1), clock divider select
    u16 cta = 0;   // color table address (9 bits meaningful)

    // Latches for the in-progress low/high byte write to CTW, since the
    // 9-bit value only commits on the high-byte write.
    u8 ctwLowLatch = 0;

    void commitColorWrite(u8 highByte);
};

// ── PSG ──────────────────────────────────────────────────────────────────
// Programmable Sound Generator, a sub-block of the HuC6280 (not a
// separate chip — confirmed via HuC62 System Outline Note). 6 channels,
// waveform-memory synthesis (5 bits x 32 words/cycle), channels 5/6 can
// swap to noise generation, channel 1 can be frequency-modulated by
// channel 2 via the built-in LFO.
//
// Register layout and channel-select addressing CONFIRMED via HuC6280
// CMOS Programmable Sound Generator Manual (project file):
//   - R0 (channel select), R1 (main L/R amplitude), R8 (LFO freq), R9
//     (LFO control) are singletons, addressed directly by A0-A3.
//   - R2-R7 are banked per-channel: A0-A3 selects the register *within*
//     whichever channel R0 currently points at. R7 (noise) only exists
//     for channels 5/6 (index 4/5 here).
//   - R4's chON/DDA bits select one of four modes (write/reset-counter/
//     mixing/direct-D-A) that change what writing R6 does — see
//     PSG.cpp's onR4Written()/writeRegister() for the confirmed table.
class PSG {
public:
    void reset();
    void runFrame();
    uint32_t readSamples(float* buf, uint32_t count);

    // CPU-facing register access, offset&0xF selects R0-R9 directly
    // (A0-A3) — see class comment for the channel-banking behavior of
    // R2-R7.
    uint8_t readRegister(u16 offset);
    void writeRegister(u16 offset, uint8_t val);

private:
    static constexpr int kChannelCount = 6;

    struct Channel {
        u16 freq = 0;        // R2/R3 combined, 12 bits
        bool chOn = false;   // R4 bit 7
        bool dda = false;    // R4 bit 6
        u8 al = 0;           // R4 bits 4-0, amplitude level
        u8 lal = 0, ral = 0; // R5: L/R amplitude, 4 bits each
        u8 waveData[32] = {};
        u8 waveAddr = 0;
        u8 ddaLatch = 0;     // last value written to R6 in direct-D/A mode
        bool noiseEnable = false;   // R7 (channels 5/6 only, index 4/5)
        u8 noiseFreq = 0;           // R7 bits 4-0
    };
    Channel channels[kChannelCount];

    u8 channelSelect = 0;   // R0 — 0-5 select ch1-ch6
    u8 mainAmpLeft = 0, mainAmpRight = 0;   // R1
    u8 lfoFreq = 0;          // R8
    bool lfTrg = false;      // R9 bit 7
    u8 lfCtl = 0;            // R9 bits 1-0

    void onR4Written(Channel& ch);
    void onR6Written(Channel& ch, u8 val);

    // TODO(next step): actual sample synthesis. All register state above
    // is real and matches the confirmed hardware behavior, but runFrame()
    // and readSamples() don't yet turn it into audio — that needs a
    // waveform-table sample generator per channel, LFO frequency
    // modulation of channel 1 by channel 2 (per §2.1.10's confirmed
    // addition/shift table), noise generation for channels 5/6, and
    // final L/R mixing through R1/R4/R5's amplitude stages.
    //
    // CONFIRMED frequency formulas, cross-checked between the official
    // manual and PCE_PSG_Hardware_Documentation.htm (Paul Clifford) —
    // both agree once you account for fmaster=7.16MHz vs. the already-
    // halved 3.58MHz the community doc uses as its base:
    //   waveform: freq_hz = 3580000 / (32 * F)          F = 12-bit reg value
    //   noise:    freq_hz = 3580000 / (64 * (NF XOR 31)) NF = 5-bit reg value
    //   LFO:      freq_hz = 3580000 / (32 * F2 * FLF)    F2=ch2's F, FLF=R8
    //
    // UNRESOLVED CONFLICT — LF CTL shift amounts: the official manual's
    // diagram implies 0/2/4-bit left shifts for LFCTL=1/2/3. The
    // community doc gives a worked example (LFCTL=2, wave value %10111
    // = -9 signed, result "(-9 << 4) added") that only holds together
    // with 0/4/8-bit shifts. Leaning toward the community doc here since
    // a concrete worked example is harder to misread than an OCR'd
    // ASCII diagram, but this needs a real hardware/test-ROM check
    // before the LFO modulation math gets implemented — don't guess
    // between them silently when that day comes.
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

    Cartridge     cartridge;
    Bus           bus;
    HuC6280       cpu;
    VDC           vdc;
    VCE           vce;
    PSG           psg;
    Timer         timer;
    Joypad        joypad;
    IrqController irqController;
};

#endif // PCE_CORE_H
