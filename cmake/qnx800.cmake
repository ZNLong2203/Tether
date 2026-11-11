# CMake toolchain file for QNX SDP 8.0.
#
#   source /opt/qnx800/qnxsdp-env.sh
#   cmake -S . -B build-qnx -DCMAKE_TOOLCHAIN_FILE=cmake/qnx800.cmake [-DQNX_ARCH=aarch64le|x86_64]
#   cmake --build build-qnx
#
# QNX_HOST and QNX_TARGET come from qnxsdp-env.sh. The SDP does not ship a
# toolchain file of its own, so this follows CMake's built-in QNX support:
# the compiler is qcc and the architecture is chosen with -V<variant>.
set(CMAKE_SYSTEM_NAME QNX)

if(NOT DEFINED QNX_ARCH)
  set(QNX_ARCH aarch64le CACHE STRING "aarch64le or x86_64")
endif()
if(NOT QNX_ARCH MATCHES "^(aarch64le|x86_64)$")
  message(FATAL_ERROR "QNX_ARCH must be aarch64le or x86_64, got '${QNX_ARCH}'")
endif()

if(NOT DEFINED ENV{QNX_HOST} OR NOT DEFINED ENV{QNX_TARGET})
  message(FATAL_ERROR "QNX_HOST/QNX_TARGET not set: run 'source <sdp>/qnxsdp-env.sh' first")
endif()
set(QNX_HOST "$ENV{QNX_HOST}")
set(QNX_TARGET "$ENV{QNX_TARGET}")

set(CMAKE_C_COMPILER "${QNX_HOST}/usr/bin/qcc")
set(CMAKE_C_COMPILER_TARGET "gcc_nto${QNX_ARCH}")
# The binutils are named after the CPU, without the endianness suffix.
if(QNX_ARCH STREQUAL "aarch64le")
  set(QNX_CPU aarch64)
else()
  set(QNX_CPU x86_64)
endif()
set(CMAKE_AR "${QNX_HOST}/usr/bin/nto${QNX_CPU}-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${QNX_HOST}/usr/bin/nto${QNX_CPU}-ranlib" CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH "${QNX_TARGET}/${QNX_ARCH}" "${QNX_TARGET}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
