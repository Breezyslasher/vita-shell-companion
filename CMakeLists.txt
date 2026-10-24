# vita-shell-companion: the SceShell plugin (shell_companion.suprx).
#
#   scripts/fetch_paf_headers.sh build-paf
#   cmake -B build -G Ninja -DSC_PAF_INCLUDE_DIR=$PWD/build-paf/include
#   cmake --build build
#
# Starting point: VitaPlex's quick menu plugin (protocol 17), moved here as it
# is. docs/PROTOCOL.md describes the shared protocol it is being turned into;
# until then it still serves VitaPlex only (SC_LEGACY_TITLEID).
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "VITASDK is not set: install vitasdk, or build in vitasdk/vitasdk")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")
endif()

project(shell_companion LANGUAGES C CXX ASM)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(SC_PAF_INCLUDE_DIR "" CACHE PATH "PAF headers (scripts/fetch_paf_headers.sh <dir>, then <dir>/include)")
if(NOT SC_PAF_INCLUDE_DIR OR NOT EXISTS "${SC_PAF_INCLUDE_DIR}/paf.h")
    message(FATAL_ERROR "PAF headers not found: run scripts/fetch_paf_headers.sh <dir> "
                        "and configure with -DSC_PAF_INCLUDE_DIR=<dir>/include")
endif()

# The app the VitaPlex-era code still serves (updates, notification icon).
set(SC_LEGACY_TITLEID "VPLEX0001" CACHE STRING "title the VitaPlex-era code serves")

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
target_compile_definitions(shell_companion PRIVATE VITAPLEX_TITLEID=\"${SC_LEGACY_TITLEID}\")
target_link_libraries(shell_companion
    SceLibKernel_stub
    SceAppMgr_stub
    SceIofilemgr_stub
    SceKernelThreadMgr_stub
    SceRtc_stub
    SceSysmodule_stub
    gcc)
# With no C runtime nothing runs static constructors: fail the build on any.
add_custom_command(TARGET shell_companion POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DOBJDUMP=${CMAKE_OBJDUMP} -DELF=$<TARGET_FILE:shell_companion>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_no_static_init.cmake
    VERBATIM)
vita_create_self(shell_companion.suprx shell_companion
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/plugin/exports.yml UNSAFE STRIPPED)
