cmake_minimum_required(VERSION 3.25)
project(heziMTP VERSION 1.0.0
    DESCRIPTION "Nintendo Switch MTP Client for macOS"
    LANGUAGES CXX OBJCXX OBJC C
)

# ─── Language standards ───────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ─── macOS targeting ──────────────────────────────────────────────────────────
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")

if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Target CPU architectures" FORCE)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# ─── libusb (built from source, fully static) ─────────────────────────────────
cmake_policy(PUSH)
cmake_policy(SET CMP0169 OLD)  # allow FetchContent_Populate (deprecated in 3.30 but fine)
FetchContent_Declare(libusb_src
    GIT_REPOSITORY https://github.com/libusb/libusb.git
    GIT_TAG        v1.0.27
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(libusb_src)
if(NOT libusb_src_POPULATED)
    FetchContent_Populate(libusb_src)
endif()
cmake_policy(POP)

# Provide a pre-configured config.h for macOS (avoids autoconf requirement)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/libusb_config.h"
    "${libusb_src_SOURCE_DIR}/config.h"
    COPYONLY
)

add_library(usb_static STATIC
    ${libusb_src_SOURCE_DIR}/libusb/core.c
    ${libusb_src_SOURCE_DIR}/libusb/descriptor.c
    ${libusb_src_SOURCE_DIR}/libusb/hotplug.c
    ${libusb_src_SOURCE_DIR}/libusb/io.c
    ${libusb_src_SOURCE_DIR}/libusb/sync.c
    ${libusb_src_SOURCE_DIR}/libusb/strerror.c
    ${libusb_src_SOURCE_DIR}/libusb/os/darwin_usb.c
    ${libusb_src_SOURCE_DIR}/libusb/os/events_posix.c
    ${libusb_src_SOURCE_DIR}/libusb/os/threads_posix.c
)
target_include_directories(usb_static PUBLIC  ${libusb_src_SOURCE_DIR}/libusb)
target_include_directories(usb_static PRIVATE
    ${libusb_src_SOURCE_DIR}          # config.h lives here
    ${libusb_src_SOURCE_DIR}/libusb/os
)
target_compile_definitions(usb_static PRIVATE HAVE_CONFIG_H)
target_compile_options(usb_static PRIVATE -Wno-deprecated-declarations -Wno-error)

find_library(FW_IOKIT          IOKit          REQUIRED)
find_library(FW_COREFOUNDATION CoreFoundation REQUIRED)
find_library(FW_SECURITY       Security       REQUIRED)
target_link_libraries(usb_static PUBLIC
    ${FW_IOKIT} ${FW_COREFOUNDATION} ${FW_SECURITY}
)

# ─── System frameworks ────────────────────────────────────────────────────────
find_library(FW_METAL        Metal        REQUIRED)
find_library(FW_APPKIT       AppKit       REQUIRED)
find_library(FW_FOUNDATION   Foundation   REQUIRED)
find_library(FW_WEBKIT       WebKit       REQUIRED)
find_library(FW_CORETEXT     CoreText     REQUIRED)

# ─── Application ─────────────────────────────────────────────────────────────
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.mm"
)

# webroot resource files — copied into the bundle at Resources/webroot/
file(GLOB WEBROOT_FILES "src/ui/webroot/*")
foreach(WF ${WEBROOT_FILES})
    set_source_files_properties(${WF} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/webroot)
endforeach()

add_executable(heziMTP MACOSX_BUNDLE ${APP_SOURCES} ${WEBROOT_FILES})

target_include_directories(heziMTP PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${libusb_src_SOURCE_DIR}/libusb
)
target_link_libraries(heziMTP PRIVATE
    usb_static
    ${FW_APPKIT} ${FW_FOUNDATION} ${FW_METAL} ${FW_WEBKIT} ${FW_CORETEXT}
)

target_compile_options(heziMTP PRIVATE
    -Wall -Wextra
    $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc -Wpedantic>
)

set_target_properties(heziMTP PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER       "com.hezi.heziMTP"
    MACOSX_BUNDLE_BUNDLE_NAME          "heziMTP"
    MACOSX_BUNDLE_BUNDLE_VERSION       "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_COPYRIGHT            "Copyright © 2025 Hezi"
    MACOSX_BUNDLE_INFO_PLIST           "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
)

# ─── Status ───────────────────────────────────────────────────────────────────
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS "  heziMTP — Nintendo Switch MTP Client")
message(STATUS "  macOS target : ${CMAKE_OSX_DEPLOYMENT_TARGET}+")
message(STATUS "  Architectures: ${CMAKE_OSX_ARCHITECTURES}")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS "Build: cmake -B build && cmake --build build -j$(sysctl -n hw.logicalcpu)")
