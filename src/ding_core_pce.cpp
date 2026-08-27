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

    // BUG FOUND (harness trace): if reset() runs before the ROM is loaded,
    // the CPU's reset-vector fetch hits an empty cartridge (open bus,
    // reads as 0xFF/0xFF) and latches PC=$FFFF, then stumbles through
    // whatever real ROM bytes happen to follow once the ROM does load —
    // never running actual boot code. Re-resetting here guarantees a
    // correct reset-vector fetch regardless of caller call order.
    g_engine.reset();

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
    size_t n = g_engine.cpu.dumpState(buf, buf_size);
    if (n >= buf_size) return n;

    // Appended here rather than a separate diag call — MPR banking is
    // exactly the kind of thing that silently invalidates memory peeks
    // if you don't know it at the same time (page 0 reads RAM only if
    // MPR0 is banked to $F8; otherwise "zero page" is actually ROM).
    int r = std::snprintf(buf + n, buf_size - n,
        "MPR0=%02X MPR1=%02X MPR2=%02X MPR3=%02X MPR4=%02X MPR5=%02X MPR6=%02X MPR7=%02X\n",
        g_engine.bus.readMPR(0), g_engine.bus.readMPR(1),
        g_engine.bus.readMPR(2), g_engine.bus.readMPR(3),
        g_engine.bus.readMPR(4), g_engine.bus.readMPR(5),
        g_engine.bus.readMPR(6), g_engine.bus.readMPR(7));
    if (r > 0) n += static_cast<size_t>(r);
    return n;
}

size_t ding_diag_video_state(char* buf, size_t buf_size) {
    return g_engine.vdc.dumpState(buf, buf_size);
}

uint8_t ding_has_error() {
    return g_hasError;
}

// ── Extra diagnostics (not part of sdk/ding_core.h contract) ─────────────
// Raw memory peek for harness debugging. Goes through Bus::read(), so it
// respects MPR banking exactly like the CPU would see it — but note some
// hardware-page reads have side effects (e.g. VDC's VRR auto-increment on
// high-byte read). Safe for ROM/RAM peeking, use with care on $FF-bank
// addresses.
size_t ding_diag_read_memory(uint16_t addr, uint8_t* buf, size_t len) {
    if (!buf) return 0;
    for (size_t i = 0; i < len; i++) {
        u16 wrapped = static_cast<u16>(addr + i);   // wrap at 64K like real CPU address space
        buf[i] = g_engine.bus.read(static_cast<u32>(wrapped));
    }
    return len;
}

size_t ding_diag_trace(char* buf, size_t buf_size) {
    HuC6280::TraceEntry entries[HuC6280::kTraceSize];
    size_t n = g_engine.cpu.getTrace(entries, HuC6280::kTraceSize);

    size_t written = 0;
    for (size_t i = 0; i < n && written < buf_size; i++) {
        int r = std::snprintf(buf + written, buf_size - written, "%04X:%02X ",
            entries[i].pc, entries[i].opcode);
        if (r <= 0) break;
        written += static_cast<size_t>(r);
    }
    if (written < buf_size) buf[written] = '\0';
    return written;
}

namespace {
const char* vdcRegName(uint8_t idx) {
    switch (idx) {
        case 0x00: return "MAWR";
        case 0x01: return "MARR";
        case 0x02: return "VWR";
        case 0x05: return "CR";
        case 0x06: return "RCR";
        case 0x07: return "BXR";
        case 0x08: return "BYR";
        case 0x09: return "MWR";
        case 0x0A: return "HSR";
        case 0x0B: return "HDR";
        case 0x0C: return "VPR";
        case 0x0D: return "VDR";
        case 0x0E: return "VCR";
        case 0x0F: return "DCR";
        case 0x10: return "SOUR";
        case 0x11: return "DESR";
        case 0x12: return "LENR";
        case 0x13: return "DVSSR";
        default:   return "?";
    }
}
} // namespace

void ding_diag_add_watchpoint(uint16_t addr) {
    g_engine.bus.addWatchpoint(addr);
}

void ding_diag_clear_watchpoints() {
    g_engine.bus.clearWatchpoints();
}

size_t ding_diag_mpr_log(char* buf, size_t buf_size) {
    Bus::MprLogEntry entries[Bus::kMprLogSize];
    size_t n = g_engine.bus.getMprLog(entries, Bus::kMprLogSize);

    size_t written = 0;
    for (size_t i = 0; i < n && written < buf_size; i++) {
        int r = std::snprintf(buf + written, buf_size - written, "[%04X]MPR%u=%02X ",
            entries[i].pc, entries[i].index, entries[i].bank);
        if (r <= 0) break;
        written += static_cast<size_t>(r);
    }
    if (written < buf_size) buf[written] = '\0';
    return written;
}

size_t ding_diag_watch_log(char* buf, size_t buf_size) {
    Bus::WatchEntry entries[Bus::kWatchLogSize];
    size_t n = g_engine.bus.getWatchLog(entries, Bus::kWatchLogSize);

    size_t written = 0;
    for (size_t i = 0; i < n && written < buf_size; i++) {
        int r = std::snprintf(buf + written, buf_size - written, "[%04X]$%04X=%02X ",
            entries[i].pc, entries[i].addr, entries[i].val);
        if (r <= 0) break;
        written += static_cast<size_t>(r);
    }
    if (written < buf_size) buf[written] = '\0';
    return written;
}

size_t ding_diag_vdc_reg_log(char* buf, size_t buf_size) {
    VDC::RegLogEntry entries[VDC::kRegLogSize];
    size_t n = g_engine.vdc.getRegLog(entries, VDC::kRegLogSize);

    size_t written = 0;
    for (size_t i = 0; i < n && written < buf_size; i++) {
        int r = std::snprintf(buf + written, buf_size - written, "[%04X]%s=%04X ",
            entries[i].pc, vdcRegName(entries[i].regIndex), entries[i].value);
        if (r <= 0) break;
        written += static_cast<size_t>(r);
    }
    if (written < buf_size) buf[written] = '\0';
    return written;
}

size_t ding_diag_boot_trace(char* buf, size_t buf_size) {
    HuC6280::TraceEntry entries[HuC6280::kBootTraceSize];
    size_t n = g_engine.cpu.getBootTrace(entries, HuC6280::kBootTraceSize);

    size_t written = 0;
    for (size_t i = 0; i < n && written < buf_size; i++) {
        int r = std::snprintf(buf + written, buf_size - written, "%04X:%02X ",
            entries[i].pc, entries[i].opcode);
        if (r <= 0) break;
        written += static_cast<size_t>(r);
    }
    if (written < buf_size) buf[written] = '\0';
    return written;
}

size_t ding_diag_irq_state(char* buf, size_t buf_size) {
    return static_cast<size_t>(std::snprintf(buf, buf_size,
        "CLI_COUNT=%u IRQ2_ASSERT=%u IRQ2_SVC=%u IRQ1_ASSERT=%u IRQ1_SVC=%u TIQ_ASSERT=%u TIQ_SVC=%u\n",
        g_engine.cpu.getCliCount(),
        g_engine.irqController.getAssertCount(0), g_engine.irqController.getServiceCount(0),
        g_engine.irqController.getAssertCount(1), g_engine.irqController.getServiceCount(1),
        g_engine.irqController.getAssertCount(2), g_engine.irqController.getServiceCount(2)));
}

const char* ding_diag_last_error() {
    return g_hasError ? g_lastError : nullptr;
}

} // extern "C"
