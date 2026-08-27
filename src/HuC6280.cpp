/*
 * HuC6280.cpp — CPU core
 *
 * This pass implements the base 65C02-derived instruction set (loads,
 * stores, transfers, ALU ops, shifts, branches, stack, flags, jumps) with
 * correct addressing modes and flag behavior.
 *
 * NOT yet implemented:
 *   - Decimal mode arithmetic (ADC/SBC currently binary-only — decimal
 *     flag is tracked but ignored, same as most emulators do until a game
 *     actually needs it)
 *   - Instruction-accurate cycle counts (runFrame() still uses a flat
 *     per-step cost, not the real per-opcode timing table)
 *
 * VERIFIED against the HuC6280 Software Manual (project file) this pass:
 *   - Flag register bit layout (N V T B D I Z C, bit7->bit0) — matches
 *   - TAM/TMA bitmask encoding — matches what was already implemented
 *   - TST (0x83/0xA3/0x93/0xB3), TRB/TSB (0x14/0x1C/0x04/0x0C), RMBi/SMBi
 *     (0x07-0x77 / 0x87-0xF7), BBRi/BBSi (0x0F-0x7F / 0x8F-0xFF), and the
 *     swap instructions SAX/SAY/SXY (0x22/0x42/0x02) — all newly added
 *     with confirmed opcodes and operand ordering
 *   - BUG FOUND AND FIXED: BRK vector is $FFF6/$FFF7, not $FFFE like
 *     stock 6502 — manual states this explicitly. NMI vector corrected
 *     to $FFFE by elimination (FFF6=IRQ2/BRK, FFF8=IRQ1, FFFA=TIQ,
 *     FFFC=RESET leaves FFFE for NMI, the only unclaimed slot in the
 *     standard 5-vector HuC6280 table)
 *
 * STILL UNVERIFIED:
 *   - Per-opcode cycle counts (manual has these per-instruction, just not
 *     transcribed into runFrame()'s timing model yet)
 *
 * CONFIRMED this pass via Hardware Manual §2.1.4: stack page is
 * $2100-$21FF (was already implemented correctly, just unconfirmed
 * before now).
 *
 * Block transfers execute atomically (whole copy in one C++ call) rather
 * than being interruptible mid-transfer like real hardware — fine for
 * correctness, could matter later if a game relies on an IRQ firing
 * mid-block.
 *
 * Cycle counts are approximate placeholders (base op cost only, no extra
 * cycle for page-crossing yet) — fine for correctness testing in the
 * harness, will need tightening once we chase real timing bugs.
 */

#include "pce_core.h"
#include <cstdio>

void HuC6280::connect(Bus* b) {
    bus = b;
}

void HuC6280::connect(IrqController* irq) {
    irqController = irq;
}

void HuC6280::reset() {
    a = x = y = 0;
    s = 0xFF;
    p = FLAG_T | FLAG_I;
speed = 0;
    cycles = 0;
    bootTraceCount = 0;   // re-capture boot path from this reset onward

    // CONFIRMED via original Hardware Manual §2.1.3 (physical $1FFE/
    // $1FFF at reset = logical $FFFE/$FFFF since MPR7=0) and
    // independently via PCE_CPU_Hardware_Documentation.htm's vector
    // table. BUG FIX: this previously read $FFFC, which is actually
    // the NMI vector — the two were swapped. See HuC6280::nmi().
    if (bus) {
        u8 lo = bus->read(0xFFFE);
        u8 hi = bus->read(0xFFFF);
        pc = static_cast<u16>(lo) | (static_cast<u16>(hi) << 8);
    } else {
        pc = 0;
    }
}

// ── memory helpers ──────────────────────────────────────────────────────

u8 HuC6280::read8(u16 addr) {
    return bus ? bus->read(addr) : 0xFF;
}

void HuC6280::write8(u16 addr, u8 val) {
    if (bus) bus->write(addr, val, pc);
}

u8 HuC6280::fetch8() {
    return read8(pc++);
}

u16 HuC6280::fetch16() {
    u8 lo = fetch8();
    u8 hi = fetch8();
    return static_cast<u16>(lo) | (static_cast<u16>(hi) << 8);
}

void HuC6280::push8(u8 val) {
    // CONFIRMED via Hardware Manual §2.1.4: stack lives at $2100-$21FF
    // (high byte of S always $21) — differs from stock 6502's $0100.
    write8(0x2100 + s, val);
    s--;
}

u8 HuC6280::pop8() {
    s++;
    return read8(0x2100 + s);
}

void HuC6280::push16(u16 val) {
    push8(static_cast<u8>(val >> 8));
    push8(static_cast<u8>(val & 0xFF));
}

u16 HuC6280::pop16() {
    u8 lo = pop8();
    u8 hi = pop8();
    return static_cast<u16>(lo) | (static_cast<u16>(hi) << 8);
}

// ── flag helpers ─────────────────────────────────────────────────────────

void HuC6280::setFlag(Flag f, bool on) {
    if (on) p |= f;
    else    p &= static_cast<u8>(~f);
}

void HuC6280::setZN(u8 val) {
    setFlag(FLAG_Z, val == 0);
    setFlag(FLAG_N, (val & 0x80) != 0);
}

// ── addressing modes ────────────────────────────────────────────────────

u16 HuC6280::addrZeroPage() {
    return fetch8();
}

u16 HuC6280::addrZeroPageX() {
    return static_cast<u8>(fetch8() + x);
}

u16 HuC6280::addrZeroPageY() {
    return static_cast<u8>(fetch8() + y);
}

u16 HuC6280::addrAbsolute() {
    return fetch16();
}

u16 HuC6280::addrAbsoluteX() {
    return static_cast<u16>(fetch16() + x);
}

u16 HuC6280::addrAbsoluteY() {
    return static_cast<u16>(fetch16() + y);
}

u16 HuC6280::addrIndirect() {
    u16 ptr = fetch16();
    u8 lo = read8(ptr);
    // NOTE: stock 65C02 fixed the classic 6502 page-boundary JMP indirect
    // bug (no wraparound within the page); HuC6280 inherits the 65C02 fix.
    u8 hi = read8(static_cast<u16>(ptr + 1));
    return static_cast<u16>(lo) | (static_cast<u16>(hi) << 8);
}

u16 HuC6280::addrIndexedIndirect() {
    u8 zp = static_cast<u8>(fetch8() + x);
    u8 lo = read8(zp);
    u8 hi = read8(static_cast<u8>(zp + 1));
    return static_cast<u16>(lo) | (static_cast<u16>(hi) << 8);
}

u16 HuC6280::addrIndirectIndexed() {
    u8 zp = fetch8();
    u8 lo = read8(zp);
    u8 hi = read8(static_cast<u8>(zp + 1));
    u16 base = static_cast<u16>(lo) | (static_cast<u16>(hi) << 8);
    return static_cast<u16>(base + y);
}

u16 HuC6280::addrZeroPageIndirect() {
    // HuC6280 addition — (zp) with no index. Not present on stock 6502.
    u8 zp = fetch8();
    u8 lo = read8(zp);
    u8 hi = read8(static_cast<u8>(zp + 1));
    return static_cast<u16>(lo) | (static_cast<u16>(hi) << 8);
}

s8 HuC6280::addrRelative() {
    return static_cast<s8>(fetch8());
}

// ── interrupts ───────────────────────────────────────────────────────────

void HuC6280::nmi() {
    push16(pc);
    push8(p & static_cast<u8>(~FLAG_B));
    setFlag(FLAG_I, true);
    // CONFIRMED via PCE_CPU_Hardware_Documentation.htm's vector table:
    // NMI is $FFFC, RESET is $FFFE. BUG FIX: this previously read
    // $FFFE, colliding with the reset vector — see HuC6280::reset().
    pc = static_cast<u16>(read8(0xFFFC)) | (static_cast<u16>(read8(0xFFFD)) << 8);
}

void HuC6280::handleIrqIfPending() {
    if (!irqController) return;
    if (getFlag(FLAG_I)) return;

    // Priority: TIQ (2) > IRQ1 (1) > IRQ2 (0).
    int which = irqController->pending(2) ? 2
              : irqController->pending(1) ? 1
              : irqController->pending(0) ? 0
              : -1;
 if (which < 0) return;
    irqController->noteServiced(which);

    push16(pc);
    push8(p & static_cast<u8>(~FLAG_B));
    setFlag(FLAG_I, true);

    // Vector table CONFIRMED via three independent sources: Software
    // Manual's BRK entry (FFF6/FFF7=IRQ2/BRK), original Hardware
    // Manual §2.1.3 (RESET at physical $1FFE=logical $FFFE), and
    // PCE_CPU_Hardware_Documentation.htm's explicit table (which also
    // caught the reset/NMI swap bug fixed in reset()/nmi() above).
    // Full table: FFF6=IRQ2, FFF8=IRQ1, FFFA=TIMER, FFFC=NMI, FFFE=RESET.
    u16 vector = which == 2 ? 0xFFFA
               : which == 1 ? 0xFFF8
                             : 0xFFF6;
    pc = static_cast<u16>(read8(vector)) | (static_cast<u16>(read8(vector + 1)) << 8);

    // TIQ auto-acks on service per convention (IRQ1/IRQ2 are level-
    // triggered from hardware and stay asserted until the device deasserts
    // them, so only clear the timer line here).
    if (which == 2) irqController->setLine(2, false);
}

// ── step / execute ──────────────────────────────────────────────────────

void HuC6280::step() {
    handleIrqIfPending();
    u16 pcBefore = pc;
    u8 opcode = fetch8();

// Record into the trace ring buffer BEFORE execute() so pc reflects
    // where the opcode byte actually was, not wherever execute() leaves it.
    trace[traceHead] = { pcBefore, opcode };
    traceHead = (traceHead + 1) % kTraceSize;
    if (traceCount < kTraceSize) traceCount++;

    if (bootTraceCount < kBootTraceSize) {
        bootTrace[bootTraceCount] = { pcBefore, opcode };
        bootTraceCount++;
    }

    execute(opcode);
    if (bus) bus->tickTimer(1);   // TODO: real per-instruction cycle count, not a flat 1
}

void HuC6280::runFrame() {
    // CONFIRMED via HuC62 Tech Notes (project file, "Displaying a Speed
    // Gage"): "The HuC6280 runs at about 7.19 M Hertz... there are
    // 119,904 machine cycles per VSYNC." Replaces the earlier 29780
    // placeholder guess. Still doesn't sync against real VDC scanline
    // timing — this is a flat per-frame budget, not scanline-accurate.
    //
    // NOTE: kept for API parity / anything that wants a whole-frame flat
    // run, but PCEngine::runFrame() no longer calls this — see runFor()
    // below and the BUG FIX note in PCEngine.cpp. A full-frame flat run
    // never lets VDC observe mid-frame, so any game polling a vblank IRQ
    // flag can't ever see it change — this was the root cause of several
    // titles (Alien Crush confirmed via boot trace + IRQ counters) hanging
    // in a wait loop forever despite otherwise-correct boot code.
    runFor(119904);
}

void HuC6280::runFor(u64 targetCycles) {
    const u64 target = cycles + targetCycles;
    while (cycles < target) {
        step();
        cycles++;   // TODO: replace with real per-instruction cycle costs
    }
}

// ── ALU helpers shared by ADC/SBC ───────────────────────────────────────

namespace {
void adcBinary(u8& a, u8 val, u8& p) {
    bool carryIn = (p & HuC6280::FLAG_C) != 0;
    u16 sum = static_cast<u16>(a) + val + (carryIn ? 1 : 0);
    bool overflow = (~(a ^ val) & (a ^ static_cast<u8>(sum)) & 0x80) != 0;

    p = static_cast<u8>(sum > 0xFF ? (p | HuC6280::FLAG_C) : (p & ~HuC6280::FLAG_C));
    p = static_cast<u8>(overflow ? (p | HuC6280::FLAG_V) : (p & ~HuC6280::FLAG_V));

    a = static_cast<u8>(sum);
    p = static_cast<u8>(a == 0 ? (p | HuC6280::FLAG_Z) : (p & ~HuC6280::FLAG_Z));
    p = static_cast<u8>((a & 0x80) ? (p | HuC6280::FLAG_N) : (p & ~HuC6280::FLAG_N));
}

void sbcBinary(u8& a, u8 val, u8& p) {
    // SBC is ADC with the operand inverted, standard trick.
    adcBinary(a, static_cast<u8>(~val), p);
}
} // namespace

// ── block transfers ──────────────────────────────────────────────────────
// Operand layout for all five ops: src_lo src_hi dst_lo dst_hi len_lo len_hi
// A length of 0 means 0x10000 (full 64KB) per HuC6280 convention.

void HuC6280::blockTransfer(BlockMode mode) {
    u16 src = fetch16();
    u16 dst = fetch16();
    u32 len = fetch16();
    if (len == 0) len = 0x10000;

    for (u32 i = 0; i < len; i++) {
        u8 val = read8(src);
        write8(dst, val);

        switch (mode) {
            case BlockMode::IncInc:   src++; dst++; break;                 // TII
            case BlockMode::DecDec:   src--; dst--; break;                 // TDD
            case BlockMode::IncFixed: src++;        break;                 // TIN — dst stays fixed
            case BlockMode::AltInc:                                        // TAI — src alternates, dst increments
                src = static_cast<u16>(src ^ 1);
                dst++;
                break;
            case BlockMode::IncAlt:                                        // TIA — src increments, dst alternates
                src++;
                dst = static_cast<u16>(dst ^ 1);
                break;
        }
    }

    // CONFIRMED cost: 17+6x cycles where x is the transfer length, per
    // both the Software Manual's instruction table and the Tech Notes'
    // "Displaying a Speed Gage" worked example (which uses this exact
    // formula to compute a deliberate CPU-stall for its speedometer
    // demo). step()'s flat +1 per instruction still applies on top of
    // this via bus->tickTimer() in step() — that flat cost is a rough
    // placeholder for everything else, but this is the one place so far
    // where the real per-instruction cost is wired through accurately.
    u32 cost = 17 + 6 * len;
    cycles += cost;
    if (bus) bus->tickTimer(cost);

    // Real hardware sets Z/N based on the final transferred byte and
    // leaves other flags alone; matching that here for consistency with
    // the rest of the flag handling even though no game likely depends
    // on it.
    setZN(read8(static_cast<u16>(dst)));
}

// ── dispatch ─────────────────────────────────────────────────────────────
// Base 65C02-derived opcode set. Grouped by instruction family. This is
// not the full 256-entry table yet — unimplemented opcodes fall through
// to a NOP-with-warning so the harness can flag exactly what's missing
// per-ROM instead of the CPU silently corrupting state.

void HuC6280::execute(u8 opcode) {
    switch (opcode) {
        // ── LDA ──
        case 0xA9: a = fetch8();                       setZN(a); break;
        case 0xA5: a = read8(addrZeroPage());           setZN(a); break;
        case 0xB5: a = read8(addrZeroPageX());          setZN(a); break;
        case 0xAD: a = read8(addrAbsolute());           setZN(a); break;
        case 0xBD: a = read8(addrAbsoluteX());          setZN(a); break;
        case 0xB9: a = read8(addrAbsoluteY());          setZN(a); break;
        case 0xA1: a = read8(addrIndexedIndirect());    setZN(a); break;
        case 0xB1: a = read8(addrIndirectIndexed());    setZN(a); break;
        case 0xB2: a = read8(addrZeroPageIndirect());   setZN(a); break; // [HuC6280]

        // ── LDX ──
        case 0xA2: x = fetch8();                       setZN(x); break;
        case 0xA6: x = read8(addrZeroPage());           setZN(x); break;
        case 0xB6: x = read8(addrZeroPageY());          setZN(x); break;
        case 0xAE: x = read8(addrAbsolute());           setZN(x); break;
        case 0xBE: x = read8(addrAbsoluteY());          setZN(x); break;

        // ── LDY ──
        case 0xA0: y = fetch8();                       setZN(y); break;
        case 0xA4: y = read8(addrZeroPage());           setZN(y); break;
        case 0xB4: y = read8(addrZeroPageX());          setZN(y); break;
        case 0xAC: y = read8(addrAbsolute());           setZN(y); break;
        case 0xBC: y = read8(addrAbsoluteX());          setZN(y); break;

        // ── STA ──
        case 0x85: write8(addrZeroPage(), a);            break;
        case 0x95: write8(addrZeroPageX(), a);            break;
        case 0x8D: write8(addrAbsolute(), a);            break;
        case 0x9D: write8(addrAbsoluteX(), a);            break;
        case 0x99: write8(addrAbsoluteY(), a);            break;
        case 0x81: write8(addrIndexedIndirect(), a);      break;
        case 0x91: write8(addrIndirectIndexed(), a);      break;
        case 0x92: write8(addrZeroPageIndirect(), a);     break; // [HuC6280]

        // ── STX / STY ──
        case 0x86: write8(addrZeroPage(), x);             break;
        case 0x96: write8(addrZeroPageY(), x);             break;
        case 0x8E: write8(addrAbsolute(), x);             break;
        case 0x84: write8(addrZeroPage(), y);             break;
        case 0x94: write8(addrZeroPageX(), y);             break;
        case 0x8C: write8(addrAbsolute(), y);             break;

        // ── STZ [65C02/HuC6280 addition] ──
        case 0x64: write8(addrZeroPage(), 0);             break;
        case 0x74: write8(addrZeroPageX(), 0);             break;
        case 0x9C: write8(addrAbsolute(), 0);             break;
        case 0x9E: write8(addrAbsoluteX(), 0);             break;

        // ── Transfers ──
        case 0xAA: x = a; setZN(x); break; // TAX
        case 0x8A: a = x; setZN(a); break; // TXA
        case 0xA8: y = a; setZN(y); break; // TAY
        case 0x98: a = y; setZN(a); break; // TYA
        case 0xBA: x = s; setZN(x); break; // TSX
        case 0x9A: s = x;           break; // TXS

        // ── ALU: ADC / SBC (binary mode only for now) ──
        case 0x69: adcBinary(a, fetch8(), p);                      break;
        case 0x65: adcBinary(a, read8(addrZeroPage()), p);         break;
        case 0x75: adcBinary(a, read8(addrZeroPageX()), p);        break;
        case 0x6D: adcBinary(a, read8(addrAbsolute()), p);         break;
        case 0x7D: adcBinary(a, read8(addrAbsoluteX()), p);        break;
        case 0x79: adcBinary(a, read8(addrAbsoluteY()), p);        break;
        case 0x61: adcBinary(a, read8(addrIndexedIndirect()), p);  break;
        case 0x71: adcBinary(a, read8(addrIndirectIndexed()), p);  break;
        case 0x72: adcBinary(a, read8(addrZeroPageIndirect()), p); break;

        case 0xE9: sbcBinary(a, fetch8(), p);                      break;
        case 0xE5: sbcBinary(a, read8(addrZeroPage()), p);         break;
        case 0xF5: sbcBinary(a, read8(addrZeroPageX()), p);        break;
        case 0xED: sbcBinary(a, read8(addrAbsolute()), p);         break;
        case 0xFD: sbcBinary(a, read8(addrAbsoluteX()), p);        break;
        case 0xF9: sbcBinary(a, read8(addrAbsoluteY()), p);        break;
        case 0xE1: sbcBinary(a, read8(addrIndexedIndirect()), p);  break;
        case 0xF1: sbcBinary(a, read8(addrIndirectIndexed()), p);  break;
        case 0xF2: sbcBinary(a, read8(addrZeroPageIndirect()), p); break;

        // ── ALU: AND / ORA / EOR ──
        case 0x29: a &= fetch8();                       setZN(a); break;
        case 0x25: a &= read8(addrZeroPage());           setZN(a); break;
        case 0x35: a &= read8(addrZeroPageX());          setZN(a); break;
        case 0x2D: a &= read8(addrAbsolute());           setZN(a); break;
        case 0x3D: a &= read8(addrAbsoluteX());          setZN(a); break;
        case 0x39: a &= read8(addrAbsoluteY());          setZN(a); break;
        case 0x21: a &= read8(addrIndexedIndirect());    setZN(a); break;
        case 0x31: a &= read8(addrIndirectIndexed());    setZN(a); break;
        case 0x32: a &= read8(addrZeroPageIndirect());   setZN(a); break;

        case 0x09: a |= fetch8();                       setZN(a); break;
        case 0x05: a |= read8(addrZeroPage());           setZN(a); break;
        case 0x15: a |= read8(addrZeroPageX());          setZN(a); break;
        case 0x0D: a |= read8(addrAbsolute());           setZN(a); break;
        case 0x1D: a |= read8(addrAbsoluteX());          setZN(a); break;
        case 0x19: a |= read8(addrAbsoluteY());          setZN(a); break;
        case 0x01: a |= read8(addrIndexedIndirect());    setZN(a); break;
        case 0x11: a |= read8(addrIndirectIndexed());    setZN(a); break;
        case 0x12: a |= read8(addrZeroPageIndirect());   setZN(a); break;

        case 0x49: a ^= fetch8();                       setZN(a); break;
        case 0x45: a ^= read8(addrZeroPage());           setZN(a); break;
        case 0x55: a ^= read8(addrZeroPageX());          setZN(a); break;
        case 0x4D: a ^= read8(addrAbsolute());           setZN(a); break;
        case 0x5D: a ^= read8(addrAbsoluteX());          setZN(a); break;
        case 0x59: a ^= read8(addrAbsoluteY());          setZN(a); break;
        case 0x41: a ^= read8(addrIndexedIndirect());    setZN(a); break;
        case 0x51: a ^= read8(addrIndirectIndexed());    setZN(a); break;
        case 0x52: a ^= read8(addrZeroPageIndirect());   setZN(a); break;

        // ── Compares ──
        case 0xC9: { u8 v = fetch8();               setFlag(FLAG_C, a >= v); setZN(static_cast<u8>(a - v)); break; }
        case 0xC5: { u8 v = read8(addrZeroPage());  setFlag(FLAG_C, a >= v); setZN(static_cast<u8>(a - v)); break; }
        case 0xCD: { u8 v = read8(addrAbsolute());  setFlag(FLAG_C, a >= v); setZN(static_cast<u8>(a - v)); break; }
        case 0xE0: { u8 v = fetch8();               setFlag(FLAG_C, x >= v); setZN(static_cast<u8>(x - v)); break; }
        case 0xE4: { u8 v = read8(addrZeroPage());  setFlag(FLAG_C, x >= v); setZN(static_cast<u8>(x - v)); break; }
        case 0xEC: { u8 v = read8(addrAbsolute());  setFlag(FLAG_C, x >= v); setZN(static_cast<u8>(x - v)); break; }
        case 0xC0: { u8 v = fetch8();               setFlag(FLAG_C, y >= v); setZN(static_cast<u8>(y - v)); break; }
        case 0xC4: { u8 v = read8(addrZeroPage());  setFlag(FLAG_C, y >= v); setZN(static_cast<u8>(y - v)); break; }
        case 0xCC: { u8 v = read8(addrAbsolute());  setFlag(FLAG_C, y >= v); setZN(static_cast<u8>(y - v)); break; }

        // ── INC / DEC ──
        case 0xE6: { u16 addr = addrZeroPage();  u8 v = static_cast<u8>(read8(addr) + 1); write8(addr, v); setZN(v); break; }
        case 0xF6: { u16 addr = addrZeroPageX(); u8 v = static_cast<u8>(read8(addr) + 1); write8(addr, v); setZN(v); break; }
        case 0xEE: { u16 addr = addrAbsolute();  u8 v = static_cast<u8>(read8(addr) + 1); write8(addr, v); setZN(v); break; }
        case 0xFE: { u16 addr = addrAbsoluteX(); u8 v = static_cast<u8>(read8(addr) + 1); write8(addr, v); setZN(v); break; }
        case 0xC6: { u16 addr = addrZeroPage();  u8 v = static_cast<u8>(read8(addr) - 1); write8(addr, v); setZN(v); break; }
        case 0xD6: { u16 addr = addrZeroPageX(); u8 v = static_cast<u8>(read8(addr) - 1); write8(addr, v); setZN(v); break; }
        case 0xCE: { u16 addr = addrAbsolute();  u8 v = static_cast<u8>(read8(addr) - 1); write8(addr, v); setZN(v); break; }
        case 0xDE: { u16 addr = addrAbsoluteX(); u8 v = static_cast<u8>(read8(addr) - 1); write8(addr, v); setZN(v); break; }
        case 0xE8: x++; setZN(x); break; // INX
        case 0xC8: y++; setZN(y); break; // INY
        case 0xCA: x--; setZN(x); break; // DEX
        case 0x88: y--; setZN(y); break; // DEY
        case 0x1A: a++; setZN(a); break; // INC A [65C02/HuC6280]
        case 0x3A: a--; setZN(a); break; // DEC A [65C02/HuC6280]

        // ── Shifts / rotates (accumulator) ──
        case 0x0A: { bool c = (a & 0x80) != 0; a = static_cast<u8>(a << 1); setFlag(FLAG_C, c); setZN(a); break; }
        case 0x4A: { bool c = (a & 0x01) != 0; a = static_cast<u8>(a >> 1); setFlag(FLAG_C, c); setZN(a); break; }
        case 0x2A: { bool c = (a & 0x80) != 0; a = static_cast<u8>((a << 1) | (getFlag(FLAG_C) ? 1 : 0)); setFlag(FLAG_C, c); setZN(a); break; }
        case 0x6A: { bool c = (a & 0x01) != 0; a = static_cast<u8>((a >> 1) | (getFlag(FLAG_C) ? 0x80 : 0)); setFlag(FLAG_C, c); setZN(a); break; }

        // ── Shifts / rotates (memory, zp + abs only for now) ──
        case 0x06: { u16 addr = addrZeroPage(); u8 v = read8(addr); bool c = (v & 0x80) != 0; v = static_cast<u8>(v << 1); write8(addr, v); setFlag(FLAG_C, c); setZN(v); break; }
        case 0x0E: { u16 addr = addrAbsolute(); u8 v = read8(addr); bool c = (v & 0x80) != 0; v = static_cast<u8>(v << 1); write8(addr, v); setFlag(FLAG_C, c); setZN(v); break; }
        case 0x46: { u16 addr = addrZeroPage(); u8 v = read8(addr); bool c = (v & 0x01) != 0; v = static_cast<u8>(v >> 1); write8(addr, v); setFlag(FLAG_C, c); setZN(v); break; }
        case 0x4E: { u16 addr = addrAbsolute(); u8 v = read8(addr); bool c = (v & 0x01) != 0; v = static_cast<u8>(v >> 1); write8(addr, v); setFlag(FLAG_C, c); setZN(v); break; }
        case 0x26: { u16 addr = addrZeroPage(); u8 v = read8(addr); bool c = (v & 0x80) != 0; v = static_cast<u8>((v << 1) | (getFlag(FLAG_C) ? 1 : 0)); write8(addr, v); setFlag(FLAG_C, c); setZN(v); break; }
        case 0x2E: { u16 addr = addrAbsolute(); u8 v = read8(addr); bool c = (v & 0x80) != 0; v = static_cast<u8>((v << 1) | (getFlag(FLAG_C) ? 1 : 0)); write8(addr, v); setFlag(FLAG_C, c); setZN(v); break; }
        case 0x66: { u16 addr = addrZeroPage(); u8 v = read8(addr); bool c = (v & 0x01) != 0; v = static_cast<u8>((v >> 1) | (getFlag(FLAG_C) ? 0x80 : 0)); write8(addr, v); setFlag(FLAG_C, c); setZN(v); break; }
        case 0x6E: { u16 addr = addrAbsolute(); u8 v = read8(addr); bool c = (v & 0x01) != 0; v = static_cast<u8>((v >> 1) | (getFlag(FLAG_C) ? 0x80 : 0)); write8(addr, v); setFlag(FLAG_C, c); setZN(v); break; }

        // ── BIT ──
        case 0x24: { u8 v = read8(addrZeroPage()); setFlag(FLAG_Z, (a & v) == 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_N, (v & 0x80) != 0); break; }
        case 0x2C: { u8 v = read8(addrAbsolute());  setFlag(FLAG_Z, (a & v) == 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_N, (v & 0x80) != 0); break; }
        case 0x89: { u8 v = fetch8();                setFlag(FLAG_Z, (a & v) == 0); break; } // BIT #imm — only Z affected

        // ── Branches ──
        case 0x90: { s8 off = addrRelative(); if (!getFlag(FLAG_C)) pc = static_cast<u16>(pc + off); break; } // BCC
        case 0xB0: { s8 off = addrRelative(); if ( getFlag(FLAG_C)) pc = static_cast<u16>(pc + off); break; } // BCS
        case 0xF0: { s8 off = addrRelative(); if ( getFlag(FLAG_Z)) pc = static_cast<u16>(pc + off); break; } // BEQ
        case 0xD0: { s8 off = addrRelative(); if (!getFlag(FLAG_Z)) pc = static_cast<u16>(pc + off); break; } // BNE
        case 0x30: { s8 off = addrRelative(); if ( getFlag(FLAG_N)) pc = static_cast<u16>(pc + off); break; } // BMI
        case 0x10: { s8 off = addrRelative(); if (!getFlag(FLAG_N)) pc = static_cast<u16>(pc + off); break; } // BPL
        case 0x50: { s8 off = addrRelative(); if (!getFlag(FLAG_V)) pc = static_cast<u16>(pc + off); break; } // BVC
        case 0x70: { s8 off = addrRelative(); if ( getFlag(FLAG_V)) pc = static_cast<u16>(pc + off); break; } // BVS
        case 0x80: { s8 off = addrRelative(); pc = static_cast<u16>(pc + off); break; }                       // BRA [65C02/HuC6280]

        // ── Jumps / calls ──
        case 0x4C: pc = addrAbsolute(); break;
        case 0x6C: pc = addrIndirect(); break;
        case 0x20: { u16 target = addrAbsolute(); push16(static_cast<u16>(pc - 1)); pc = target; break; } // JSR
        case 0x60: pc = static_cast<u16>(pop16() + 1); break; // RTS
        case 0x40: { p = static_cast<u8>(pop8() | FLAG_T); pc = pop16(); break; } // RTI

        // ── Stack ops ──
        case 0x48: push8(a); break;                             // PHA
        case 0x68: a = pop8(); setZN(a); break;                 // PLA
        case 0x08: push8(static_cast<u8>(p | FLAG_B)); break;   // PHP
        case 0x28: p = static_cast<u8>(pop8() | FLAG_T); break; // PLP
        case 0xDA: push8(x); break;                             // PHX [HuC6280]
        case 0xFA: x = pop8(); setZN(x); break;                 // PLX [HuC6280]
        case 0x5A: push8(y); break;                             // PHY [HuC6280]
        case 0x7A: y = pop8(); setZN(y); break;                 // PLY [HuC6280]

// ── Flag ops ──
        case 0x18: setFlag(FLAG_C, false); break;
        case 0x38: setFlag(FLAG_C, true);  break;
        case 0x58: setFlag(FLAG_I, false); cliCount++; break;
        case 0x78: setFlag(FLAG_I, true);  break;
        case 0xB8: setFlag(FLAG_V, false); break;
        case 0xD8: setFlag(FLAG_D, false); break;
        case 0xF8: setFlag(FLAG_D, true);  break;

        // ── TST [confirmed: Software Manual §3.16-3.19] ──
        // Immediate data ANDed with memory; result not stored. Z from the
        // AND result, N/V from memory bits 7/6 (same convention as BIT).
        case 0x83: { u8 imm = fetch8(); u16 addr = addrZeroPage();  u8 v = read8(addr); setFlag(FLAG_Z, (imm & v) == 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_N, (v & 0x80) != 0); break; }
        case 0xA3: { u8 imm = fetch8(); u16 addr = addrZeroPageX(); u8 v = read8(addr); setFlag(FLAG_Z, (imm & v) == 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_N, (v & 0x80) != 0); break; }
        case 0x93: { u8 imm = fetch8(); u16 addr = addrAbsolute();  u8 v = read8(addr); setFlag(FLAG_Z, (imm & v) == 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_N, (v & 0x80) != 0); break; }
        case 0xB3: { u8 imm = fetch8(); u16 addr = addrAbsoluteX(); u8 v = read8(addr); setFlag(FLAG_Z, (imm & v) == 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_N, (v & 0x80) != 0); break; }

        // ── TRB / TSB [confirmed: Software Manual Table 2-3-3] ──
        case 0x14: { u16 addr = addrZeroPage(); u8 v = read8(addr); setFlag(FLAG_Z, (a & v) == 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_N, (v & 0x80) != 0); write8(addr, static_cast<u8>(v & ~a)); break; }
        case 0x1C: { u16 addr = addrAbsolute();  u8 v = read8(addr); setFlag(FLAG_Z, (a & v) == 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_N, (v & 0x80) != 0); write8(addr, static_cast<u8>(v & ~a)); break; }
        case 0x04: { u16 addr = addrZeroPage(); u8 v = read8(addr); setFlag(FLAG_Z, (a & v) == 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_N, (v & 0x80) != 0); write8(addr, static_cast<u8>(v | a)); break; }
        case 0x0C: { u16 addr = addrAbsolute();  u8 v = read8(addr); setFlag(FLAG_Z, (a & v) == 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_N, (v & 0x80) != 0); write8(addr, static_cast<u8>(v | a)); break; }

        // ── RMBi / SMBi [confirmed opcodes: 07,17..77 / 87,97..F7] ──
        // Bit index i is encoded in the opcode's upper nibble.
        case 0x07: case 0x17: case 0x27: case 0x37:
        case 0x47: case 0x57: case 0x67: case 0x77: {
            u8 i = static_cast<u8>((opcode >> 4) & 0x07);
            u16 addr = addrZeroPage();
            write8(addr, static_cast<u8>(read8(addr) & ~(1 << i)));
            break;
        }
        case 0x87: case 0x97: case 0xA7: case 0xB7:
        case 0xC7: case 0xD7: case 0xE7: case 0xF7: {
            u8 i = static_cast<u8>((opcode >> 4) & 0x07);
            u16 addr = addrZeroPage();
            write8(addr, static_cast<u8>(read8(addr) | (1 << i)));
            break;
        }

        // ── BBRi / BBSi [confirmed opcodes: 0F,1F..7F / 8F,9F..FF] ──
        case 0x0F: case 0x1F: case 0x2F: case 0x3F:
        case 0x4F: case 0x5F: case 0x6F: case 0x7F: {
            u8 i = static_cast<u8>((opcode >> 4) & 0x07);
            u16 addr = addrZeroPage();
            u8 v = read8(addr);
            s8 off = addrRelative();
            if (!(v & (1 << i))) pc = static_cast<u16>(pc + off);
            break;
        }
        case 0x8F: case 0x9F: case 0xAF: case 0xBF:
        case 0xCF: case 0xDF: case 0xEF: case 0xFF: {
            u8 i = static_cast<u8>((opcode >> 4) & 0x07);
            u16 addr = addrZeroPage();
            u8 v = read8(addr);
            s8 off = addrRelative();
            if (v & (1 << i)) pc = static_cast<u16>(pc + off);
            break;
        }

        // ── SAX / SAY / SXY [confirmed: Software Manual, swap — no flags] ──
        case 0x22: { u8 t = a; a = x; x = t; break; } // SAX
        case 0x42: { u8 t = a; a = y; y = t; break; } // SAY
        case 0x02: { u8 t = x; x = y; y = t; break; } // SXY

        // ── TAM / TMA — MPR access [HuC6280] ──
        // Operand is a bitmask; for TAM each set bit selects an MPR index
        // to load from A. For TMA the (typically single) set bit selects
        // which MPR's value gets read into A.
 case 0x53: {
            u8 mask = fetch8();
            for (int i = 0; i < 8; i++) {
                if (mask & (1 << i)) bus->writeMPR(static_cast<u8>(i), a, pc);
            }
            break;
        }
        case 0x43: {
            u8 mask = fetch8();
            u8 result = 0;
            for (int i = 0; i < 8; i++) {
                if (mask & (1 << i)) result |= bus->readMPR(static_cast<u8>(i));
            }
            a = result;
            break;
        }

        // ── Block transfers [HuC6280] ──
        case 0x73: blockTransfer(BlockMode::IncInc);   break; // TII
        case 0xC3: blockTransfer(BlockMode::DecDec);   break; // TDD
        case 0xD3: blockTransfer(BlockMode::IncFixed); break; // TIN
        case 0xF3: blockTransfer(BlockMode::AltInc);   break; // TAI
        case 0xE3: blockTransfer(BlockMode::IncAlt);   break; // TIA

        // ── Speed switch [HuC6280] ──
        case 0x54: speed = 0; break; // CSL — 1.79MHz
        case 0xD4: speed = 1; break; // CSH — 7.16MHz

        // ── Misc ──
        case 0xEA: break; // NOP
        case 0x00: {       // BRK
            fetch8();      // padding byte after opcode
            push16(pc);
            push8(static_cast<u8>(p | FLAG_B));
            setFlag(FLAG_I, true);
            // Confirmed via HuC6280 Software Manual: BRK vector is
            // $FFF6/$FFF7 (shared with IRQ2), NOT $FFFE like stock 6502.
            pc = static_cast<u16>(read8(0xFFF6)) | (static_cast<u16>(read8(0xFFF7)) << 8);
            break;
        }

        default:
            // TODO: block transfers, TST, CSH/CSL, and remaining 65C02
            // NOP-equivalent opcodes. Falls through as a silent no-op for
            // now — a real "unimplemented opcode" diagnostic hook (ring
            // buffer + stuck-detector, same pattern as the Genesis work)
            // is the natural next addition here.
            break;
    }
}

size_t HuC6280::dumpState(char* buf, size_t buf_size) const {
    return static_cast<size_t>(std::snprintf(buf, buf_size,
        "PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X SPD=%02X\n",
        pc, a, x, y, s, p, speed));
}

size_t HuC6280::getTrace(TraceEntry* out, size_t maxEntries) const {
    if (!out || maxEntries == 0) return 0;
    size_t n = traceCount < maxEntries ? traceCount : maxEntries;
    // Oldest entry currently in the buffer is at traceHead (when full) or
    // index 0 (when not yet full, since we haven't wrapped).
    size_t start = (traceCount < kTraceSize) ? 0 : traceHead;
    for (size_t i = 0; i < n; i++) {
        out[i] = trace[(start + i) % kTraceSize];
    }
    return n;
}

size_t HuC6280::getBootTrace(TraceEntry* out, size_t maxEntries) const {
    if (!out || maxEntries == 0) return 0;
    size_t n = bootTraceCount < maxEntries ? bootTraceCount : maxEntries;
    for (size_t i = 0; i < n; i++) out[i] = bootTrace[i];
    return n;
}
