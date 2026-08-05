cmake_minimum_required(VERSION 3.20)

# ================================================================
#  MORGUE -- an instrument for dark noise, ambience and terror.
#
#  The audio core (engine.c + the pure-C DSP) is built as a plain C
#  static library with no binding to any audio device, so the SAME
#  code that drives the terminal instrument's ALSA thread drives this
#  JUCE front end. See engine.h for the seam.
# ================================================================

project(MORGUE VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
)
# These are C11 sources and must be compiled with a C compiler -- C++ has no
# _Atomic() and the engine's lock-free swap depends on the C memory model.
set_source_files_properties(engine.c expr.c dsp.c knob.c rack.c gen.c
    PROPERTIES LANGUAGE C
)
set_target_properties(bbengine PROPERTIES LINKER_LANGUAGE C)
target_include_directories(bbengine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Bytebeat overflows signed integers on essentially every sample and that
# overflow IS the instrument (see the Makefile notes).
target_compile_options(bbengine PRIVATE -fwrapv)

# ----------------------------------------------------------------
#  JUCE
# ----------------------------------------------------------------
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
# ----------------------------------------------------------------
add_executable(MORGUE
    app/Main.cpp
    app/AudioEngine.h
    app/AudioEngine.cpp
    app/Theme.h
    app/Theme.cpp
    app/Primitives.h
    app/Primitives.cpp
    app/panels/Chrome.h
    app/panels/Chrome.cpp
    app/panels/RackPanel.h
    app/panels/RackPanel.cpp
    app/panels/ArrangePanel.h
    app/panels/ArrangePanel.cpp
    app/panels/LicksPanel.h
    app/panels/LicksPanel.cpp
    app/panels/GrainMassPanel.h
    app/panels/GrainMassPanel.cpp
    app/panels/SurvivorPanel.h
    app/panels/SurvivorPanel.cpp
    app/panels/MixerPanel.h
    app/panels/MixerPanel.cpp
    app/panels/HwSyncPanel.h
    app/panels/HwSyncPanel.cpp
    app/panels/ExportSheet.h
    app/panels/ExportSheet.cpp
    app/panels/FieldManual.h
    app/panels/FieldManual.cpp
)
target_compile_definitions(MORGUE PRIVATE JUCE_USE_CURL=0)
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
)
if(NOT APPLE)
    target_link_libraries(MORGUE PRIVATE m)
endif()

if(APPLE)
    # App icon: rendered by app/icon/make_icon.swift in the design language
    # (regenerate with: swift app/icon/make_icon.swift app/fonts <iconset> &&
    #  iconutil -c icns <iconset> -o app/icon/MORGUE.icns)
    set(MORGUE_ICON ${CMAKE_CURRENT_SOURCE_DIR}/app/icon/MORGUE.icns)
    target_sources(MORGUE PRIVATE ${MORGUE_ICON})
    set_source_files_properties(${MORGUE_ICON} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
endif()

set_target_properties(MORGUE PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "MORGUE"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.morgue.instrument"
    MACOSX_BUNDLE_ICON_FILE "MORGUE.icns"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)
