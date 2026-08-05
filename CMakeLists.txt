cmake_minimum_required(VERSION 3.22)   # JUCE 8.0.15's own floor; 3.20 was a lie

# ================================================================
#  MORGUE -- an instrument for dark noise, ambience and terror.
#
#  The audio core (engine.c + the pure-C DSP) is built as a plain C
#  static library with no binding to any audio device, so the SAME
#  code drives the JUCE front end and the headless test target.
#  See engine.h for the seam.
#
#  Three targets:
#     bbengine      the instrument, device-independent, pure C11
#     morgue-tests  the regression suite -- NO JUCE, so the engine
#                   can be proven on a machine with nothing but a
#                   C compiler and no submodule checked out
#     MORGUE        the JUCE console
# ================================================================

project(MORGUE VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Say which dialect we mean. Left unset, CMake emits -std=gnu11/-std=gnu++17 on
# GCC and Clang, so the build silently accepts GNU extensions that the strict
# -std=c11 -pedantic the old Makefile used would have rejected. Two build
# systems accepting different languages is how a Linux-only extension creeps in
# and only fails on the machine you do not own.
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# A realtime audio instrument built -O0 will glitch and you will blame the
# code. Every documented invocation uses a single-config generator without
# -DCMAKE_BUILD_TYPE, so without this the default is an unoptimised build.
get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
    message(STATUS "MORGUE: no CMAKE_BUILD_TYPE given, defaulting to RelWithDebInfo")
endif()

# ----------------------------------------------------------------
#  The engine: device-independent instrument core
# ----------------------------------------------------------------
add_library(bbengine STATIC
    engine.c
    expr.c
    dsp.c
    knob.c
    rack.c
    gen.c
    bb_platform.c
)
# These are C11 sources and must be compiled with a C compiler -- C++ has no
# _Atomic() and the engine's lock-free swap depends on the C memory model.
set_source_files_properties(engine.c expr.c dsp.c knob.c rack.c gen.c bb_platform.c
    PROPERTIES LANGUAGE C
)
set_target_properties(bbengine PROPERTIES LINKER_LANGUAGE C)
target_include_directories(bbengine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Bytebeat overflows signed integers on essentially every sample and that
# overflow IS the instrument.
#
# -fwrapv is GCC/Clang only. MSVC has no equivalent and, passed one, emits
# `warning D9002: ignoring unknown option` and carries on -- so on Windows the
# guarantee simply evaporated, silently, which is the worst way for a
# correctness flag to fail. The real fix was in the code: expr.c already routed
# its arithmetic through uint32 (U()/S()), and the negative left-shifts in
# engine.c and dsp.c have been rewritten to shift through the matching unsigned
# type. The flag stays for GCC/Clang as defence in depth, not as the guarantee.
target_compile_options(bbengine PRIVATE
    $<$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:AppleClang>>:-fwrapv>
)

# MSVC ships C11 atomics only behind an opt-in switch. Without it, <stdatomic.h>
# fails with `error C1189: C atomic support is not enabled` even though /std:c11
# was requested -- verified on 19.44.35228.
if(MSVC)
    if(CMAKE_C_COMPILER_VERSION VERSION_LESS 19.34)
        message(FATAL_ERROR
            "MORGUE needs Visual Studio 2022 17.5 or newer for C11 atomics "
            "(found ${CMAKE_C_COMPILER_VERSION}).")
    endif()
    target_compile_options(bbengine PRIVATE /experimental:c11atomics)
    # The CRT deprecation warnings on fopen/getenv are noise here: every one of
    # those call sites now goes through bb_platform.c, which does the widening
    # and the checking the "secure" variants would have done.
    target_compile_definitions(bbengine PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(bbengine PUBLIC m)
endif()

# ----------------------------------------------------------------
#  The regression suite
#
#  Deliberately independent of JUCE: this target configures and runs
#  with third_party/JUCE completely empty, so the engine can be
#  proven on any machine with a C compiler. The suite used to live
#  inside main.c and link -lasound -lncurses, which meant it could
#  only ever run on Linux -- on the one platform whose front end was
#  being deleted.
# ----------------------------------------------------------------
add_executable(morgue-tests tests/engine_tests.c)
set_source_files_properties(tests/engine_tests.c PROPERTIES LANGUAGE C)
set_target_properties(morgue-tests PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(morgue-tests PRIVATE bbengine)
if(MSVC)
    target_compile_options(morgue-tests PRIVATE /experimental:c11atomics)
    target_compile_definitions(morgue-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

enable_testing()
add_test(NAME engine COMMAND morgue-tests)

# ----------------------------------------------------------------
#  JUCE
# ----------------------------------------------------------------
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt)
    # Fail loudly and usefully rather than with an opaque add_subdirectory
    # error -- but do NOT stop the configure, because bbengine and morgue-tests
    # above are perfectly buildable without JUCE and are what you want when you
    # are bisecting the engine.
    message(WARNING
        "third_party/JUCE is empty -- the MORGUE console will not be built.\n"
        "   Fix it with:  git submodule update --init --recursive\n"
        "   bbengine and morgue-tests are still configured and will build.")
    return()
endif()

set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/JUCE)

# ----------------------------------------------------------------
#  Embedded fonts (IBM Plex Mono 400/500 + IBM Plex Sans Condensed 700,
#  OFL; spec section 2 -- shipped as binary resources, no system fallback)
# ----------------------------------------------------------------
juce_add_binary_data(MorgueFonts
    SOURCES
        app/fonts/IBMPlexMono-Regular.ttf
        app/fonts/IBMPlexMono-Medium.ttf
        app/fonts/IBMPlexSansCondensed-Bold.ttf
)

# ----------------------------------------------------------------
#  The application
#
#  juce_add_gui_app, not a bare add_executable. On Windows JUCE defines
#  WinMain and a plain add_executable links /SUBSYSTEM:CONSOLE, so the
#  linker goes looking for main() and does not find it. This also gets
#  the macOS bundle and the Info.plist right without hand-setting them.
# ----------------------------------------------------------------
juce_add_gui_app(MORGUE
    PRODUCT_NAME    "MORGUE"
    COMPANY_NAME    "MORGUE"
    BUNDLE_ID       "com.morgue.instrument"
    VERSION         ${PROJECT_VERSION}
)

target_sources(MORGUE PRIVATE
    app/Main.cpp
    app/AudioEngine.cpp
    app/Theme.cpp
    app/Primitives.cpp
    app/panels/Chrome.cpp
    app/panels/RackPanel.cpp
    app/panels/ArrangePanel.cpp
    app/panels/LicksPanel.cpp
    app/panels/GrainMassPanel.cpp
    app/panels/SurvivorPanel.cpp
    app/panels/MixerPanel.cpp
    app/panels/HwSyncPanel.cpp
    app/panels/ExportSheet.cpp
    app/panels/FieldManual.cpp
)

target_compile_definitions(MORGUE PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MORGUE,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:MORGUE,JUCE_VERSION>"
    # No web view anywhere in this instrument. Left at its default of 1 this
    # makes libwebkit2gtk-4.1-dev a hard build dependency on Linux, for nothing.
    JUCE_WEB_BROWSER=0
    # Windows uses WinINet and macOS NSURLSession, both with the OS trust store,
    # so curl buys nothing on the two platforms that matter here. NOTE: JUCE's
    # non-curl POSIX fallback has no TLS at all, so if EXHUME ever needs to make
    # HTTPS requests from a Linux build, this has to go back to 1 and link curl.
    JUCE_USE_CURL=0
)

target_include_directories(MORGUE PRIVATE app)

target_link_libraries(MORGUE
    PRIVATE
        bbengine
        MorgueFonts
        juce::juce_gui_extra
        juce::juce_gui_basics
        juce::juce_graphics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_basics
        juce::juce_events
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
)

if(MSVC)
    # 71 lines across app/ carry raw UTF-8 inside string literals and not one
    # file has a BOM, so without this MSVC decodes them through the machine's
    # ANSI codepage and the console fills with mojibake.
    target_compile_options(MORGUE PRIVATE /utf-8)
    target_compile_definitions(MORGUE PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(APPLE)
    # App icon: rendered by app/icon/make_icon.swift in the design language
    # (regenerate with: swift app/icon/make_icon.swift app/fonts <iconset> &&
    #  iconutil -c icns <iconset> -o app/icon/MORGUE.icns)
    set(MORGUE_ICON ${CMAKE_CURRENT_SOURCE_DIR}/app/icon/MORGUE.icns)
    if(EXISTS ${MORGUE_ICON})
        target_sources(MORGUE PRIVATE ${MORGUE_ICON})
        set_source_files_properties(${MORGUE_ICON} PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
        set_target_properties(MORGUE PROPERTIES
            MACOSX_BUNDLE_ICON_FILE "MORGUE.icns")
    endif()
endif()
