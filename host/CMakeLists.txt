# SPDX-License-Identifier: GPL-3.0-only
# RemoteFM1 host side: libremotefm1 (portable C99), its macOS back ends, the tests, fm1remote.
#   cmake -S host -B host/build -DCMAKE_BUILD_TYPE=Release && cmake --build host/build -j
#   host/run_tests.sh                (an ASan + UBSan build in host/build-asan, then ctest)
cmake_minimum_required(VERSION 3.16)
project(remotefm1_host C CXX)

set(RFM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
option(RFM_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(RFM_FM1REMOTE "Build fm1remote (needs FM1VST's engine sources)" ON)

# FM1VST next to this repo (the main checkout: ../FM1VST; a worktree under .claude/worktrees: four levels up)
if(NOT FM1VST_DIR)
    foreach(cand ${RFM_ROOT}/../FM1VST ${RFM_ROOT}/../../../../FM1VST)
        if(EXISTS ${cand}/engine/device.cpp AND NOT FM1VST_DIR)
            get_filename_component(FM1VST_DIR ${cand} ABSOLUTE)
        endif()
    endforeach()
endif()
set(FM1VST_DIR ${FM1VST_DIR} CACHE PATH "FM1VST checkout (read-only: engine/ and core-api/ are compiled in)")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
if(RFM_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---- libremotefm1: portable (no OS calls), for macOS, iOS and WASM alike
add_library(remotefm1 STATIC
    src/codec.c src/screen.c src/tiles_ref.c src/panel.c src/session.c src/fake.c)
target_include_directories(remotefm1 PUBLIC include ${RFM_ROOT}/proto PRIVATE ${RFM_ROOT}/firmware/src)
target_compile_options(remotefm1 PRIVATE -Wall -Wextra -Wshadow -Wstrict-prototypes)

# ---- the macOS back ends: CoreMIDI, libusb (vendor bulk)
if(APPLE)
    find_path(LIBUSB_INCLUDE libusb.h PATHS /opt/homebrew/include/libusb-1.0 /usr/local/include/libusb-1.0)
    find_library(LIBUSB_LIB usb-1.0 PATHS /opt/homebrew/lib /usr/local/lib)
    add_library(remotefm1_macos STATIC src/macos/coremidi.c src/macos/usb.c src/macos/host.c)
    target_link_libraries(remotefm1_macos PUBLIC remotefm1 "-framework CoreMIDI" "-framework CoreFoundation" pthread)
    target_compile_options(remotefm1_macos PRIVATE -Wall -Wextra -Wno-deprecated-declarations)
    if(LIBUSB_INCLUDE AND LIBUSB_LIB)
        target_include_directories(remotefm1_macos PRIVATE ${LIBUSB_INCLUDE})
        target_link_libraries(remotefm1_macos PUBLIC ${LIBUSB_LIB})
    else()
        message(WARNING "libusb not found: fm1remote gets no vendor bulk transport")
        target_compile_definitions(remotefm1_macos PRIVATE RFM_NO_LIBUSB=1)
    endif()
endif()

# ---- tests
enable_testing()
foreach(t codec screen panel session)
    add_executable(test_${t} tests/test_${t}.c tests/frames.c)
    target_link_libraries(test_${t} PRIVATE remotefm1)
    target_include_directories(test_${t} PRIVATE tests ${RFM_ROOT}/firmware/src)
    target_compile_options(test_${t} PRIVATE -Wall -Wextra)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()
# the compression report (bytes per frame, frames to converge) for every frame set at both default budgets;
# with FM1VST's screenshots when they are there
set(RFM_PPMS "")
if(FM1VST_DIR)
    file(GLOB RFM_PPMS ${FM1VST_DIR}/build/cores/*.ppm)
endif()
add_test(NAME screen_report COMMAND test_screen --report ${RFM_PPMS})
# the cross test: the host library against the device's own protocol code (firmware/src/rfm_app.c, rfm_draw.c,
# rfm_push.c compiled natively), on a simulated clock, every transport and budget; with the core screenshots
add_executable(test_cross tests/test_cross.c tests/frames.c)
target_link_libraries(test_cross PRIVATE remotefm1)
target_include_directories(test_cross PRIVATE tests ${RFM_ROOT}/firmware/src)
target_compile_options(test_cross PRIVATE -Wall -Wextra -Wno-unused-function -Wno-sign-compare)
add_test(NAME cross COMMAND test_cross ${RFM_PPMS})
set_tests_properties(cross PROPERTIES TIMEOUT 600)
if(APPLE)
    add_executable(test_coremidi tests/test_coremidi.c)
    target_link_libraries(test_coremidi PRIVATE remotefm1_macos)
    target_include_directories(test_coremidi PRIVATE src/macos)
    add_test(NAME coremidi_virtual COMMAND test_coremidi)
endif()

# ---- fm1remote: an .fm1core with the FM-1 as its panel
if(APPLE AND RFM_FM1REMOTE AND FM1VST_DIR)
    add_executable(fm1remote tools/fm1remote/main.cpp tools/fm1remote/engine.cpp tools/fm1remote/audio.cpp
        tools/fm1remote/patterns.cpp ${FM1VST_DIR}/engine/cores.cpp ${FM1VST_DIR}/engine/device.cpp)
    target_include_directories(fm1remote PRIVATE tools/fm1remote ${FM1VST_DIR}/engine ${FM1VST_DIR}/core-api)
    target_link_libraries(fm1remote PRIVATE remotefm1_macos "-framework AudioToolbox" "-framework CoreAudio"
        ${CMAKE_DL_LIBS})
    target_compile_options(fm1remote PRIVATE -Wall -Wextra)
    target_compile_definitions(fm1remote PRIVATE FM1VST_CORES="${FM1VST_DIR}/build/cores")
    if(EXISTS ${FM1VST_DIR}/build/cores/choralroot.fm1core)
        add_test(NAME fm1remote_fake COMMAND fm1remote --core ${FM1VST_DIR}/build/cores/choralroot.fm1core
            --no-audio --fake-test --seconds 5 --flash ${CMAKE_CURRENT_BINARY_DIR}/test-flash.bin --fresh)
        set_tests_properties(fm1remote_fake PROPERTIES ENVIRONMENT "FM1EMU_HOME=${CMAKE_CURRENT_BINARY_DIR}/fm1emu-home"
            TIMEOUT 60)
        add_test(NAME fm1remote_pattern COMMAND fm1remote --pattern --no-audio --fake-test --seconds 3)
        set_tests_properties(fm1remote_pattern PROPERTIES TIMEOUT 30)
    endif()
endif()
