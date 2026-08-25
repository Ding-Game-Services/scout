/*
 * ding_core_pce.cpp — implements the public sdk/ding_core.h contract for
 * the PC Engine / TurboGrafx-16 core.
 *
 * NOTE: this file calls into ding_md5.cpp for ROM identity — the exact
 * function signature there wasn't available while writing this stub, so
 * that part is left as a clearly marked TODO rather than guessed at.
 */

#include "ding_core.h"
#include "pce_core.h"
#include <cstring>

namespace {

PCEngine g_engine;
bool     g_initialized = false;

// Framebuffer sized for max PCE resolution (512x242 is the widest common
// mode; VDC actually supports several horizontal timings — max_width set
// generously here, refine once VDC output is real).
constexpr uint32_t kMaxWidth  = 512;
constexpr uint32_t kMaxHeight = 242;
uint8_t g_framebuffer[kMaxWidth * kMaxHeight * 4] = {};

DingCoreInfo g_coreInfo = {
    "PCE Core",
    "NEC PC Engine / TurboGrafx-16",
    "0.0.1",
    DING_CORE_API_VERSION_MAJOR,
    DING_CORE_API_VERSION_MINOR,
};

DingVideoInfo g_videoInfo = {
    256, 224,          // matches VDC::kVisibleWidth/kVisibleHeight — see
                       // VDC.cpp for what's simplified (fixed 32x32
                       // virtual screen, no other SCREEN sizes yet)
    kMaxWidth, kMaxHeight,
    DING_PIXFMT_RGBA8,
    1,                 // dynamic — VDC supports multiple horizontal timings
};

DingAudioInfo g_audioInfo = {
    44100,
    2,
};

DingRomIdentity g_romIdentity = {};

DingSaveStateInfo g_saveStateInfo = {
    DING_SAVE_UNSUPPORTED,  // TODO: flip to DING_SAVE_FULL once state serialization exists
    0,
    0,
};

char g_lastError[256] = {};
uint8_t g_hasError = 0;

} // namespace

extern "C" {

// ── Lifecycle ────────────────────────────────────────────────────────────

void ding_init() {
    g_engine.init();
    g_initialized = true;
    g_hasError = 0;
    g_lastError[0] = '\0';
}

void ding_destroy() {
    g_engine.destroy();
    g_initialized = false;
}

void ding_reset() {
    g_engine.reset();
}

void ding_run_frame() {
    g_engine.runFrame();
    g_engine.vce.resolveFramebuffer(
        g_engine.vdc.getVideoCodes(), g_framebuffer,
        g_engine.vdc.getVisibleWidth(), g_engine.vdc.getVisibleHeight());
}

// ── ROM loading ──────────────────────────────────────────────────────────
// Stock cart-only core — no disc plumbing (PCE CD is a future separate core).

DingResult ding_load_rom(const uint8_t* data, size_t len) {
    if (!g_engine.loadRom(data, len)) {
        std::snprintf(g_lastError, sizeof(g_lastError), "ROM load failed");
        g_hasError = 1;
        return DING_ERR_BAD_ROM;
    }

    // TODO: compute real MD5 via sdk/ding_md5.cpp once its signature is
    // confirmed. STRIPPED because PCE HuCards can carry a copier header
    // some dump sets include, per the ding_core.h identity method docs.
    g_romIdentity.method = DING_ID_MD5_STRIPPED;
    std::memset(g_romIdentity.hash, 0, sizeof(g_romIdentity.hash));
    g_romIdentity.serial[0] = '\0';
    g_romIdentity.disc_id[0] = '\0';

    return DING_OK;
}

DingResult ding_load_disc(DingDiscImage* disc) {
    (void)disc;
    return DING_ERR_NO_DISC;   // stock PCE core — no disc support
}

DingResult ding_load_bios(uint32_t bios_index, const uint8_t* data, size_t len) {
    (void)bios_index;
    (void)data;
    (void)len;
    // Stock PCE doesn't require a BIOS to boot HuCards.
    return DING_OK;
}

uint8_t ding_is_disc_swap_pending() {
    return 0;
}

void ding_swap_disc(DingDiscImage* disc) {
    (void)disc;
}

// ── Capability queries ───────────────────────────────────────────────────

const DingCoreInfo* ding_get_core_info() {
    return &g_coreInfo;
}

const DingVideoInfo* ding_get_video_info() {
    return &g_videoInfo;
}

const DingAudioInfo* ding_get_audio_info() {
    return &g_audioInfo;
}

const DingRomIdentity* ding_get_rom_identity() {
    return &g_romIdentity;
}

const DingSaveStateInfo* ding_get_savestate_info() {
    return &g_saveStateInfo;
}

uint32_t ding_get_memory_region_count() {
    // TODO: expose WRAM/VRAM/palette RAM as real regions once Bus/VDC
    // ownership of that memory is finalized.
    return 0;
}

void ding_get_memory_region(uint32_t index, DingMemoryRegion* out) {
    (void)index;
    if (out) std::memset(out, 0, sizeof(DingMemoryRegion));
}

uint32_t ding_get_bios_count() {
    return 0;   // stock PCE requires no BIOS
}

void ding_get_bios_descriptor(uint32_t index, DingBiosDescriptor* out) {
    (void)index;
    if (out) std::memset(out, 0, sizeof(DingBiosDescriptor));
}

uint32_t ding_get_input_descriptor_count() {
    // TODO: standard PCE 2-button pad (I, II, Select, Run, D-Pad) x up to
    // 5 ports via multitap — wire real descriptors once input is built.
    return 0;
}

void ding_get_input_descriptor(uint32_t index, DingInputDescriptor* out) {
    (void)index;
    if (out) std::memset(out, 0, sizeof(DingInputDescriptor));
}

// ── Video output ─────────────────────────────────────────────────────────

const uint8_t* ding_get_framebuffer() {
    return g_framebuffer;
}

void ding_get_current_dimensions(uint32_t* width, uint32_t* height) {
    if (width)  *width  = g_videoInfo.base_width;
    if (height) *height = g_videoInfo.base_height;
}

// ── Audio output ─────────────────────────────────────────────────────────

uint32_t ding_get_audio_sample_count() {
    return 0;   // TODO: real ring buffer once PSG produces samples
}

uint32_t ding_read_audio_samples(float* buf, uint32_t count) {
    return g_engine.psg.readSamples(buf, count);
}

// ── Input ────────────────────────────────────────────────────────────────

void ding_set_button(uint8_t port, uint8_t index, uint8_t pressed) {
    (void)port;   // single pad only for now — multitap not modeled
    g_engine.joypad.setButton(index, pressed != 0);
}

void ding_set_axis(uint8_t port, uint8_t index, int16_t value) {
    (void)port;
    (void)index;
    (void)value;
    // PCE pads are digital-only — no-op unless a future analog peripheral needs it
}

// ── Save states ──────────────────────────────────────────────────────────

size_t ding_save_state(uint8_t* buf, size_t buf_size) {
    (void)buf;
    (void)buf_size;
    return 0;   // DING_SAVE_UNSUPPORTED for now
}

DingResult ding_load_state(const uint8_t* buf, size_t len) {
    (void)buf;
    (void)len;
    return DING_ERR_BAD_STATE;
}

// ── Region / config ──────────────────────────────────────────────────────

void ding_set_region(const char* region) {
    (void)region;
    // TODO: PCE region mostly affects timing (7.16MHz base clock is fixed
    // NTSC-derived even on PAL-market Japanese-only releases) — revisit
    // once real timing is implemented.
}

// ── Diagnostics ──────────────────────────────────────────────────────────

size_t ding_diag_cpu_state(char* buf, size_t buf_size) {
    return g_engine.cpu.dumpState(buf, buf_size);
}

size_t ding_diag_video_state(char* buf, size_t buf_size) {
    return g_engine.vdc.dumpState(buf, buf_size);
}

uint8_t ding_has_error() {
    return g_hasError;
}

const char* ding_diag_last_error() {
    return g_hasError ? g_lastError : nullptr;
}

} // extern "C"
