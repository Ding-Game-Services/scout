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
    // TODO: real timing loop interleaving cpu.step()/vdc.runLine() per
    // scanline (currently CPU and VDC run as separate flat passes, not
    // interleaved cycle-by-cycle) — and real NTSC line count (262/263
    // total including vblank) instead of just the visible area.
    cpu.runFrame();
    for (int line = 0; line < vdc.getVisibleHeight(); ++line) {
        vdc.runLine();
    }
    psg.runFrame();
}
