cmake_minimum_required(VERSION 3.22)

# Static MSVC runtime so the plugin needs no Visual C++ redistributable on the user's PC.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")

project(AIOStudio VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(AIO_BUILD_TESTS "Build the offline DSP / generator test runner" OFF)
option(AIO_COPY_AFTER_BUILD "Copy the VST3 into the system VST3 folder after building" OFF)
set(AIO_JUCE_PATH "" CACHE PATH "Use a local JUCE checkout instead of downloading one")

if(AIO_JUCE_PATH)
    add_subdirectory(${AIO_JUCE_PATH} JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.6
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(AIO_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Presets.cpp
    Source/compose/Composer.cpp
    Source/master/MasterEngine.cpp
    Source/master/MasterSession.cpp
    Source/sing/Sing.cpp)

option(AIO_LTO "Link-time optimisation (slower build, slightly faster plugin)" OFF)

set(AIO_LIBS
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
if(AIO_LTO)
    list(APPEND AIO_LIBS juce::juce_recommended_lto_flags)
endif()

set(AIO_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

# ---------------------------------------------------------------------------
# 1) AIO Studio  - effect plugin. Put it on a mixer insert / master.
#    FX chain + pitch correction process the incoming audio, the Compose engine
#    plays through the same chain from the host transport.
# ---------------------------------------------------------------------------
juce_add_plugin(AIOStudio
    COMPANY_NAME "JC Creative Holdings"
    BUNDLE_ID com.jccreativeholdings.aiostudio
    PLUGIN_MANUFACTURER_CODE JcCr
    PLUGIN_CODE AioF
    FORMATS VST3
    PRODUCT_NAME "AIO Studio"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx
    COPY_PLUGIN_AFTER_BUILD ${AIO_COPY_AFTER_BUILD})
target_sources(AIOStudio PRIVATE ${AIO_SOURCES})
target_include_directories(AIOStudio PRIVATE Source)
target_compile_definitions(AIOStudio PUBLIC ${AIO_DEFS})
target_link_libraries(AIOStudio PRIVATE ${AIO_LIBS})

# ---------------------------------------------------------------------------
# 2) AIO Studio Instrument - generator plugin. Same engine, but it is an
#    instrument so it can OUTPUT MIDI (route the chords/melody to any other
#    synth in FL Studio) and makes sound on its own.
# ---------------------------------------------------------------------------
juce_add_plugin(AIOStudioInstrument
    COMPANY_NAME "JC Creative Holdings"
    BUNDLE_ID com.jccreativeholdings.aiostudioinstrument
    PLUGIN_MANUFACTURER_CODE JcCr
    PLUGIN_CODE AioI
    FORMATS VST3
    PRODUCT_NAME "AIO Studio Instrument"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    VST3_CATEGORIES Instrument Synth
    COPY_PLUGIN_AFTER_BUILD ${AIO_COPY_AFTER_BUILD})
target_sources(AIOStudioInstrument PRIVATE ${AIO_SOURCES})
target_include_directories(AIOStudioInstrument PRIVATE Source)
target_compile_definitions(AIOStudioInstrument PUBLIC ${AIO_DEFS})
target_link_libraries(AIOStudioInstrument PRIVATE ${AIO_LIBS})

# ---------------------------------------------------------------------------
# Offline tests (no audio device needed)
# ---------------------------------------------------------------------------
if(AIO_BUILD_TESTS)
    juce_add_console_app(AIOTests PRODUCT_NAME "AIOTests")
    target_sources(AIOTests PRIVATE tests/Tests.cpp ${AIO_SOURCES})
    target_include_directories(AIOTests PRIVATE Source)
    target_compile_definitions(AIOTests PUBLIC ${AIO_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(AIOTests PRIVATE ${AIO_LIBS})
endif()
