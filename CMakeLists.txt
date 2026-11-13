cmake_minimum_required(VERSION 3.24)

# Before project(): on Apple platforms project() creates an empty cache entry
# for the deployment target, and a set(... CACHE ...) after it never applies,
# so the plugin only loads on the macOS it was built on (PsyBox, PhaseFucker
# and BuildApp shipped with minos 26.0 this way until 2026-10-17). FORCE also
# fills that empty entry in existing build dirs; an explicit
# -DCMAKE_OSX_DEPLOYMENT_TARGET still wins. Check: otool -l <binary> | grep minos
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# Release builds pass the tag's version, e.g. -DVOXAL_VERSION=1.2.0.
set(VOXAL_VERSION "0.1.1" CACHE STRING "Plugin version, MAJOR.MINOR.PATCH")
project(VoxAl VERSION ${VOXAL_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Static MSVC runtime, so the Windows plugin doesn't need the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(VOXAL_BUILD_PLUGIN "Build the VST3/AU plugin (needs JUCE)" ON)
option(VOXAL_BUILD_TESTS "Build the DSP unit tests" ON)
option(VOXAL_COPY_PLUGIN "Copy the built plugins into the system plug-in folders" ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# Core: the DSP. Plain C++20, no JUCE, so the tests build in seconds and the
# audio code cannot reach into GUI types. Every .cpp is listed by hand (no
# globbing).
# ---------------------------------------------------------------------------
add_library(voxal_core STATIC
    src/core/fx/Params.cpp
    src/core/fx/Order.cpp
    src/core/fx/Chain.cpp
    src/core/fx/PitchShifter.cpp
    src/core/fx/Shift.cpp
    src/core/fx/Clean.cpp
    src/core/fx/Breath.cpp
    src/core/fx/Comp.cpp
    src/core/fx/DeEss.cpp
    src/core/fx/Smooth.cpp
    src/core/fx/Warmth.cpp
    src/core/fx/Air.cpp
    src/core/fx/Radio.cpp
    src/core/fx/Doubler.cpp
    src/core/fx/Delay.cpp
    src/core/fx/Reverb.cpp
    src/core/fx/Presets.cpp
    src/core/dsp/Fft.cpp
    src/core/dsp/Halfband.cpp
)
target_include_directories(voxal_core PUBLIC src/core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # -fcx-limited-range: complex products as the plain formula, without the
    # inf/NaN rescue (__muldc3) the standard asks for; the FFTs and the
    # spectral loops run 6x faster, and no audio value is ever inf or NaN.
    target_compile_options(voxal_core PRIVATE -Wall -Wextra -Wno-unused-parameter -fcx-limited-range)
endif()
set_target_properties(voxal_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Plugin
# ---------------------------------------------------------------------------
if(VOXAL_BUILD_PLUGIN)
    # Point FETCHCONTENT_SOURCE_DIR_JUCE at a local checkout to skip the download.
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)

    # The fonts src/ui/Theme.cpp loads (BuildApp/PhaseFucker: IBM Plex; PsyBox:
    # Inter + JetBrains Mono). Keep the licence file next to them in resources/fonts.
    juce_add_binary_data(voxal_assets SOURCES
        resources/fonts/IBMPlexSansCondensed-Medium.ttf
        resources/fonts/IBMPlexSansCondensed-SemiBold.ttf
        resources/fonts/IBMPlexMono-Regular.ttf
        resources/fonts/IBMPlexMono-Medium.ttf)

    juce_add_plugin(VoxAl
        COMPANY_NAME "Jonas Grunau"
        BUNDLE_ID "de.grunau.voxal"
        PLUGIN_MANUFACTURER_CODE Jgru
        PLUGIN_CODE Voxl
        FORMATS VST3 AU
        PRODUCT_NAME "VoxAl"               # the bundles and what hosts show; the VST3/AU ids come from the codes
        # ---- EFFECT (BuildApp, PhaseFucker) -------------------------------
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        VST3_CATEGORIES Fx Filter          # BuildApp: Fx Filter; PhaseFucker: Fx Tools
        AU_MAIN_TYPE kAudioUnitType_Effect # auval type aufx
        # ---- INSTRUMENT (PsyBox): replace the block above with ------------
        # FORMATS VST3 AU Standalone       # (replaces the FORMATS line above)
        # IS_SYNTH TRUE
        # NEEDS_MIDI_INPUT TRUE
        # NEEDS_MIDI_OUTPUT FALSE
        # IS_MIDI_EFFECT FALSE
        # EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        # VST3_CATEGORIES Instrument Synth
        # AU_MAIN_TYPE kAudioUnitType_MusicDevice   # auval type aumu
        # ---- STANDALONE that takes audio input (PhaseFucker had one before
        # its demo host replaced it; it never shipped it):
        # FORMATS VST3 AU Standalone
        # MICROPHONE_PERMISSION_ENABLED TRUE
        # MICROPHONE_PERMISSION_TEXT "VoxAl listens to its inputs."
        # ---- SIDECHAIN: nothing here. The bus is declared in the processor's
        # BusesProperties (.withInput ("Reference", stereo, true)), see the audio-plugin skill (references/build-and-release.md).
        COPY_PLUGIN_AFTER_BUILD ${VOXAL_COPY_PLUGIN})

    # Listed once, used by the plugin and the dev tools. The tools compile these
    # sources directly (not the plugin target), so a new src/plugin or src/ui
    # .cpp goes here and nowhere else.
    set(VOXAL_UI_SOURCES
        src/plugin/PluginProcessor.cpp
        src/plugin/PluginEditor.cpp
        src/plugin/PresetManager.cpp
        src/ui/Theme.cpp
        src/ui/Controls.cpp
        src/ui/VoiceSpectrum.cpp
        src/ui/ChainStrip.cpp
        src/ui/ModulePanels.cpp
        src/ui/PresetBar.cpp)

    target_sources(VoxAl PRIVATE ${VOXAL_UI_SOURCES})
    target_include_directories(VoxAl PRIVATE src)

    target_compile_definitions(VoxAl PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(VoxAl
        PRIVATE
            voxal_core
            voxal_assets
            juce::juce_audio_utils
            # juce::juce_dsp               # PsyBox links it; add only if the plugin layer uses it
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    # Dev tool: renders the editor to PNG without a host.
    # The tools define the JucePlugin_* macros the processor/editor use
    # themselves (getName() uses JucePlugin_Name, the editor header prints
    # JucePlugin_VersionString). Add any other JucePlugin_* macro you use here
    # and on the demo target too.
    juce_add_console_app(voxal_snapshot PRODUCT_NAME "voxal_snapshot")
    target_sources(voxal_snapshot PRIVATE tools/Snapshot.cpp ${VOXAL_UI_SOURCES})
    target_include_directories(voxal_snapshot PRIVATE src tests)
    target_compile_definitions(voxal_snapshot PRIVATE
        JucePlugin_Name="VoxAl"
        JucePlugin_VersionString="${PROJECT_VERSION}"
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(voxal_snapshot PRIVATE
        voxal_core voxal_assets juce::juce_audio_utils
        juce::juce_recommended_config_flags)

    # Dev tool: the plugin in a window with a built-in psytrance loop (or a
    # file) playing through it. ./start.sh runs it.
    # (An instrument has no demo host: PsyBox's start.sh opens its Standalone.)
    juce_add_gui_app(voxal_demo PRODUCT_NAME "VoxAl Demo")
    target_sources(voxal_demo PRIVATE tools/Demo.cpp ${VOXAL_UI_SOURCES})
    target_include_directories(voxal_demo PRIVATE src tests)
    target_compile_definitions(voxal_demo PRIVATE
        JucePlugin_Name="VoxAl"
        JucePlugin_VersionString="${PROJECT_VERSION}"
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(voxal_demo PRIVATE
        voxal_core voxal_assets juce::juce_audio_utils
        juce::juce_recommended_config_flags)
endif()

# ---------------------------------------------------------------------------
# Tests (and core-only command-line tools)
# ---------------------------------------------------------------------------
if(VOXAL_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)

    add_executable(voxal_tests
        tests/TestChain.cpp
        tests/TestOrder.cpp
        tests/TestRealtime.cpp
        tests/TestSynthVoice.cpp
        tests/TestDsp.cpp
        tests/TestShift.cpp
        tests/TestClean.cpp
        tests/TestBreath.cpp
        tests/TestComp.cpp
        tests/TestDeEss.cpp
        tests/TestSmooth.cpp
        tests/TestWarmth.cpp
        tests/TestAir.cpp
        tests/TestRadio.cpp
        tests/TestDoubler.cpp
        tests/TestDelay.cpp
        tests/TestReverb.cpp
        tests/TestPresets.cpp)
    target_include_directories(voxal_tests PRIVATE tests)
    target_link_libraries(voxal_tests PRIVATE voxal_core Catch2::Catch2WithMain)

    # Core-only CLI tools, JUCE-free so they build in build-core.
    # voxal_bench: CPU per rate and block size, worst block during reorders.
    add_executable(voxal_bench tools/Bench.cpp)
    target_link_libraries(voxal_bench PRIVATE voxal_core)

    # voxal_render: a WAV through the chain with any settings (before/after files).
    add_executable(voxal_render tools/Render.cpp)
    target_link_libraries(voxal_render PRIVATE voxal_core)

    include(CTest)
    include(Catch)
    catch_discover_tests(voxal_tests)
endif()
