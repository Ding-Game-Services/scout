#include "pce_core.h"

void PCEngine::init() {
    bus.connect(&cartridge);
    bus.connect(&vdc);
    bus.connect(&vce);
    bus.connect(&psg);
    bus.connect(&timer);
    bus.connect(&joypad);
    bus.connect(&irqController);
    cpu.connect(&bus);
    cpu.connect(&irqController);
    vdc.connect(&bus);
    vdc.connect(&irqController);
}

void PCEngine::destroy() {
    // nothing to free yet — everything is owned by value on PCEngine
}

void PCEngine::reset() {
    bus.reset();
    cpu.reset();
    vdc.reset();
    vce.reset();
    psg.reset();
    timer.reset();
    joypad.reset();
    irqController.reset();
}

bool PCEngine::loadRom(const uint8_t* data, size_t len) {
    return cartridge.load(data, len);
}

void PCEngine::runFrame() {
    // BUG FIX (harness testing — Alien Crush and others): this used to run
    // cpu.runFrame() (the CPU's entire 119,904-cycle budget) BEFORE any
    // vdc.runLine() calls. Since VDC's vblank IRQ1 assertion only happens
    // inside runLine(), the CPU could never observe an IRQ1 during its own
    // execution — any game polling a vblank-driven flag (an extremely
    // common, correct pattern) would spin forever. Confirmed via IRQ
    // counter diagnostics: CLI executed, interrupts were enabled, but
    // IRQ1_ASSERT stayed 0 for hundreds of frames under the old ordering.
    //
    // Now interleaved per visible scanline so VDC gets a chance to run
    // (and fire IRQ1) partway through the CPU's frame, not just after it.
    //
    // STILL a simplification vs real hardware:
    //   - kCyclesPerLine is 119904 split evenly across only the visible
    //     lines (224), not the real 262/263-line NTSC total including
    //     vblank — so overall frame cycle count is right but per-line
    //     timing isn't scanline-accurate yet.
    //   - CPU cycles don't line up 1:1 with real HuC6280 per-opcode
    //     timing (see HuC6280::step()'s existing TODO), so line boundaries
    //     are approximate regardless.
    // Good enough to unblock IRQ-dependent boot code; real scanline-
    // accurate timing is future work.
    const int visibleLines = vdc.getVisibleHeight();
    const u64 kCyclesPerLine = 119904 / static_cast<u64>(visibleLines);

    for (int line = 0; line < visibleLines; ++line) {
        cpu.runFor(kCyclesPerLine);
        vdc.runLine();
    }
    psg.runFrame();
}
