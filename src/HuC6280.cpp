#include "pce_core.h"
#include <cstdio>

void HuC6280::connect(Bus* b) {
    bus = b;
}

void HuC6280::reset() {
    a = x = y = 0;
    s = 0xFF;
    pc = 0;
    p = 0;
    speed = 0;
    // TODO: real reset vector fetch once Bus mapping is wired up
}

void HuC6280::step() {
    // TODO: fetch/decode/execute — 65C02 base op table + HuC6280 extensions
    // (block moves, extra zero-page modes, timer/IRQ controller regs)
}

void HuC6280::runFrame() {
    // TODO: cycle-accurate loop; for now just a placeholder step count
}

size_t HuC6280::dumpState(char* buf, size_t buf_size) const {
    return static_cast<size_t>(std::snprintf(buf, buf_size,
        "PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X SPD=%02X\n",
        pc, a, x, y, s, p, speed));
}
