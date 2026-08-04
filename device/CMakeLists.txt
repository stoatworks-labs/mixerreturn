cmake_minimum_required(VERSION 3.22)

# The AudioServerPlugIn. A separate build from the plugin at the repo root: this produces a
# CFBundle that coreaudiod loads, and shares no code with the plugin — only the idea.
project(MixerReturnDevice VERSION 0.1.0 LANGUAGES CXX)

if(NOT APPLE)
    message(FATAL_ERROR "The AudioServerPlugIn is macOS-only. Windows is a WDM driver and "
                        "a separate project entirely — see ../AGENTS.md §7.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target")

# --------------------------------------------------------------------------------------
# libASPL (MIT) owns the HAL property layer.
#
# The first version of this driver answered the HAL's property requests by hand and never
# loaded — coreaudiod drops a device over a single wrong answer, silently, with nothing in
# the log and nothing in the device list. That layer is large, unforgiving and not where
# this project's value is.
#
# GIT_TAG is pinned rather than following a branch, and tags are fetched because libASPL
# derives its own version from `git describe` — a shallow clone without tags fails to
# configure with a confusing regex error.
# --------------------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(libASPL
    GIT_REPOSITORY https://github.com/gavv/libASPL.git
    GIT_TAG v3.1.2)
FetchContent_MakeAvailable(libASPL)

set(DRIVER_NAME       "MixerReturn")
set(DRIVER_VERSION    "${PROJECT_VERSION}")
set(DRIVER_COPYRIGHT  "MIT licence")
set(DRIVER_IDENTIFIER "com.allansargeant.mixerreturn.driver")
set(DRIVER_ENTRYPOINT "MixerReturnEntryPoint")

# Generated per configure, never hand-written. A factory UUID identifies this plug-in to
# CFPlugIn; reusing one that another bundle on this machine has already claimed makes the
# bundle unloadable, which is indistinguishable from every other silent HAL failure.
if(NOT DRIVER_UID)
    execute_process(COMMAND uuidgen OUTPUT_VARIABLE DRIVER_UID OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(DRIVER_UID "${DRIVER_UID}" CACHE STRING "CFPlugIn factory UUID" FORCE)
endif()
message(STATUS "MixerReturn driver factory UUID: ${DRIVER_UID}")

add_library(${DRIVER_NAME} MODULE src/Driver.cpp)
target_link_libraries(${DRIVER_NAME} PRIVATE libASPL)

find_library(LIB_CoreFoundation CoreFoundation REQUIRED)
find_library(LIB_CoreAudio CoreAudio REQUIRED)
target_link_libraries(${DRIVER_NAME} PRIVATE ${LIB_CoreFoundation} ${LIB_CoreAudio})

# PREFIX/SUFFIX cleared because a MODULE library is otherwise "libMixerReturn.so", and the
# executable name has to match CFBundleExecutable. MACOSX_BUNDLE *and* BUNDLE are both
# needed — setting only BUNDLE was one of the defects in the hand-written version.
set_target_properties(${DRIVER_NAME} PROPERTIES
    OUTPUT_NAME "${DRIVER_NAME}"
    PREFIX "" SUFFIX ""
    BUNDLE true
    MACOSX_BUNDLE ON
    BUNDLE_EXTENSION "driver"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "${DRIVER_NAME}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${DRIVER_VERSION}"
    MACOSX_BUNDLE_COPYRIGHT "${DRIVER_COPYRIGHT}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${DRIVER_IDENTIFIER}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${DRIVER_VERSION}")

# Signing is not optional: coreaudiod will not load an unsigned HAL plugin, and says so by
# simply not showing the device. Developer ID on this Mac, ad-hoc anywhere else.
set(MR_SIGN_IDENTITY "-" CACHE STRING "codesign identity for the driver bundle")
if(EXISTS "$ENV{HOME}/.config/stoatworks/release-signing.env" AND MR_SIGN_IDENTITY STREQUAL "-")
    file(STRINGS "$ENV{HOME}/.config/stoatworks/release-signing.env" _mr_env
         REGEX "^RL_MAC_SIGN_IDENTITY=")
    if(_mr_env)
        string(REGEX REPLACE "^RL_MAC_SIGN_IDENTITY=\"?([^\"]*)\"?$" "\\1" MR_SIGN_IDENTITY "${_mr_env}")
    endif()
endif()

add_custom_command(TARGET ${DRIVER_NAME} POST_BUILD
    COMMAND codesign --force --sign "${MR_SIGN_IDENTITY}" --timestamp
            "$<TARGET_BUNDLE_DIR:${DRIVER_NAME}>"
    COMMAND codesign --verify --strict "$<TARGET_BUNDLE_DIR:${DRIVER_NAME}>"
    # VERBATIM: the identity contains spaces and parentheses, and without it the shell
    # reads the brackets as syntax and leaves the bundle unsigned.
    VERBATIM)
