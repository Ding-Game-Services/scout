#include "pce_core.h"
#include <cstring>

void PSG::reset() {
    sampleCount = 0;
}

void PSG::runFrame() {
    // TODO: 6-channel wavetable synthesis, 2 channels w/ noise/LFO modes
}

uint32_t PSG::readSamples(float* buf, uint32_t count) {
    // TODO: pull from ring buffer once runFrame() actually produces samples
    if (!buf) return 0;
    std::memset(buf, 0, static_cast<size_t>(count) * sizeof(float));
    return 0;
}
