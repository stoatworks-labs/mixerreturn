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

# --------------------------------------------------------------------------------------
# Signing, which is not optional here.
#
# coreaudiod will not load an unsigned HAL plugin, and — this is the part that costs an
# afternoon — it fails *silently*: the driver simply never appears in the device list, with
# nothing in the log to say why. The linker's own ad-hoc signature on the executable is not
# enough; `codesign -v` on the bundle reports "code object is not signed at all", because
# the bundle wrapper is a separate thing from the Mach-O inside it.
#
# Identity comes from the machine-local release config if it is there, so a dev box signs
# with Developer ID and CI falls back to ad-hoc rather than failing to configure.
# --------------------------------------------------------------------------------------
set(MR_SIGN_IDENTITY "-" CACHE STRING "codesign identity for the driver bundle")
if(EXISTS "$ENV{HOME}/.config/stoatworks/release-signing.env" AND MR_SIGN_IDENTITY STREQUAL "-")
    file(STRINGS "$ENV{HOME}/.config/stoatworks/release-signing.env" _mr_env
         REGEX "^RL_MAC_SIGN_IDENTITY=")
    if(_mr_env)
        string(REGEX REPLACE "^RL_MAC_SIGN_IDENTITY=\"?([^\"]*)\"?$" "\\1" MR_SIGN_IDENTITY "${_mr_env}")
    endif()
endif()

add_custom_command(TARGET MixerReturnDriver POST_BUILD
    # Rewrite the Info.plist through plutil before signing.
    #
    # Not cosmetic, and not yet fully explained. The plist CMake writes passes `plutil
    # -lint`, parses correctly, and has exactly the keys a working driver has -- and
    # CFBundleCreate still refuses to open the bundle, which means coreaudiod cannot look
    # at it and the device never appears. Rewriting the same content through plutil
    # produces a bundle that opens. Converting to binary1 is a real rewrite (unlike
    # `-convert xml1` on a file that is already xml1, which is a no-op), and binary is the
    # normal form for a plist inside a bundle anyway.
    COMMAND plutil -convert binary1
            "$<TARGET_BUNDLE_DIR:MixerReturnDriver>/Contents/Info.plist"
    COMMAND codesign --force --sign "${MR_SIGN_IDENTITY}" --timestamp --options runtime
            "$<TARGET_BUNDLE_DIR:MixerReturnDriver>"
    # Verify rather than trust the exit code above: a signature that codesign writes
    # happily can still fail to validate, and an invalid one loads exactly as well as no
    # signature at all, which is to say not at all and without saying so.
    COMMAND codesign --verify --strict "$<TARGET_BUNDLE_DIR:MixerReturnDriver>"
    # Then prove the result is loadable, rather than finding out via a root install and a
    # coreaudiod restart that ends in an empty device list. mrprobe does what coreaudiod
    # does -- CFBundle, CFPlugIn instance, QueryInterface, Initialize -- and exits non-zero
    # on the first step that fails, which fails the build.
    COMMAND "$<TARGET_FILE:mrprobe>" "$<TARGET_BUNDLE_DIR:MixerReturnDriver>"
    COMMAND ${CMAKE_COMMAND} -E echo
        "signed and probed OK - install needs root, see README.md"
    # VERBATIM, because the identity contains spaces and parentheses -- "Developer ID
    # Application: NAME (TEAMID)" -- and without it CMake hands the bare string to the
    # shell, which reads the brackets as syntax and leaves the bundle unsigned.
    VERBATIM)

# --------------------------------------------------------------------------------------
# mrprobe — loads the driver the way coreaudiod does, in a process that can be debugged.
#
# Kept in the build because the failure it catches is completely silent: a HAL plugin that
# coreaudiod will not load produces no log line and no device, which is indistinguishable
# from a driver that loaded and published nothing. This separates the two in a second,
# with no root install and no coreaudiod restart.
# --------------------------------------------------------------------------------------
add_executable(mrprobe tools/mrprobe.c)
target_link_libraries(mrprobe PRIVATE "-framework CoreFoundation" "-framework CoreAudio")
add_dependencies(MixerReturnDriver mrprobe)
