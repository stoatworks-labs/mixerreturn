cmake_minimum_required(VERSION 3.22)

# Deliberately a separate build from the plugin at the repo root. This produces a
# CFBundle that coreaudiod loads, not a JUCE target, and it shares no code with the
# plugin — only the idea.
project(MixerReturnDevice VERSION 0.1.0 LANGUAGES C)

if(NOT APPLE)
    message(FATAL_ERROR "The AudioServerPlugIn is macOS-only. Windows is a WDM driver and "
                        "a separate project entirely — see ../AGENTS.md §7.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Universal, for the same reason the plugin is: a host loads whichever slice matches its
# own process, and coreaudiod's architecture is not ours to choose.
if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target")

set(MIXERRETURN_VERSION ${PROJECT_VERSION})
configure_file(Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)

add_library(MixerReturnDriver MODULE src/mr_driver.c src/mr_shared.h)

set_target_properties(MixerReturnDriver PROPERTIES
    OUTPUT_NAME "MixerReturn"
    BUNDLE TRUE
    BUNDLE_EXTENSION "driver"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    # coreaudiod dlopens the bundle and looks the factory up by name, so it has to survive
    # into the symbol table rather than being stripped as unreferenced.
    LINK_FLAGS "-Wl,-exported_symbol,_MixerReturnCreate")

target_compile_options(MixerReturnDriver PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    # The IO path runs on a realtime thread. -ffast-math would be a false economy here;
    # correctness of the sum matters more than a few cycles.
    -O2)

target_link_libraries(MixerReturnDriver PRIVATE
    "-framework CoreAudio" "-framework CoreFoundation")

# The bundle extension has to be .driver, and CMake's BUNDLE_EXTENSION only renames the
# directory — the executable inside still needs to be where Info.plist says it is, which
# the default CFBundle layout already handles.
add_custom_command(TARGET MixerReturnDriver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
        "built $<TARGET_BUNDLE_DIR:MixerReturnDriver> - install needs root, see README.md")
