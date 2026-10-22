# SPDX-License-Identifier: GPL-3.0-only
# The native engine's core build (web/engines/native/build.sh copies this file to the root of its mirror of FM1VST:
# web/build/native/src/CMakeLists.txt, next to the mirrored cmake/, core-api/, core-glue/ and cores/<tree>).
#
# It runs FM1VST's own cmake/FM1Cores.cmake (fm1_add_core) and each core's own core-glue/<family>/core_<id>.cmake
# under Emscripten's toolchain (emcmake), so the generate steps, the defines, the include paths and the units are
# exactly FM1VST's. Only these differ, set here after the core's call:
#   - core-glue/common/coro.c (ucontext) is replaced by the engine's own Asyncify coroutine (FM1WEB_CORO);
#   - the units' -O2 becomes FM1WEB_OPT, and they are built without -fPIC (one static wasm, no dynamic linking);
#   - FM1CORE_COMMIT is the FM1VST submodule's commit (the mirror has no .git: build.sh passes it);
#   - a fork's tools/emu/web/compat (its own shims of macOS headers for its browser build) is on the include path.
# Only the static libraries core_<id> (and their generate steps) are built; build.sh links each with emcc.
# An author's build (build.sh --ext): FM1WEB_TREE is the author's tree (cores/<tree> in the mirror; a glue cmake of the
# author's names it as SOURCE), FM1WEB_FAMILY_DIR the family's glue folder (on the include path of the author's glue
# units), FM1WEB_VERSION_<id> the version the descriptor reports (FM1CORE_VERSION). FM1WEB_HOST: the same core with
# the host's compiler (build.sh --host: the conformance test's reference picture), libm linked.
cmake_minimum_required(VERSION 3.20)
project(fm1web_cores C CXX)

set(CR_PYTHON "" CACHE FILEPATH "python3 with Pillow (+ libraqm) and fontTools, for the cores' generate steps")
set(FM1WEB_CORES "" CACHE STRING "id=core-glue/<family>/core_<id>.cmake=<tree under cores/>;..")
set(FM1WEB_CORO "" CACHE FILEPATH "the coroutine that replaces core-glue/common/coro.c")
set(FM1WEB_OPT "-Os" CACHE STRING "the optimisation level of the units")
set(FM1WEB_TREE "" CACHE PATH "an author's build: the firmware's tree")
set(FM1WEB_FAMILY_DIR "" CACHE PATH "an author's build: the family's glue folder, for the author's glue units")
option(FM1WEB_HOST "build with the host's compiler (the reference picture), not Emscripten" OFF)
set(FM1_PUBLIC_BUILD ON CACHE BOOL "only the un-gated cores" FORCE)
if(NOT CR_PYTHON)
    message(FATAL_ERROR "CR_PYTHON is required")
endif()
if(FM1WEB_HOST)
    set(MATH_LIB m)
else()
    set(MATH_LIB "")                              # (Emscripten's libc has libm)
endif()

include(cmake/FM1Cores.cmake)
set(FM1_COMMON_SOURCES ${FM1_COMMON_DIR}/nor.c ${FM1_COMMON_DIR}/fifo.c ${FM1WEB_CORO})
set_source_files_properties(${FM1_COMMON_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-fno-common")

# the golden checks of the glue files refer to FM1VST's test library: never built here
add_library(fm1testlib INTERFACE)

foreach(_e IN LISTS FM1WEB_CORES)
    string(REPLACE "=" ";" _p "${_e}")
    list(GET _p 0 _id)
    list(GET _p 1 _cm)
    list(GET _p 2 _tree)
    include(${CMAKE_SOURCE_DIR}/${_cm})
    if(NOT TARGET core_${_id}_obj)
        message(FATAL_ERROR "${_id}: ${_cm} made no core (gated in a public build?)")
    endif()
    get_target_property(_o core_${_id}_obj COMPILE_OPTIONS)
    list(TRANSFORM _o REPLACE "^-O2$" "${FM1WEB_OPT}")
    set_target_properties(core_${_id}_obj PROPERTIES COMPILE_OPTIONS "${_o}" POSITION_INDEPENDENT_CODE OFF)
    get_target_property(_d core_${_id}_obj COMPILE_DEFINITIONS)
    if(DEFINED FM1WEB_COMMIT_${_id})
        list(TRANSFORM _d REPLACE "^FM1CORE_COMMIT=.*$" "FM1CORE_COMMIT=\"${FM1WEB_COMMIT_${_id}}\"")
        set_target_properties(core_${_id}_obj PROPERTIES COMPILE_DEFINITIONS "${_d}")
    endif()
    if(DEFINED FM1WEB_VERSION_${_id})
        get_target_property(_d core_${_id}_obj COMPILE_DEFINITIONS)
        list(TRANSFORM _d REPLACE "^FM1CORE_VERSION=.*$" "FM1CORE_VERSION=\"${FM1WEB_VERSION_${_id}}\"")
        set_target_properties(core_${_id}_obj PROPERTIES COMPILE_DEFINITIONS "${_d}")
        message(STATUS "${_id} core: reports version ${FM1WEB_VERSION_${_id}} (the release's)")
    endif()
    # an author's glue units (core-glue/ext_<id>) reach the family's headers as FM1VST's own units do
    if(FM1WEB_FAMILY_DIR AND _cm MATCHES "^core-glue/ext_")
        target_include_directories(core_${_id}_obj PRIVATE ${FM1WEB_FAMILY_DIR})
    endif()
    # a fork's own browser shims for the macOS headers its emulator unit includes (ChoralRoot's family:
    # tools/emu/web/compat/os/lock.h, as the fork's own build_web.sh passes -I; without its -DEMU_WEB)
    if(EXISTS ${CMAKE_SOURCE_DIR}/cores/${_tree}/tools/emu/web/compat)
        target_include_directories(core_${_id}_obj PRIVATE ${CMAKE_SOURCE_DIR}/cores/${_tree}/tools/emu/web/compat)
    endif()
endforeach()
