cmake_minimum_required(VERSION 3.22)   # JUCE 8.0.15's own floor; the 3.20 we
                                       # used to claim was never true, it just
                                       # happened not to fail yet.

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
#
#  Build it with a preset (see CMakePresets.json), e.g.
#     cmake --preset windows-msvc-relwithdebinfo
#     cmake --build --preset windows-msvc-relwithdebinfo
#     ctest --preset windows-msvc-relwithdebinfo
# ================================================================

project(MORGUE VERSION 0.1.0 LANGUAGES C CXX)

# ----------------------------------------------------------------
#  Language dialects
#
#  Both standards are set BEFORE add_subdirectory(third_party/JUCE)
#  below, which means they apply to every JUCE target as well as to
#  ours. That is deliberate, not an accident of ordering:
#
#    - JUCE 8 requires C++17 or newer anyway, and building JUCE at a
#      different standard than the app that includes its headers is
#      how you get an ODR violation that only shows up as a crash in
#      a release build. One standard for the whole tree.
#
#    - C++17 specifically, not 20 or 23. bb_atomic.h exists precisely
#      so that nothing here needs MSVC's C++23-only <stdatomic.h>;
#      raising this to 23 to "fix" atomics would undo that work.
#
#  EXTENSIONS OFF is the other half. Left unset, CMake emits
#  -std=gnu11/-std=gnu++17 on GCC and Clang, so the build silently
#  accepts GNU extensions that the strict -std=c11 -pedantic the old
#  Makefile used would have rejected. Two build systems accepting
#  different languages is how a Linux-only extension creeps in and
#  only fails on the machine you do not own.
#
#  The cost of EXTENSIONS OFF is real and is paid below: strict
#  -std=c11 defines __STRICT_ANSI__, which makes glibc hide every
#  POSIX declaration. See morgue_configure_c_target().
# ----------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# A realtime audio instrument built -O0 will glitch and you will blame the
# code. Every documented invocation uses a single-config generator without
# -DCMAKE_BUILD_TYPE, so without this the default is an unoptimised build.
# (The presets all name a build type explicitly; this is for the person who
# types `cmake -B build` from memory.)
get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
    message(STATUS "MORGUE: no CMAKE_BUILD_TYPE given, defaulting to RelWithDebInfo")
endif()

# ----------------------------------------------------------------
#  Everything the C targets need, in one place
#
#  bbengine and morgue-tests are the same language built the same way
#  -- morgue-tests includes bytebeat.h and calls bb_platform.h, so
#  every flag that makes bbengine compile makes the suite compile
#  too. Keeping the list in a function is what stops the two from
#  drifting apart, which is a real failure mode: a suite built with a
#  different view of the C library than the library it is testing is
#  worse than no suite.
# ----------------------------------------------------------------
function(morgue_configure_c_target tgt)

    # -- Bytebeat overflows signed integers on essentially every sample and
    #    that overflow IS the instrument.
    #
    #    -fwrapv is GCC/Clang only. MSVC has no equivalent and, passed one,
    #    emits `warning D9002: ignoring unknown option` and carries on -- so on
    #    Windows the guarantee simply evaporated, silently, which is the worst
    #    way for a correctness flag to fail. The real fix was in the code:
    #    expr.c already routed its arithmetic through uint32 (U()/S()), and the
    #    negative left-shifts in engine.c and dsp.c have been rewritten to
    #    shift through the matching unsigned type. The flag stays for GCC/Clang
    #    as defence in depth, not as the guarantee.
    #
    # -- -Wall -Wextra is what the old Makefile compiled this code with, so it
    #    is already known to be quiet. -pedantic is deliberately NOT here: the
    #    files written for the port have not been through it, and a wall of new
    #    noise trains people to stop reading warnings.
    #
    #    Written as a plain if() rather than a $<C_COMPILER_ID:...> generator
    #    expression because both targets here are C-only, so there is no
    #    per-language ambiguity to resolve, and because a genex carrying a list
    #    of flags has to be quoted or it gets split on its own semicolons at
    #    parse time. This reads the same and cannot be got subtly wrong.
    if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${tgt} PRIVATE -fwrapv -Wall -Wextra)
    endif()

    # -- POSIX visibility.
    #
    #    This is the bill for CMAKE_C_EXTENSIONS OFF. With -std=c11 (rather
    #    than -std=gnu11) the compiler defines __STRICT_ANSI__, and glibc reads
    #    that as "this program wants ISO C and nothing else": <time.h> stops
    #    declaring clock_gettime, <stdio.h> stops declaring fileno, <unistd.h>
    #    stops declaring fsync, getpid and rmdir. Every one of those is used --
    #    bb_platform.c needs clock_gettime and mkdir, engine.c needs
    #    fileno/fsync for the session flush, engine_tests.c needs getpid and
    #    rmdir -- so without this the Linux build dies on implicit
    #    declarations, which GCC 14 and Clang 16 make hard errors.
    #
    #    _GNU_SOURCE is exactly what the old Makefile passed (see its comment
    #    at line 22), so this is not a new dependency, only a newly explicit
    #    one. On Darwin the equivalent is _DARWIN_C_SOURCE; asking Apple's
    #    headers for _POSIX_C_SOURCE instead would RESTRICT them, which is the
    #    opposite of what is wanted.
    if(APPLE)
        target_compile_definitions(${tgt} PRIVATE _DARWIN_C_SOURCE)
    elseif(UNIX)
        target_compile_definitions(${tgt} PRIVATE _GNU_SOURCE)
    endif()

    if(MSVC)
        # MSVC ships C11 atomics only behind an opt-in switch. Without it,
        # <stdatomic.h> fails with `error C1189: C atomic support is not
        # enabled` even though /std:c11 was requested -- verified on
        # 19.44.35228. bb_atomic.h routes every C translation unit through that
        # header, so this is not optional for any target that includes
        # bytebeat.h, which is all of them.
        #
        # Scoped to COMPILE_LANGUAGE:C so that if this ever propagates to a C++
        # consumer it evaporates rather than turning into another D9002.
        target_compile_options(${tgt} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)

        # The CRT deprecation warnings on fopen/getenv are noise here: every
        # one of those call sites now goes through bb_platform.c, which does
        # the widening and the checking the "secure" variants would have done.
        # (bb_platform.c defines this itself if the build does not; engine.c
        # and engine_tests.c rely on the build.)
        target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS)

        # No /utf-8 here, unlike on MORGUE below, and that is checked rather
        # than assumed: every C source and every shared header is pure 7-bit
        # ASCII, non-ASCII test data included (engine_tests.c spells it with
        # \xNN escapes, and says why). There is nothing for MSVC to decode
        # through the ANSI codepage, so the flag would only be decoration.
    endif()

endfunction()

if(MSVC AND CMAKE_C_COMPILER_VERSION VERSION_LESS 19.35)
    # /experimental:c11atomics landed in Visual Studio 2022 17.5 (toolset
    # 19.35). Earlier 2022 toolsets accept /std:c11 and then have no
    # <stdatomic.h> to give you, so the failure is a confusing C1189 several
    # headers deep. Say so here instead.
    message(FATAL_ERROR
        "MORGUE needs Visual Studio 2022 17.5 or newer for C11 atomics "
        "(/experimental:c11atomics); found MSVC ${CMAKE_C_COMPILER_VERSION}.")
endif()

# ----------------------------------------------------------------
#  The engine: device-independent instrument core
# ----------------------------------------------------------------
set(MORGUE_ENGINE_SOURCES
    engine.c
    expr.c
    dsp.c
    knob.c
    rack.c
    gen.c
    bb_platform.c
)

add_library(bbengine STATIC ${MORGUE_ENGINE_SOURCES})

# These are C11 sources and must be compiled with a C compiler -- C++ has no
# _Atomic() and the engine's lock-free swap depends on the C memory model.
# (bb_atomic.h gives the GUI a std::atomic<> view of the same bytes; it does
# not make the engine itself compilable as C++, nor should it.)
set_source_files_properties(${MORGUE_ENGINE_SOURCES} PROPERTIES LANGUAGE C)
set_target_properties(bbengine PROPERTIES LINKER_LANGUAGE C)

target_include_directories(bbengine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
morgue_configure_c_target(bbengine)

if(UNIX AND NOT APPLE)
    # libm is a separate library only on Linux and the BSDs. This used to read
    # `if(NOT APPLE)`, which is also true on Windows, where there is no m.lib
    # and the link died with LNK1104. macOS folds libm into libSystem.
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
morgue_configure_c_target(morgue-tests)

enable_testing()
add_test(NAME engine COMMAND morgue-tests)

# ----------------------------------------------------------------
#  JUCE
# ----------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
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
#  App icon
#
#  Rendered by app/icon/make_icon.swift in the design language
#  (regenerate with: swift app/icon/make_icon.swift app/fonts <iconset> &&
#   iconutil -c icns <iconset> -o app/icon/MORGUE.icns)
#
#  It has to be wired in BEFORE juce_add_gui_app, not after, and it cannot go
#  through the obvious MACOSX_BUNDLE_ICON_FILE property. JUCE does not use
#  CMake's Info.plist machinery: it writes the plist itself, at configure time,
#  from a record file assembled inside juce_add_gui_app (JUCEUtils.cmake:364).
#  A property set afterwards is read by nobody, and CFBundleIconFile comes out
#  empty. JUCE's own ICON_BIG is no help either -- it feeds juceaide, which
#  rasterises PNG/JPEG into an .icns and cannot take an .icns as input.
#
#  PLIST_TO_MERGE is the seam that does work: JUCE parses it as the plist and
#  then adds its own keys with addKeyIfNotFound (juce_PlistOptions.cpp:80), so
#  a CFBundleIconFile we put there survives. The file itself still has to be
#  copied into Contents/Resources, which is the second half below.
# ----------------------------------------------------------------
set(MORGUE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/app/icon/MORGUE.icns")
set(MORGUE_ICON_PLIST_ARG "")
if(APPLE AND EXISTS "${MORGUE_ICON}")
    set(MORGUE_ICON_PLIST_ARG PLIST_TO_MERGE
        "<plist><dict><key>CFBundleIconFile</key><string>MORGUE.icns</string></dict></plist>")
endif()

# ----------------------------------------------------------------
#  The application
#
#  juce_add_gui_app, not a bare add_executable. On Windows JUCE defines
#  WinMain and a plain add_executable links /SUBSYSTEM:CONSOLE, so the
#  linker goes looking for main() and does not find it; juce_add_gui_app
#  sets WIN32_EXECUTABLE and MACOSX_BUNDLE for us (JUCEUtils.cmake:903)
#  and gets the bundle layout right on every platform.
# ----------------------------------------------------------------
juce_add_gui_app(MORGUE
    PRODUCT_NAME    "MORGUE"
    COMPANY_NAME    "MORGUE"
    BUNDLE_ID       "com.morgue.instrument"
    VERSION         ${PROJECT_VERSION}
    ${MORGUE_ICON_PLIST_ARG}
)

if(APPLE AND EXISTS "${MORGUE_ICON}")
    target_sources(MORGUE PRIVATE "${MORGUE_ICON}")
    set_source_files_properties("${MORGUE_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
endif()

target_sources(MORGUE PRIVATE
    app/Main.cpp
    app/AudioEngine.cpp
    app/Theme.cpp
    app/Primitives.cpp
    app/Ledger.cpp
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
    app/panels/ExhumePanel.cpp
    app/panels/PlatePanel.cpp
)

target_compile_definitions(MORGUE PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MORGUE,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:MORGUE,JUCE_VERSION>"
    # No web view anywhere in this instrument. Left at its default of 1 this
    # makes libwebkit2gtk-4.1-dev a hard build dependency on Linux, for
    # nothing, and drags in WebKit.framework on macOS and the ActiveX/WebView2
    # shims on Windows. (On Linux JUCE also emits JUCE_WEB_BROWSER=0 of its own
    # accord, because NEEDS_WEB_BROWSER was not requested above --
    # JUCEUtils.cmake:268. The two agree, so the duplicate -D is inert. On
    # Windows and macOS JUCE emits nothing and this line is the whole switch.)
    JUCE_WEB_BROWSER=0
    # Windows uses WinINet and macOS NSURLSession, both with the OS trust
    # store, so curl buys nothing on the two platforms that matter here. NOTE:
    # JUCE's non-curl POSIX fallback has no TLS at all, so if EXHUME ever needs
    # to make HTTPS requests from a Linux build, this has to go back to 1 and
    # the target needs NEEDS_CURL TRUE so JUCE links libcurl.
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
        # SHA-256 for the accession register's content hashes, and MD5 to check
        # a download against the per-file checksum archive.org publishes in its
        # /metadata response. Implemented in-module, so this pulls in no
        # external dependency on any platform.
        juce::juce_cryptography
    PUBLIC
        juce::juce_recommended_config_flags
        # LTO is scoped to $<CONFIG:Release> inside JUCE, so it does nothing in
        # the RelWithDebInfo builds the presets produce. That is the intended
        # trade: RelWithDebInfo for daily work and for profiling the audio
        # thread, Release when you are cutting a build to hand to someone.
        juce::juce_recommended_lto_flags
)

if(MSVC)
    # 71 lines across app/ carry raw UTF-8 -- in comments now that the string
    # literals have been converted to \xNN escapes -- and not one file has a
    # BOM. Without this MSVC decodes them through the machine's ANSI codepage
    # and every one of them becomes a C4819 warning.
    target_compile_options(MORGUE PRIVATE /utf-8)
    target_compile_definitions(MORGUE PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
