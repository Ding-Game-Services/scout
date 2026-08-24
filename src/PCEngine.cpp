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
    // scanline. Placeholder just drives one frame's worth of stub calls.
    cpu.runFrame();
    for (int line = 0; line < 263; ++line) {
        vdc.runLine();
    }
    psg.runFrame();
}
