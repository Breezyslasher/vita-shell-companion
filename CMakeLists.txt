# vita-shell-companion: the SceShell plugin (shell_companion.suprx).
#
#   scripts/fetch_paf_headers.sh build-paf
#   cmake -B build -G Ninja -DSC_PAF_INCLUDE_DIR=$PWD/build-paf/include
#   cmake --build build
#
# The plugin speaks protocol 1 (docs/PROTOCOL.md); client/ is what apps link.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "VITASDK is not set: install vitasdk, or build in vitasdk/vitasdk")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")
endif()

project(shell_companion VERSION 0.3.2 LANGUAGES C CXX ASM)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(SC_PAF_INCLUDE_DIR "" CACHE PATH "PAF headers (scripts/fetch_paf_headers.sh <dir>, then <dir>/include)")
if(NOT SC_PAF_INCLUDE_DIR OR NOT EXISTS "${SC_PAF_INCLUDE_DIR}/paf.h")
    message(FATAL_ERROR "PAF headers not found: run scripts/fetch_paf_headers.sh <dir> "
                        "and configure with -DSC_PAF_INCLUDE_DIR=<dir>/include")
endif()

add_executable(shell_companion
    plugin/main.cpp
    plugin/paf_stubs.S
    plugin/taihen_stub.S)
target_include_directories(shell_companion PRIVATE
    ${SC_PAF_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
# No C runtime, PAF's 16-bit wchar_t, always -O2 (the PAF stubs cover what
# the compiler leaves out of line at -O2), and GCC 15's template-body check
# off for PAF's headers.
target_compile_options(shell_companion PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-O2;-fno-rtti;-fno-exceptions;-fno-builtin;-fshort-wchar;-fno-threadsafe-statics;-fno-sized-deallocation;-Wno-unused-function;-Wno-sign-compare;-Wno-template-body;-D__declspec(x)=>")
target_link_options(shell_companion PRIVATE -nostartfiles -nostdlib)
# What it answers each HELLO with (SC_PLUGIN_INFO): the version as text, and
# as a number to compare, major * 10000 + minor * 100 + patch.
math(EXPR SC_VERSION_CODE
     "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
target_compile_definitions(shell_companion PRIVATE
    SC_PLUGIN_VERSION="${PROJECT_VERSION}" SC_PLUGIN_VERSION_CODE=${SC_VERSION_CODE})
target_link_libraries(shell_companion
    SceLibKernel_stub
    SceAppMgr_stub
    SceIofilemgr_stub
    SceKernelThreadMgr_stub
    SceRtc_stub
    SceSysmodule_stub
    ScePower_stub
    SceSysmem_stub
    gcc)
# With no C runtime nothing runs static constructors: fail the build on any.
add_custom_command(TARGET shell_companion POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DOBJDUMP=${CMAKE_OBJDUMP} -DELF=$<TARGET_FILE:shell_companion>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_no_static_init.cmake
    VERBATIM)
vita_create_self(shell_companion.suprx shell_companion
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/plugin/exports.yml UNSAFE STRIPPED)

# The client library, built here too so that it is known to compile; apps
# usually build client/*.c with their own sources instead. sc_cover.c is left
# out: it needs the app's stb_image.
add_library(shell_companion_client STATIC
    client/shell_companion.c
    client/sc_files.c
    client/sc_link_stub.S)
target_include_directories(shell_companion_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/client)
target_compile_options(shell_companion_client PRIVATE -Wall -Wextra)
