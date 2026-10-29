# SPDX-License-Identifier: GPL-3.0-only
# FIZZ (https://github.com/Quixotic7/FIZZ, MPL-2.0) as an .rfm1 file for the site (web/site/fw/), with FM1VST's
# fm1_add_core and fm1_add_rfm1 (cmake/FM1Cores.cmake, cmake/FM1Rfm1.cmake). Configured by tools/rfm1/build_fizz.sh,
# which prepares FIZZ_DIR (FIZZ at its release tag, without site/ and core/tests/, which are GPL) and FM1VST_DIR (at
# the commit the glue is pinned to). The core's call is RemoteFM1IOS's cores/fizz/core.cmake, the same units.
cmake_minimum_required(VERSION 3.22)
set(FM1VST_DIR "" CACHE PATH "FM1VST at the pinned commit (its cmake/, core-api/, core-glue/, tools/, third_party/wasm3)")
set(FIZZ_DIR "" CACHE PATH "FIZZ at its release, without site/ and core/tests/")
if(NOT EXISTS ${FM1VST_DIR}/cmake/FM1Rfm1.cmake OR NOT EXISTS ${FIZZ_DIR}/core/glue/core_quadrille.c)
    message(FATAL_ERROR "run tools/rfm1/build_fizz.sh (FM1VST_DIR ${FM1VST_DIR}, FIZZ_DIR ${FIZZ_DIR})")
endif()
# (built_by names FM1VST's version: FM1Rfm1.cmake reads PROJECT_VERSION)
file(STRINGS ${FM1VST_DIR}/CMakeLists.txt _p REGEX "^project\\(FM1VST VERSION ")
string(REGEX MATCH "VERSION ([0-9.]+)" _m "${_p}")
project(FIZZrfm1 VERSION ${CMAKE_MATCH_1} LANGUAGES C)

set(CR_PYTHON "python3" CACHE FILEPATH "python3 (rfm1_pack.py, rfm1_check.py)")
find_library(MATH_LIB m)
set(FM1_RFM1 ON CACHE BOOL "" FORCE)
include(${FM1VST_DIR}/cmake/FM1Cores.cmake)
include(${FM1VST_DIR}/cmake/FM1Rfm1.cmake)

set(_qd ${FIZZ_DIR})
set(_qd_glue ${_qd}/core/glue)
set(_qd_pvg ${_qd}/third_party/plutovg/source)
set(_qd_pvg_sources plutovg-blend.c plutovg-canvas.c plutovg-font.c plutovg-ft-math.c plutovg-ft-raster.c
                    plutovg-ft-stroker.c plutovg-matrix.c plutovg-paint.c plutovg-path.c plutovg-rasterize.c
                    plutovg-surface.c)
file(GLOB _qd_pvg_found RELATIVE ${_qd_pvg} ${_qd_pvg}/*.c)
list(SORT _qd_pvg_found)
if(NOT _qd_pvg_found STREQUAL _qd_pvg_sources)
    message(FATAL_ERROR "fizz: plutovg's sources changed (${_qd_pvg_found}): update tools/rfm1/fizz/CMakeLists.txt")
endif()
list(TRANSFORM _qd_pvg_sources PREPEND ../../third_party/plutovg/source/)   # (EXTRA_SOURCES: glue dir relative)
# FIZZ's tools/Makefile CORE_SRC with GFX=plutovg; every other .c in core/ is included by one of these
set(_qd_sources ../app/app.c ../app/ctl.c ../app/ctl_step.c ../app/ctl_rec.c ../app/ctl_leds.c ../app/ctl_latch.c
                ../app/ctl_preview.c ../app/view.c
                ../ui/ui_kit.c ../ui/ui_idle.c ../ui/ui_keys.c ../ui/ui_picker.c ../ui/ui_tracks.c ../ui/ui_lane.c
                ../ui/ui_held.c ../ui/ui_turn.c ../ui/ui_layer.c ../ui/ui_octave.c
                ../gfx/qd_gfx_plutovg.c
                ../seq/model.c ../seq/pattern.c ../seq/seq.c ../seq/player.c ../seq/demo.c ../seq/chord.c
                ../seq/scale.c ../seq/sounds.c ../seq/clip.c
                ../voice/quad/quad_voice.c ../voice/pool.c ../voice/mix.c ../voice/fx/qd_fx.c)
file(RELATIVE_PATH _qd_desc ${_qd_glue} ${CMAKE_CURRENT_LIST_DIR}/fizz_desc.c)
set(FIZZ_VERSION "" CACHE STRING "FIZZ's version (its release tag without the v)")
fm1_add_core(fizz ${_qd_glue} SOURCE ${_qd}
             UNIT core_quadrille.c DESC ${_qd_desc}
             EXTRA_SOURCES ${_qd_sources} ${_qd_pvg_sources}
             INCLUDES ${_qd_glue} ${_qd}/core ${_qd}/core/app ${_qd}/core/ui ${_qd}/core/gfx ${_qd}/core/seq
                      ${_qd}/core/voice ${_qd}/core/voice/quad ${_qd}/core/voice/fx ${_qd}/third_party/plutovg/include
                      ${_qd_pvg}
             DEFINES PLUTOVG_BUILD PLUTOVG_BUILD_STATIC PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD
             FAMILY fizz LICENSE MPL-2.0
             CREDITS "FIZZ, an FM TONE step sequencer for the FM-1, by Michael P Jones (Quixotic7). MPL-2.0, the source: https://github.com/Quixotic7/FIZZ (LICENSING.md says what is under which licence).\nFM TONE, the effects and the output chain: from ChoralRoot FM-1 v0.25.1, the owner's, relicensed MPL-2.0, and the parts that came from Felucca were rewritten (L1).\nDrawn with plutovg v1.3.3 (Samuel Ugochukwu, MIT), which carries stb_truetype, stb_image and stb_image_write (Sean Barrett, MIT or Unlicense). Portions of this software are copyright (c) 2006 The FreeType Project (www.freetype.org). All rights reserved.\nType: Inter Tight (The Inter Project Authors, SIL Open Font License 1.1).\nThis .rfm1: FIZZ's core built to WebAssembly by RemoteFM1 (tools/rfm1/build_fizz.sh, https://github.com/Quixotic7/RemoteFM1) with FM1VST's core glue (GPL-3.0-only) and Emscripten's runtime (MIT, with musl libc, MIT). Its plutovg-ft-raster.c has one change, for WebAssembly: the render pool's overflow without setjmp (tools/rfm1/fizz/plutovg-ft-raster-wasm.patch, FTL)."
             CREDITS_FILES ./LICENSE third_party/plutovg/LICENSE third_party/plutovg/source/FTL.TXT assets/fonts/OFL.txt
             NO_GEN
             VERSION ${FIZZ_VERSION})
set(_qd_pvg_abs ${_qd_pvg_sources})
list(TRANSFORM _qd_pvg_abs PREPEND ${_qd_glue}/)
set_source_files_properties(${_qd_pvg_abs} TARGET_DIRECTORY core_fizz_obj PROPERTIES COMPILE_OPTIONS "-w")

# the .rfm1: FIZZ's licence texts and its third parties' (fm1_add_rfm1 adds FM1VST's, Emscripten's and musl's)
set(FIZZ_MEMORY_MIB 16 CACHE STRING "the .rfm1's fixed memory")
fm1_add_rfm1(fizz
    AUTHORS "Michael P Jones (Quixotic7)"
    MEMORY_MIB ${FIZZ_MEMORY_MIB}
    # (the asserts' __FILE__: the trees' names, not this machine's paths, so a build elsewhere gives the same file)
    FLAGS -ffile-prefix-map=${FIZZ_DIR}=FIZZ -ffile-prefix-map=${FM1VST_DIR}=FM1VST
    LICENSE_FILES "MPL-2.0 ({name})=LICENSE"
                  "LICENSING.md ({name})=LICENSING.md"
                  "MIT (plutovg)=third_party/plutovg/LICENSE"
                  "FTL (FreeType, in plutovg)=third_party/plutovg/source/FTL.TXT"
                  "OFL-1.1 (Inter Tight)=assets/fonts/OFL.txt")
fm1_rfm1_finish()
