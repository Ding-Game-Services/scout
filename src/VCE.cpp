#include "pce_core.h"
#include <cstring>

void VCE::reset() {
    std::memset(palette, 0, sizeof(palette));
}

void VCE::writeFramebuffer(uint8_t* fb, uint32_t width, uint32_t height) {
    // TODO: palette -> RGBA8 conversion per pixel, driven by VDC output
    if (!fb) return;
    std::memset(fb, 0, static_cast<size_t>(width) * height * 4);
}
