#
#
#    SysmonForLinux
#
#    Copyright (c) Microsoft Corporation
#
#    All rights reserved.
#
#    MIT License
#
#    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the ""Software""), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
#
#    The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
#
#    THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
#

#################################################################################
#
# CMakeLists.txt
#
# Build script
#
#################################################################################

#################################################################################
# NOTE:                                                                         #
# Please compile in a build directory rather than the source directory.         #
# From Sysmon/linux:                                                            #
#                                                                               #
#    $ mkdir build; cd build                                                    #
#    $ cmake ..                                                                 #
#    $ make                                                                     #
#                                                                               #
# If you later change this CMakeLists.txt file, remake the build directory.     #
#                                                                               #
#    $ cd ..                                                                    #
#    $ rm -rf build                                                             #
#    $ mkdir build; cd build                                                    #
#    $ cmake ..                                                                 #
#    $ make                                                                     #
#                                                                               #
#################################################################################

cmake_minimum_required(VERSION 3.10)
cmake_policy(SET CMP0048 NEW)

#
# set the project name - version is MAJOR.MINOR.PATCH.RELEASE - releases start at 1
#
if (DEFINED ENV{VERSION})
  project(SysmonForLinux VERSION $ENV{VERSION})
else()
  project(SysmonForLinux VERSION 0.0.0.0)
endif()

if ("${PROJECT_VERSION_TWEAK}" STREQUAL "")
  set(PROJECT_VERSION_TWEAK 0)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(SYSMON_TARGET_ARCH "aarch64")
    set(BPF_TARGET_ARCH "arm64")
    set(DEB_ARCH "arm64")
    set(RPM_ARCH "aarch64")
    set(OPENSSL_TARGET "linux-aarch64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(SYSMON_TARGET_ARCH "x86_64")
    set(BPF_TARGET_ARCH "x86")
    set(DEB_ARCH "amd64")
    set(RPM_ARCH "x86_64")
    set(OPENSSL_TARGET "linux-x86_64")
else()
    message(FATAL_ERROR "Unsupported target architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -print-multiarch
    OUTPUT_VARIABLE TARGET_MULTIARCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE TARGET_MULTIARCH_RESULT
    ERROR_QUIET
)
# Debian-based toolchains use a multiarch include directory. Native RPM-based
# toolchains may return an empty tuple and use /usr/include directly instead.
# Keep this nonempty because the eBPF compile command passes it to -isystem.
set(TARGET_MULTIARCH_INCLUDE "/usr/include")
if(TARGET_MULTIARCH_RESULT EQUAL 0 AND TARGET_MULTIARCH AND
        IS_DIRECTORY "/usr/include/${TARGET_MULTIARCH}")
    set(TARGET_MULTIARCH_INCLUDE "/usr/include/${TARGET_MULTIARCH}")
elseif(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
        "Failed to determine the target multiarch include directory for cross-compilation")
endif()
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -print-file-name=include
    OUTPUT_VARIABLE TARGET_COMPILER_INCLUDE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE TARGET_COMPILER_INCLUDE_RESULT
)
if(NOT TARGET_COMPILER_INCLUDE_RESULT EQUAL 0 OR
        NOT TARGET_COMPILER_INCLUDE OR
        TARGET_COMPILER_INCLUDE STREQUAL "include")
    message(FATAL_ERROR "Failed to determine the target compiler include directory")
endif()

set(SYSINTERNALS_EBPF_ROOT "/opt/sysinternalsEBPF" CACHE PATH
    "Path containing the target SysinternalsEBPF eBPF resources")
set(SYSINTERNALS_EBPF_INCLUDE_DIR "/usr/local/include" CACHE PATH
    "Path containing the target libsysinternalsEBPF.h")
set(SYSINTERNALS_EBPF_LIBRARY "" CACHE FILEPATH
    "Target libsysinternalsEBPF shared library")

if(NOT SYSINTERNALS_EBPF_LIBRARY)
    find_library(DETECTED_SYSINTERNALS_EBPF_LIBRARY
        NAMES sysinternalsEBPF
        PATHS /usr/local/lib /usr/lib
    )
    if(NOT DETECTED_SYSINTERNALS_EBPF_LIBRARY)
        message(FATAL_ERROR
            "libsysinternalsEBPF was not found; set SYSINTERNALS_EBPF_LIBRARY")
    endif()
    set(SYSINTERNALS_EBPF_LIBRARY "${DETECTED_SYSINTERNALS_EBPF_LIBRARY}")
endif()

set(SYSINTERNALS_EBPF_KERN_DIR "${SYSINTERNALS_EBPF_ROOT}/ebpfKern")
set(SYSINTERNALS_EBPF_LIBBPF_DIR "${SYSINTERNALS_EBPF_ROOT}/libbpf")

configure_file(linuxVersion.h.in linuxVersion.h)
configure_file(package/DEBIAN.in/control.in DEBIANcontrol)
configure_file(package/SPECS.in/spec.in SPECS.spec)

#
# enable Debug while pre-release; re-enable it post-release to add symbols to binary
#
#set(CMAKE_BUILD_TYPE Debug)
#option(DEBUG_K "Enter debug mode" On)

#
# external programs used by this build
#
set(TEXTTRANSFORM "$ENV{HOME}/.dotnet/tools/t4")
set(ICONV "/usr/bin/iconv")
set(LD "${CMAKE_LINKER}")

#
# package name
#
set(PACKAGE_NAME "sysmonforlinux")

#
# report warnings as errors
# -g is required for BTF and CO:RE
#
add_compile_options(-Wall -Werror -g -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fstack-protector-all -O2)

#
# support for C++17
#
set(CMAKE_CXX_FLAGS "-std=gnu++17 -Wno-write-strings")

#
# pugixml - vendored lightweight XML library (replaces libxml2)
#
set(PUGIXML_SOURCE_DIR "${CMAKE_SOURCE_DIR}/pugixml")

#
# source directories
#
if(SYSMON_ADO)
set(SYSMON_COMMON_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../sysmonCommon/")
set(SYSMON_TESTS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../test/UnitTests/")
else()
set(SYSMON_COMMON_SOURCE_DIR "${CMAKE_SOURCE_DIR}/sysmonCommon/")
set(SYSMON_TESTS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/sysmonCommon/UnitTests/")
endif()

#
# Compress man page
#
set(SYSMON_COMPRESS_MAN "sysmon.8.gz")

execute_process(
    COMMAND date "+%d %b %Y"
    OUTPUT_VARIABLE BUILD_DATE
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

configure_file(
    "${CMAKE_SOURCE_DIR}/package/usr/share/man/man8/sysmon_template.8"
    "${PROJECT_BINARY_DIR}/sysmon.8"
    @ONLY
)

add_custom_target(sysmonManPageCompress ALL
                  DEPENDS ${PROJECT_BINARY_DIR}/${SYSMON_COMPRESS_MAN}
                  )

add_custom_command(OUTPUT ${PROJECT_BINARY_DIR}/${SYSMON_COMPRESS_MAN}
                   COMMAND gzip -f -c "${PROJECT_BINARY_DIR}/sysmon.8" > ${PROJECT_BINARY_DIR}/${SYSMON_COMPRESS_MAN}
                   COMMENT "Compressing Sysmon man page"
                   DEPENDS "${PROJECT_BINARY_DIR}/sysmon.8"
                  )

include(ExternalProject)


# Get and build OpenSSL.
# When you update the GIT tag please make sure to delete the build
# directory before building again.
set(OPENSSL_CROSS_COMPILE "" CACHE STRING
    "OpenSSL cross-compiler prefix, for example aarch64-linux-gnu-")

if(CMAKE_CROSSCOMPILING)
    if(NOT OPENSSL_CROSS_COMPILE)
        message(FATAL_ERROR
            "OPENSSL_CROSS_COMPILE must be set when cross-compiling")
    endif()
    set(OPENSSL_CONFIGURE_COMMAND
        "${CMAKE_COMMAND}" -E env
        "CROSS_COMPILE=${OPENSSL_CROSS_COMPILE}"
        <SOURCE_DIR>/Configure "${OPENSSL_TARGET}")
else()
    set(OPENSSL_CONFIGURE_COMMAND <SOURCE_DIR>/Configure "${OPENSSL_TARGET}")
endif()

find_program(OPENSSL_MAKE NAMES gmake make)
if(NOT OPENSSL_MAKE)
    message(FATAL_ERROR "GNU Make is required to build OpenSSL")
endif()

ExternalProject_Add(openssl
                    GIT_REPOSITORY https://github.com/openssl/openssl.git
                    GIT_TAG openssl-3.6.2
                    PREFIX ./openssl
                    BUILD_IN_SOURCE TRUE
                    CONFIGURE_COMMAND ${OPENSSL_CONFIGURE_COMMAND}
                    BUILD_COMMAND "${OPENSSL_MAKE}"
                    COMMAND "${CMAKE_COMMAND}" -E remove_directory
                        <SOURCE_DIR>/cloudflare-quiche
                    COMMAND "${CMAKE_COMMAND}" -E remove_directory
                        <SOURCE_DIR>/pyca-cryptography
                    BUILD_BYPRODUCTS <SOURCE_DIR>/libcrypto.a
                    INSTALL_COMMAND ""
                    UPDATE_COMMAND ""
                   )

set(openssl_SOURCE_DIR ${CMAKE_BINARY_DIR}/openssl/src/openssl)

#
# make sysmon
#
add_executable(sysmon
               sysmonforlinux.c
               linuxWideChar.c
               linuxHelpers.cpp
               networkTracker.cpp
               outputxml.cpp
               sysmonevents_defs.c
               ${PUGIXML_SOURCE_DIR}/pugixml.cpp
               installer.c
               "${SYSMON_COMMON_SOURCE_DIR}/usage.c"
               "${SYSMON_COMMON_SOURCE_DIR}/parsecommandline.c"
               "${SYSMON_COMMON_SOURCE_DIR}/printSchema.c"
               "${SYSMON_COMMON_SOURCE_DIR}/xml.cpp"
               "${SYSMON_COMMON_SOURCE_DIR}/rules.c"
               "${SYSMON_COMMON_SOURCE_DIR}/eventsCommon.cpp"
               "${SYSMON_COMMON_SOURCE_DIR}/networkCommon.cpp"
               "${SYSMON_COMMON_SOURCE_DIR}/networkmiscCommon.cpp"
               "${SYSMON_COMMON_SOURCE_DIR}/dumpConfiguration.c"
               sysmonevents.h           # automatically generated during build
               sysmonmsg.h              # automatically generated during build
               sysmonmsgop.c            # automatically generated during build
               sysmonmsgop.h            # automatically generated during build
               # following are all embedded in sysmon as objects
               manifest.xml.o
               sysmonEBPFkern4.15.o.o
               sysmonEBPFkern4.16.o.o
               sysmonEBPFkern4.17-5.1.o.o
               sysmonEBPFkern5.2.o.o
               sysmonEBPFkern5.3-5.5.o.o
               sysmonEBPFkern5.6-.o.o
               sysmonEBPFkern4.15_core.o.o
               sysmonEBPFkern4.16_core.o.o
               sysmonEBPFkern4.17-5.1_core.o.o
               sysmonEBPFkern5.2_core.o.o
               sysmonEBPFkern5.3-5.5_core.o.o
               sysmonEBPFkern5.6-_core.o.o
               sysmonLogView.o
               sysmon.d.o
               sysmon.service.o
               sysmonEBPFkern4.15.rep
               sysmonEBPFkern4.16.rep
               sysmonEBPFkern4.17-5.1.rep
               sysmonEBPFkern5.2.rep
               sysmonEBPFkern5.3-5.5.rep
               sysmonEBPFkern5.6-.rep
               sysmonEBPFkern4.15_core.rep
               sysmonEBPFkern4.16_core.rep
               sysmonEBPFkern4.17-5.1_core.rep
               sysmonEBPFkern5.2_core.rep
               sysmonEBPFkern5.3-5.5_core.rep
               sysmonEBPFkern5.6-_core.rep
               )

target_include_directories(sysmon PUBLIC
                           "${CMAKE_SOURCE_DIR}"
                           "${SYSMON_COMMON_SOURCE_DIR}"
                           "${PROJECT_BINARY_DIR}"
                           "/usr/include"
                           "${TARGET_MULTIARCH_INCLUDE}"
                           "${SYSINTERNALS_EBPF_INCLUDE_DIR}"
                           "${PUGIXML_SOURCE_DIR}"
                           "${SYSINTERNALS_EBPF_KERN_DIR}"
                           "${openssl_SOURCE_DIR}/include"
                          )


add_custom_target(deb
    COMMAND "${CMAKE_SOURCE_DIR}/makePackages.sh" "${CMAKE_SOURCE_DIR}" "${PROJECT_BINARY_DIR}" "${PACKAGE_NAME}" "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}" "0" "deb" "${DEB_ARCH}" "${RPM_ARCH}"
    DEPENDS "${CMAKE_SOURCE_DIR}/package" "${PROJECT_BINARY_DIR}/sysmon"
)

add_custom_target(rpm
    COMMAND "${CMAKE_SOURCE_DIR}/makePackages.sh" "${CMAKE_SOURCE_DIR}" "${PROJECT_BINARY_DIR}" "${PACKAGE_NAME}" "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}" "0" "rpm" "${DEB_ARCH}" "${RPM_ARCH}"
    DEPENDS "${CMAKE_SOURCE_DIR}/package" "${PROJECT_BINARY_DIR}/sysmon"
)

add_dependencies(sysmon openssl)

set(SYSMON_HOST_CHECK_EBPF_SIZES "" CACHE FILEPATH
    "Native checkEBPFsizes executable to use while cross-compiling")
if(CMAKE_CROSSCOMPILING)
    if(NOT EXISTS "${SYSMON_HOST_CHECK_EBPF_SIZES}")
        message(FATAL_ERROR
            "Set SYSMON_HOST_CHECK_EBPF_SIZES to a native executable")
    endif()
    set(CHECK_EBPF_SIZES "${SYSMON_HOST_CHECK_EBPF_SIZES}")
    set(CHECK_EBPF_SIZES_DEPENDS "${SYSMON_HOST_CHECK_EBPF_SIZES}")
else()
    set(CHECK_EBPF_SIZES "$<TARGET_FILE:checkEBPFsizes>")
    set(CHECK_EBPF_SIZES_DEPENDS checkEBPFsizes)
endif()

# list of ebpf programs to make
set(EBPF_PROGS
               sysmonEBPFkern4.15
               sysmonEBPFkern4.16
               sysmonEBPFkern4.17-5.1
               sysmonEBPFkern5.2
               sysmonEBPFkern5.3-5.5
               sysmonEBPFkern5.6-
)

add_custom_command(OUTPUT sysmonEBPFkern4.15.rep
                   COMMAND "${CHECK_EBPF_SIZES}" sysmonEBPFkern4.15.o 4096 && touch sysmonEBPFkern4.15.rep
                   COMMENT "Checking sysmonEBPFkern4.15.o"
                   DEPENDS ${CHECK_EBPF_SIZES_DEPENDS} sysmonEBPFkern4.15.o
                  )

add_custom_command(OUTPUT sysmonEBPFkern4.16.rep
                   COMMAND "${CHECK_EBPF_SIZES}" sysmonEBPFkern4.16.o 4096 && touch sysmonEBPFkern4.16.rep
                   COMMENT "Checking sysmonEBPFkern4.16.o"
                   DEPENDS ${CHECK_EBPF_SIZES_DEPENDS} sysmonEBPFkern4.16.o
                 )

add_custom_command(OUTPUT sysmonEBPFkern4.17-5.1.rep
                   COMMAND "${CHECK_EBPF_SIZES}" sysmonEBPFkern4.17-5.1.o 4096 && touch sysmonEBPFkern4.17-5.1.rep
                   COMMENT "Checking sysmonEBPFkern4.17-5.1.o"
                   DEPENDS ${CHECK_EBPF_SIZES_DEPENDS} sysmonEBPFkern4.17-5.1.o
                  )

add_custom_command(OUTPUT sysmonEBPFkern5.2.rep
                   COMMAND "${CHECK_EBPF_SIZES}" sysmonEBPFkern5.2.o 32768 && touch sysmonEBPFkern5.2.rep
                   COMMENT "Checking sysmonEBPFkern5.2.o"
                   DEPENDS ${CHECK_EBPF_SIZES_DEPENDS} sysmonEBPFkern5.2.o
                  )

add_custom_command(OUTPUT sysmonEBPFkern5.3-5.5.rep
                   COMMAND "${CHECK_EBPF_SIZES}" sysmonEBPFkern5.3-5.5.o 32768 && touch sysmonEBPFkern5.3-5.5.rep
                   COMMENT "Checking sysmonEBPFkern5.3-5.5.o"
                   DEPENDS ${CHECK_EBPF_SIZES_DEPENDS} sysmonEBPFkern5.3-5.5.o
                  )

add_custom_command(OUTPUT sysmonEBPFkern5.6-.rep
                   COMMAND "${CHECK_EBPF_SIZES}" sysmonEBPFkern5.6-.o 32768 && touch sysmonEBPFkern5.6-.rep
                   COMMENT "Checking sysmonEBPFkern5.6-.o"
                   DEPENDS ${CHECK_EBPF_SIZES_DEPENDS} sysmonEBPFkern5.6-.o
                  )

add_custom_command(OUTPUT sysmonEBPFkern4.15_core.rep
                   COMMAND "${CHECK_EBPF_SIZES}" sysmonEBPFkern4.15_core.o 4096 && touch sysmonEBPFkern4.15_core.rep
                   COMMENT "Checking sysmonEBPFkern4.15_core.o"
                   DEPENDS ${CHECK_EBPF_SIZES_DEPENDS} sysmonEBPFkern4.15_core.o
                  )

add_custom_command(OUTPUT sysmonEBPFkern4.16_core.rep
                   COMMAND "${CHECK_EBPF_SIZES}" sysmonEBPFkern4.16_core.o 4096 && touch sysmonEBPFkern4.16_core.rep
                   COMMENT "Checking sysmonEBPFkern4.16_core.o"
                   DEPENDS ${CHECK_EBPF_SIZES_DEPENDS} sysmonEBPFkern4.16_core.o
                  )

add_custom_command(OUTPUT sysmonEBPFkern4.17-5.1_core.rep
                   COMMAND "${CHECK_EBPF_SIZES}" sysmonEBPFkern4.17-5.1_core.o 4096 && touch sysmonEBPFkern4.17-5.1_core.rep
                   COMMENT "Checking sysmonEBPFkern4.17-5.1_core.o"
                   DEPENDS ${CHECK_EBPF_SIZES_DEPENDS} sysmonEBPFkern4.17-5.1_core.o
                  )

add_custom_command(OUTPUT sysmonEBPFkern5.2_core.rep
                   COMMAND "${CHECK_EBPF_SIZES}" sysmonEBPFkern5.2_core.o 32768 && touch sysmonEBPFkern5.2_core.rep
                   COMMENT "Checking sysmonEBPFkern5.2_core.o"
                   DEPENDS ${CHECK_EBPF_SIZES_DEPENDS} sysmonEBPFkern5.2_core.o
                  )

add_custom_command(OUTPUT sysmonEBPFkern5.3-5.5_core.rep
                   COMMAND "${CHECK_EBPF_SIZES}" sysmonEBPFkern5.3-5.5_core.o 32768 && touch sysmonEBPFkern5.3-5.5_core.rep
                   COMMENT "Checking sysmonEBPFkern5.3-5.5_core.o"
                   DEPENDS ${CHECK_EBPF_SIZES_DEPENDS} sysmonEBPFkern5.3-5.5_core.o
                  )

add_custom_command(OUTPUT sysmonEBPFkern5.6-_core.rep
                   COMMAND "${CHECK_EBPF_SIZES}" sysmonEBPFkern5.6-_core.o 32768 && touch sysmonEBPFkern5.6-_core.rep
                   COMMENT "Checking sysmonEBPFkern5.6-_core.o"
                   DEPENDS ${CHECK_EBPF_SIZES_DEPENDS} sysmonEBPFkern5.6-_core.o
                  )

# list of files the EBPF programs depend upon
set(EBPF_DEPENDS
               sysmon_defs.h
               linuxTypes.h
               "${SYSMON_COMMON_SOURCE_DIR}/ioctlcmd.h"
               "${SYSINTERNALS_EBPF_KERN_DIR}/sysinternalsEBPF_helpers.c"
               "${SYSINTERNALS_EBPF_KERN_DIR}/sysinternalsEBPF_common.h"
               "${SYSINTERNALS_EBPF_KERN_DIR}/sysinternalsEBPFoffsets.h"
               "${SYSINTERNALS_EBPF_KERN_DIR}/sysinternalsEBPFshared.h"
               ebpfKern/sysmonEBPF_common.h
               ebpfKern/sysmonGenericEntry_tp.c
               ebpfKern/sysmonGenericEntry_rawtp.c
               ebpfKern/sysmonHelpers.c
               ebpfKern/sysmonProcCreate.c
               ebpfKern/sysmonProcCreate_tp.c
               ebpfKern/sysmonProcCreate_rawtp.c
               ebpfKern/sysmonFileCreate.c
               ebpfKern/sysmonFileCreate_tp.c
               ebpfKern/sysmonFileCreate_rawtp.c
               ebpfKern/sysmonFileOpen.c
               ebpfKern/sysmonFileOpen_tp.c
               ebpfKern/sysmonFileOpen_rawtp.c
               ebpfKern/sysmonFileDelete.c
               ebpfKern/sysmonFileDelete_tp.c
               ebpfKern/sysmonFileDelete_rawtp.c
               ebpfKern/sysmonFileDeleteAt.c
               ebpfKern/sysmonFileDeleteAt_tp.c
               ebpfKern/sysmonFileDeleteAt_rawtp.c
               ebpfKern/sysmonFileDeleteAtCwd.c
               ebpfKern/sysmonFileDeleteAtCwd_tp.c
               ebpfKern/sysmonFileDeleteAtCwd_rawtp.c
               ebpfKern/sysmonProcTerminated.c
               ebpfKern/sysmonTCPconnection_4_15.c
               ebpfKern/sysmonTCPconnection_4_16_5_5.c
               ebpfKern/sysmonTCPconnection_5_6_.c
               ebpfKern/sysmonTCPaccept.c
               ebpfKern/sysmonTCPaccept_tp.c
               ebpfKern/sysmonTCPaccept_rawtp.c
               ebpfKern/sysmonProcAccessed.c
               ebpfKern/sysmonProcAccessed_tp.c
               ebpfKern/sysmonProcAccessed_rawtp.c
               ebpfKern/sysmonUDPsend.c
               ebpfKern/sysmonUDPrecv.c
               ebpfKern/sysmonUDPrecv_tp.c
               ebpfKern/sysmonUDPrecv_rawtp.c
               ebpfKern/sysmonCloseFD.c
               ebpfKern/sysmonCloseFD_tp.c
               ebpfKern/sysmonCloseFD_rawtp.c
               sysmonevents.h   # automatically generated during build
               sysmonmsg.h      # automatically generated during build
               sysmonmsgop.c    # automatically generated during build
               sysmonmsgop.h    # automatically generated during build
)


#
# link sysmon
#
target_link_libraries(sysmon "${SYSINTERNALS_EBPF_LIBRARY}" pthread ${openssl_SOURCE_DIR}/libcrypto.a ${CMAKE_DL_LIBS} )


#
# make sysmonLogView
#
add_executable(sysmonLogView
               sysmonLogView/sysmonLogView.cpp
               sysmonLogView/sysmonGetEventName.c
               ${PUGIXML_SOURCE_DIR}/pugixml.cpp
               sysmonmsgop.c
               sysmonevents.h
               sysmonmsg.h
               sysmonmsgop.h
)

target_include_directories(sysmonLogView PUBLIC
                           "${CMAKE_SOURCE_DIR}"
                           "${SYSMON_COMMON_SOURCE_DIR}"
                           "${PROJECT_BINARY_DIR}"
                           "/usr/include"
                           "${TARGET_MULTIARCH_INCLUDE}"
                           "${PUGIXML_SOURCE_DIR}"
                           )

target_link_libraries(sysmonLogView )


#
# make checkEBPFsizes
#
add_executable(checkEBPFsizes
               checkEBPFsizes/checkEBPFsizes.c
)

target_include_directories(checkEBPFsizes PUBLIC
                           "/usr/include"
                           "${TARGET_MULTIARCH_INCLUDE}"
                           "${SYSINTERNALS_EBPF_INCLUDE_DIR}"
                           )

target_link_libraries(checkEBPFsizes "${SYSINTERNALS_EBPF_LIBRARY}")

#
# GTest required for unit tests
#
find_package(GTest REQUIRED)

#
# make sysmon unit tests
#
add_executable(sysmonUnitTests
               "${SYSMON_TESTS_SOURCE_DIR}/main.cpp"
               "${SYSMON_TESTS_SOURCE_DIR}/rules.cpp"
               "${SYSMON_TESTS_SOURCE_DIR}/utils.cpp"
               "${SYSMON_TESTS_SOURCE_DIR}/RuleEntry.cpp"
               test/linuxRules.cpp
               linuxWideChar.c
               linuxHelpers.cpp
               outputxml.cpp
               sysmonevents_defs.c
               ${PUGIXML_SOURCE_DIR}/pugixml.cpp
               hexdump.c
               "${SYSMON_COMMON_SOURCE_DIR}/parsecommandline.c"
               "${SYSMON_COMMON_SOURCE_DIR}/xml.cpp"
               "${SYSMON_COMMON_SOURCE_DIR}/rules.c"
               "${SYSMON_COMMON_SOURCE_DIR}/eventsCommon.cpp"
               sysmonevents.h
               sysmonmsg.h
               sysmonmsgop.h
               sysmonmsgop.c
               "${PROJECT_BINARY_DIR}/testdata"
               "${PROJECT_BINARY_DIR}/yoursleep"
               )

target_link_libraries(sysmonUnitTests ${GTEST_LIBRARIES} pthread ${openssl_SOURCE_DIR}/libcrypto.a ${CMAKE_DL_LIBS})
add_dependencies(sysmonUnitTests openssl)
target_include_directories(sysmonUnitTests PUBLIC
                           "${CMAKE_SOURCE_DIR}"
                           "${SYSMON_COMMON_SOURCE_DIR}"
                           "${SYSMON_TESTS_SOURCE_DIR}"
                           "${PROJECT_BINARY_DIR}"
                           "/usr/include"
                           "${TARGET_MULTIARCH_INCLUDE}"
                           "${PUGIXML_SOURCE_DIR}"
                           "${GTEST_INCLUDE_DIRS}"
                           "${openssl_SOURCE_DIR}/include"
                           )

#
# copy test data
#
add_custom_command(OUTPUT "${PROJECT_BINARY_DIR}/testdata"
                   COMMAND cp -a "${SYSMON_TESTS_SOURCE_DIR}/data" "${PROJECT_BINARY_DIR}/testdata"
                   COMMENT "Copying data to testdata"
                   DEPENDS "${SYSMON_TESTS_SOURCE_DIR}/data"
                   )

#
# make mysleep test binary
#
add_executable(mysleep test/mysleep.c)
add_custom_command(OUTPUT yoursleep
                   COMMAND ln -s mysleep yoursleep
                   DEPENDS mysleep
                  )


#
# automatically generate sources from manifest.xml and manifest.tt
#
add_custom_command(OUTPUT sysmonevents.h.utf16
                   COMMAND "${TEXTTRANSFORM}" "${SYSMON_COMMON_SOURCE_DIR}/manifest.tt" -out sysmonevents.h.utf16 -a '!!type!header'
                   COMMENT "Extracting sysmonevents.h.utf16"
                   DEPENDS "${SYSMON_COMMON_SOURCE_DIR}/manifest.tt"
                   )

add_custom_command(OUTPUT sysmonevents.h
                   COMMAND "${ICONV}" -f ASCII -t UTF-8 sysmonevents.h.utf16 -o sysmonevents.h
                   COMMENT "Converting sysmonevents.h.utf16 to UTF8"
                   DEPENDS sysmonevents.h.utf16
                   )

add_custom_command(OUTPUT sysmonmsg.mc.utf16
                   COMMAND "${TEXTTRANSFORM}" "${SYSMON_COMMON_SOURCE_DIR}/manifest.tt" -out sysmonmsg.mc.utf16 -a '!!version!internal' -a '!!type!mc'
                   COMMENT "Extracting sysmonmsg.mc.utf16"
                   DEPENDS "${SYSMON_COMMON_SOURCE_DIR}/manifest.tt"
                   )

add_custom_command(OUTPUT sysmonmsg.mc
                   COMMAND "${ICONV}" -f ASCII -t UTF-8 sysmonmsg.mc.utf16 -o sysmonmsg.mc
                   COMMENT "Converting sysmonmsg.mc.utf16 to UTF8"
                   DEPENDS sysmonmsg.mc.utf16
                   )

add_custom_command(OUTPUT sysmonmsg.h
                   COMMAND "${CMAKE_SOURCE_DIR}/extractMsgMc.sh" > sysmonmsg.h
                   COMMENT "Extracting sysmonmsg.h from sysmonmsg.mc"
                   DEPENDS sysmonmsg.mc
                   )

add_custom_command(OUTPUT sysmonmsgop.man.utf16
                   COMMAND "${TEXTTRANSFORM}" "${SYSMON_COMMON_SOURCE_DIR}/manifest.tt" -out sysmonmsgop.man.utf16 -a '!!version!internal' -a '!!type!man'
                   COMMENT "Extracting sysmonmsgop.man.utf16"
                   DEPENDS "${SYSMON_COMMON_SOURCE_DIR}/manifest.tt"
                   )

add_custom_command(OUTPUT sysmonmsgop.man
                   COMMAND "${ICONV}" -f ASCII -t UTF-8 sysmonmsgop.man.utf16 -o sysmonmsgop.man
                   COMMENT "Converting sysmonmsgop.man.utf16 to UTF8"
                   DEPENDS sysmonmsgop.man.utf16
                   )

add_custom_command(OUTPUT sysmonmsgop.h
                   COMMAND "${CMAKE_SOURCE_DIR}/extractMsgOp.sh" HEADER > sysmonmsgop.h
                   COMMENT "Extracting sysmonmsgop.h from sysmonmsgop.man"
                   DEPENDS sysmonmsgop.man
                   )

add_custom_command(OUTPUT sysmonmsgop.c
                   COMMAND "${CMAKE_SOURCE_DIR}/extractMsgOp.sh" > sysmonmsgop.c
                   COMMENT "Extracting sysmonmsgop.c from sysmonmsgop.man"
                   DEPENDS sysmonmsgop.man
                   )

#
# convert embedded files to objects for linking with the sysmon binary
#
add_custom_command(OUTPUT manifest.xml.o
                   COMMAND cd "${SYSMON_COMMON_SOURCE_DIR}" && "${LD}" -r -b binary -o "${PROJECT_BINARY_DIR}/manifest.xml.o" manifest.xml
                   COMMENT "Packing manifest.xml into manifest.xml.o"
                   DEPENDS "${SYSMON_COMMON_SOURCE_DIR}/manifest.xml"
                   )

set(PACKED_BINARY_FILES
    sysmonEBPFkern4.15.o
    sysmonEBPFkern4.16.o
    sysmonEBPFkern4.17-5.1.o
    sysmonEBPFkern5.2.o
    sysmonEBPFkern5.3-5.5.o
    sysmonEBPFkern5.6-.o
    sysmonEBPFkern4.15_core.o
    sysmonEBPFkern4.16_core.o
    sysmonEBPFkern4.17-5.1_core.o
    sysmonEBPFkern5.2_core.o
    sysmonEBPFkern5.3-5.5_core.o
    sysmonEBPFkern5.6-_core.o
    sysmonLogView
    sysmon.service
    sysmon.d
)

foreach(BIN_FILE IN LISTS PACKED_BINARY_FILES)
add_custom_command(OUTPUT "${BIN_FILE}.o"
                   COMMAND "${LD}" -r -b binary -o "${BIN_FILE}.o" "${BIN_FILE}"
                   COMMENT "Packing ${BIN_FILE} into ${BIN_FILE}.o"
                   DEPENDS "${PROJECT_BINARY_DIR}/${BIN_FILE}"
                   )
endforeach(BIN_FILE)


#
# copy the service files and installer to the build directory
#
add_custom_command(OUTPUT "${PROJECT_BINARY_DIR}/sysmon.d"
                   COMMAND cp "${CMAKE_SOURCE_DIR}/sysmon.d" "${PROJECT_BINARY_DIR}/sysmon.d"
                   COMMENT "Copying sysmon.d"
                   DEPENDS "${CMAKE_SOURCE_DIR}/sysmon.d"
                   )

add_custom_command(OUTPUT "${PROJECT_BINARY_DIR}/sysmon.service"
                   COMMAND cp "${CMAKE_SOURCE_DIR}/sysmon.service" "${PROJECT_BINARY_DIR}/sysmon.service"
                   COMMENT "Copying sysmon.service"
                   DEPENDS "${CMAKE_SOURCE_DIR}/sysmon.service"
                   )


#
# EBPF COMPILE OPTIONS
#
# This section specifies the options for building ebpf programs
#

# There are parts of the build process that needs to run when the CMake file changes.
# Specifically, the eBPF program compilation. This is done by comparing the hash of
# the CMake file to the previous hash. If they are different, then the eBPF programs
# are rebuilt.
file(MD5 "${CMAKE_CURRENT_LIST_FILE}" CMAKE_FILE_HASH)

set(PREVIOUS_CMAKE_FILE_HASH "")
if(EXISTS "${PROJECT_BINARY_DIR}/previous_cmake_file_hash.txt")
    file(READ "${PROJECT_BINARY_DIR}/previous_cmake_file_hash.txt" PREVIOUS_CMAKE_FILE_HASH)
endif()

file(WRITE "${PROJECT_BINARY_DIR}/previous_cmake_file_hash.txt" "${CMAKE_FILE_HASH}")

# set binaries and options for clang and llc
find_program(CLANG clang)
if(NOT CLANG)
    message(FATAL_ERROR "clang is required to build eBPF programs")
endif()
set(LLC "llc")
set(CLANG_OPTIONS -Wno-unused-value
                  -Wno-pointer-sign
                  -Wno-compare-distinct-pointer-types
                  -Wno-gnu-variable-sized-type-not-at-end
                  -Wno-address-of-packed-member
                  -Wno-tautological-compare
                  -Wno-unknown-warning-option
                  -g
                  )
set(CLANG_DEFINES -D __KERNEL__
                  -D __BPF_TRACING__
                  -D "__TARGET_ARCH_${BPF_TARGET_ARCH}"
                  -D __linux__
                  )
if (DEBUG_K)
    message("Using DEBUG_K Option...")
    list(APPEND CLANG_DEFINES -DDEBUG_K)
endif()

set(CLANG_INCLUDES
                   -I "${CMAKE_SOURCE_DIR}/gnu"
                   -I "${CMAKE_SOURCE_DIR}"
                   -I "${SYSMON_COMMON_SOURCE_DIR}"
                   -I "${SYSINTERNALS_EBPF_KERN_DIR}"
                   -I "${SYSINTERNALS_EBPF_LIBBPF_DIR}"
                   -I "${CMAKE_BINARY_DIR}"
                   )

set(EBPF_CORE_PROG_SUFFIX "_core")

#
# EBPF
#
# This section makes the EBPF programs
#

# function to make ebpf programs
function(build_ebpf ebpfsrc suffix)
    add_custom_command(OUTPUT ${ebpfsrc}${suffix}.o
                       COMMAND "${CLANG}" -nostdinc ${CLANG_INCLUDES}
                           -isystem "${TARGET_MULTIARCH_INCLUDE}"
                           -isystem "/usr/include"
                           -isystem "${TARGET_COMPILER_INCLUDE}"
                           ${CLANG_DEFINES} -O2 ${CLANG_OPTIONS}
                           -target bpf -fno-stack-protector
                           -c "${CMAKE_SOURCE_DIR}/ebpfKern/${ebpfsrc}.c"
                           -o "${ebpfsrc}${suffix}.o"
                       COMMENT "Building EBPF object ${ebpfsrc}${suffix}.o"
                       DEPENDS ebpfKern/${ebpfsrc}.c ${EBPF_DEPENDS}
                       )
endfunction()

# Loop for all ebpf programs
foreach(EBPF_PROG IN LISTS EBPF_PROGS)

    # add custom target to build all ebpf programs with 'all'
    add_custom_target(${EBPF_PROG} ALL
                      DEPENDS "${CMAKE_SOURCE_DIR}/ebpfKern/${EBPF_PROG}.c"
                     )

    # test to only build ebpf programs when they have changed or if CMake file has changed.
    if(${CMAKE_SOURCE_DIR}/ebpfKern/${EBPF_PROG}.c IS_NEWER_THAN ${CMAKE_BINARY_DIR}/${EBPF_PROG}.o OR NOT "${CMAKE_FILE_HASH}" STREQUAL "${PREVIOUS_CMAKE_FILE_HASH}")
        # first build NON CORE program
        list(REMOVE_ITEM CLANG_DEFINES -DEBPF_CO_RE)
        build_ebpf(${EBPF_PROG} "")
        set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES ${EBPF_PROG}.o)
    endif()

    if(${CMAKE_SOURCE_DIR}/ebpfKern/${EBPF_PROG}.c IS_NEWER_THAN ${CMAKE_BINARY_DIR}/${EBPF_PROG}${EBPF_CORE_PROG_SUFFIX}.o OR NOT "${CMAKE_FILE_HASH}" STREQUAL "${PREVIOUS_CMAKE_FILE_HASH}")
        # next build CORE program
        list(APPEND CLANG_DEFINES -DEBPF_CO_RE)
        build_ebpf(${EBPF_PROG} ${EBPF_CORE_PROG_SUFFIX})
        set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES ${EBPF_PROG}${EBPF_CORE_PROG_SUFFIX}.o)
    endif()

endforeach(EBPF_PROG)
