#include "pce_core.h"
#include <cstring>
#include <cstdio>

void VDC::connect(Bus* b) {
    bus = b;
}

void VDC::reset() {
    std::memset(vram, 0, sizeof(vram));
}

void VDC::runLine() {
    // TODO: BG plane + sprite rendering per scanline
}

size_t VDC::dumpState(char* buf, size_t buf_size) const {
    return static_cast<size_t>(std::snprintf(buf, buf_size, "VDC: (stub)\n"));
}
