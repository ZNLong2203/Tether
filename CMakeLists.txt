cmake_minimum_required(VERSION 3.16)
project(tether VERSION 0.1.0 LANGUAGES C)

# One tree, two builds:
#   host (macOS / Linux): the portable core, the unit tests and the closed-loop
#                         simulator. No QNX needed.
#   QNX 8.0 (cmake -DCMAKE_TOOLCHAIN_FILE=cmake/qnx800.cmake): additionally
#                         builds the six Tether processes under qnx/.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wshadow -Wno-unused-parameter)

add_library(tether_core STATIC
  core/src/common.c
  core/src/config.c
  core/src/plant.c
  core/src/estimator.c
  core/src/arbiter.c
  core/src/policy.c
  core/src/rt.c)
target_include_directories(tether_core PUBLIC core/include)
set_target_properties(tether_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_library(M_LIB m)
if(M_LIB)
  target_link_libraries(tether_core PUBLIC ${M_LIB})
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "QNX")
  enable_testing()
  add_subdirectory(host)
  add_subdirectory(tests)
else()
  add_subdirectory(qnx)
endif()
