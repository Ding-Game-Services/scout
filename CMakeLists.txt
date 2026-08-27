# ── CMakeLists.txt ────────────────────────────────────────────────────────────
# D!NG PC Engine / TurboGrafx-16 core (pce-core)
#
# Stock cart-based PCE/TG16 only. PC Engine CD will be a future, separate
# core built on top of this one (extra RAM + CD-ROM² handling) — no disc
# plumbing here.
#
# Two build outputs, matching every other Ding core repo:
#   1. Native shared lib (.dll on Windows / .so on Linux/Steam Deck/ROG Ally)
#      for Hydra, loaded like any other libretro-style core.
#   2. WASM module (built with Emscripten) for the browser emulator frontend.
#
# Usage — native (Windows, MinGW Makefiles; Ninja isn't available there):
#   cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# Usage — native (Linux/Steam Deck):
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# Usage — WASM (requires emsdk activated in your shell):
#   emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-wasm
# ─────────────────────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.20)
project(ding_pce_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# x86_64 only — matches every other Ding core (Windows/Linux/Steam Deck/ROG Ally).
# Emscripten targets WASM32 by design, so skip this check under emcc.
if(NOT EMSCRIPTEN AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ding_pce_core targets x86_64 only (got a non-64-bit toolchain)")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── ding-core-sdk (submoduled as sdk/) ───────────────────────────────────────
set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
if(NOT EXISTS ${SDK_DIR}/ding_core.h)
    message(FATAL_ERROR
        "sdk/ is missing ding-core-sdk headers. Did you forget:\n"
        "  git submodule update --init --recursive")
endif()

set(SDK_SOURCES
    ${SDK_DIR}/ding_md5.cpp
    ${SDK_DIR}/ding_savestate.cpp
    ${SDK_DIR}/ding_audio.cpp
)

# ── Core sources ──────────────────────────────────────────────────────────────
# HuC6280 = CPU (65C02-derived) + built-in PSG + timer/IRQ controller.
# VDC     = Video Display Controller (HuC6270) — BG/sprite rendering.
# VCE     = Video Color Encoder (HuC6260) — palette -> RGB output.
# Filled in incrementally as each piece is implemented; stub source added
# now so the skeleton compiles from the start.
set(CORE_SOURCES
    src/Cartridge.cpp
    src/Bus.cpp
    src/HuC6280.cpp
    src/VDC.cpp
    src/VCE.cpp
    src/PSG.cpp
    src/Timer.cpp
    src/Joypad.cpp
    src/IrqController.cpp
    src/PCEngine.cpp
    src/ding_core_pce.cpp
)

set(ALL_SOURCES ${CORE_SOURCES} ${SDK_SOURCES})

# ── Target ───────────────────────────────────────────────────────────────────
if(EMSCRIPTEN)
    # Every function declared in ding_core.h must appear here or Emscripten's
    # linker dead-strips it — nothing in our C++ calls these, only JS does.
    set(EXPORTED_FUNCTIONS
        "_ding_init"
        "_ding_destroy"
        "_ding_reset"
        "_ding_run_frame"
        "_ding_load_rom"
        "_ding_load_bios"
        "_ding_get_core_info"
        "_ding_get_video_info"
        "_ding_get_audio_info"
        "_ding_get_rom_identity"
        "_ding_get_savestate_info"
        "_ding_get_memory_region_count"
        "_ding_get_memory_region"
        "_ding_get_bios_count"
        "_ding_get_bios_descriptor"
        "_ding_get_input_descriptor_count"
        "_ding_get_input_descriptor"
        "_ding_get_framebuffer"
        "_ding_get_current_dimensions"
        "_ding_get_audio_sample_count"
        "_ding_read_audio_samples"
        "_ding_set_button"
        "_ding_set_axis"
        "_ding_save_state"
        "_ding_load_state"
        "_ding_set_region"
        "_ding_diag_cpu_state"
        "_ding_diag_video_state"
"_ding_diag_read_memory"
        "_ding_diag_trace"
"_ding_diag_boot_trace"
        "_ding_diag_vdc_reg_log"
        "_ding_diag_add_watchpoint"
        "_ding_diag_clear_watchpoints"
 "_ding_diag_watch_log"
        "_ding_diag_mpr_log"
        "_ding_diag_irq_state"
        "_ding_has_error"
        "_ding_diag_last_error"
        "_malloc"
        "_free"
    )
    string(JOIN "," EXPORTED_FUNCTIONS_CSV ${EXPORTED_FUNCTIONS})
    set(EXPORTED_FUNCTIONS_STR "[${EXPORTED_FUNCTIONS_CSV}]")

    # Single WASM+JS glue output. MODULARIZE lets the frontend `await
    # createDingPceCore()` instead of polluting the global scope — matches
    # how genesis-core/gameboy-core's WASM builds are consumed already.
    add_executable(ding_pce_core ${ALL_SOURCES})
    target_link_options(ding_pce_core PRIVATE
        "SHELL:-s WASM=1"
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s EXPORT_NAME=createDingPceCore"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s NO_EXIT_RUNTIME=1"
        "SHELL:-s ENVIRONMENT=web"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','getValue','setValue','HEAPU8','UTF8ToString']"
        "SHELL:-s EXPORTED_FUNCTIONS=${EXPORTED_FUNCTIONS_STR}"
        --no-entry
    )
    set_target_properties(ding_pce_core PROPERTIES SUFFIX ".js")
else()
    add_library(ding_pce_core SHARED ${ALL_SOURCES})
    set_target_properties(ding_pce_core PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        PREFIX ""            # -> ding_pce_core.dll / ding_pce_core.so, no lib prefix
    )
endif()

target_include_directories(ding_pce_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SDK_DIR}
)

if(MSVC)
    target_compile_options(ding_pce_core PRIVATE /W4)
else()
    target_compile_options(ding_pce_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ── Optional: quick compile-only sanity target for CI ────────────────────────
# `cmake --build build --target ding_pce_core_check` — same sources, no link
# step, just confirms every .cpp still compiles against the current headers.
add_custom_target(ding_pce_core_check
    COMMAND ${CMAKE_COMMAND} -E echo "ding_pce_core: sources compiled as part of the main target"
    DEPENDS ding_pce_core
)
