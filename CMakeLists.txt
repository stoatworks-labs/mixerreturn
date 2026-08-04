cmake_minimum_required(VERSION 3.22)

project(MixerReturn VERSION 0.3.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Must be set before any target is created, or a "universal" build silently ships
# arm64-only and the build log will not tell you.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target")

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# --------------------------------------------------------------------------------------
# Build identity for crash reports. A compiled binary cannot read its own git revision at
# runtime, and a report carrying only a version number cannot tell a released build from
# one three commits past the tag. `-dirty` means uncommitted changes were built.
# --------------------------------------------------------------------------------------
find_package(Git QUIET)
set(MIXERRETURN_GIT_REV "unknown")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE MIXERRETURN_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE MIXERRETURN_GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT MIXERRETURN_GIT_REV)
        set(MIXERRETURN_GIT_REV "unknown")
    elseif(MIXERRETURN_GIT_DIRTY)
        set(MIXERRETURN_GIT_REV "${MIXERRETURN_GIT_REV}-dirty")
    endif()
endif()

juce_add_plugin(MixerReturn
    COMPANY_NAME "Allan Sargeant"
    BUNDLE_ID "com.allansargeant.mixerreturn"
    PLUGIN_MANUFACTURER_CODE Alsg
    PLUGIN_CODE Mrt1
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "MixerReturn"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES "Fx" "Tools"
    AU_MAIN_TYPE "kAudioUnitType_Effect"
)

juce_generate_juce_header(MixerReturn)

target_sources(MixerReturn
    PRIVATE
        Source/Diag/Diag.cpp
        Source/DSP/SummingBus.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

target_compile_definitions(MixerReturn
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        DIAG_GIT_REV="${MIXERRETURN_GIT_REV}"
)

target_link_libraries(MixerReturn
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# --------------------------------------------------------------------------------------
# mrtest — headless numerical verification of the summing bus.
#
# Kept in the build rather than thrown away, because the property it checks (a uniform
# one-block delay regardless of instance processing order) is inaudible right up to the
# moment it becomes a comb filter, and no amount of listening will catch a regression.
# --------------------------------------------------------------------------------------
juce_add_console_app(mrtest PRODUCT_NAME "mrtest")
juce_generate_juce_header(mrtest)
target_sources(mrtest PRIVATE tests/mrtest.cpp)
target_compile_definitions(mrtest PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # Needed to pump the message loop so the timer that corrects the reported latency can
    # run — see testLatencyFollowsActualBlockSize.
    JUCE_MODAL_LOOPS_PERMITTED=1
    DIAG_GIT_REV="${MIXERRETURN_GIT_REV}")
target_include_directories(mrtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mrtest PRIVATE MixerReturn juce::juce_audio_utils)

# --------------------------------------------------------------------------------------
# mrshot — renders the editor to PNG for the documentation. Not a screen capture: see the
# comment at the top of tools/mrshot.cpp for why photographing a window is the wrong tool
# for this job.
# --------------------------------------------------------------------------------------
juce_add_console_app(mrshot PRODUCT_NAME "mrshot")
juce_generate_juce_header(mrshot)
target_sources(mrshot PRIVATE tools/mrshot.cpp)
target_compile_definitions(mrshot PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    # Needed to pump the message loop so the editor's timer populates the meters and the
    # member-count readout before the snapshot is taken.
    JUCE_MODAL_LOOPS_PERMITTED=1
    DIAG_GIT_REV="${MIXERRETURN_GIT_REV}")
target_include_directories(mrshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mrshot PRIVATE MixerReturn juce::juce_audio_utils)

# --------------------------------------------------------------------------------------
# mrhost — loads the built .vst3 as a host would and runs the summing check through it.
#
# mrtest constructs the processor directly, so it never crosses the plugin-format boundary.
# The design assumes every instance a host creates from one bundle shares one loaded copy
# of it; if that were false the plugin would silently do nothing, with every instance
# reporting a bus of one. This is the only test that can tell.
# --------------------------------------------------------------------------------------
juce_add_console_app(mrhost PRODUCT_NAME "mrhost")
juce_generate_juce_header(mrhost)
target_sources(mrhost PRIVATE tests/mrhost.cpp)
target_compile_definitions(mrhost PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(mrhost PRIVATE juce::juce_audio_utils juce::juce_audio_processors)
