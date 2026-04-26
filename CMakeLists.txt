cmake_minimum_required(VERSION 3.14)

project(dpa DESCRIPTION "Data Plane AllReduce")

option(DPA_HOST    "Build DPA host library" ON)
option(DPA_SWITCH  "Build DPA device library" OFF)
option(DPA_DEVELOP "Build DPA for development" OFF)
option(DPA_DPDK    "Build the dpdk backend" ON)
option(DPA_TORCH   "Build the DPA Pytorch plugin" ON)
option(DPA_AVX     "Enable vector instructions" ON)
option(DPA_AVX_512 "Use AVX512 if available" ON)
option(DPA_TEST    "Build tests" OFF)
option(DPA_DOCS    "Build documentation" OFF)
option(DPA_TRACE   "Enable tracing in Debug builds" OFF)

option(DPA_PROFILE "Perform various performance measurements" OFF)
# set(DPA_PROFILE_SKIP 0 CACHE STRING "Number of operations to skip before starting profiling")

# option(DPA_PROFILE_FIRST "Profile the first allreduce call" OFF)

# option(DPA_FASTESTK_NOINIT "Disable fastestk during the init burst phase" OFF)
option(DPA_IMPLICIT_SYN    "Enable implicit SYNs" OFF)

# option(DPA_FASTESTK        "Enable fastestk on the host side" ON)
option(DPA_FASTESTK_BULK   "Enable entering fastestk for all entries" OFF)
option(DPA_FASTESTK_EXIT   "Enable exiting fastestk for the current task" OFF)
option(DPA_SYNCHRON_BULK   "Enable entering SYN for all entries" OFF)
# DPDK backend knobs
option(DPA_DPDK_RE_FIRST   "Check for retransmissions before flushing TX" OFF)
option(DPA_DPDK_RE_DISABLE "Disable retransmissions" OFF)
option(DPA_DPDK_RX_REUSE   "Reuse RX mbufs for TX" ON)
option(DPA_DPDK_WIN_LARGE  "Use large window (up to 256)" OFF)
option(DPA_DPDK_WIN_HUGE   "Use large window (up to 512)" ON)
# Socket backend knobs
option(DPA_SOCKET_ZEROCOPY "Enable zero copy in the socket backend" OFF)
# DPA Pytorch plugin knobs
option(DPA_TORCH_PINNEDPOOL          "Enable pinned mempool in the torch plugin" ON)
option(DPA_TORCH_PINNEDPOOL_PRETOUCH "Pretouch pinned pool tensors" OFF)
# option(DPA_TORCH_WORKSTEAL           "Enable workstealing torch plugin" OFF)
# option(DPA_TORCH_PIPELINE            "Enable pipelining torch plugin" OFF)
# option(DPA_TORCH_MIXED               "Enable mixed torch plugin" OFF)

# Other
# option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

set(PROJECT_VERSION_MAJOR "0")
set(PROJECT_VERSION_MINOR "1")
set(PROJECT_VERSION_PATCH "0")
set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH};${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CMAKE_INVOCATION_DIR ${CMAKE_CURRENT_LIST_DIR})

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(DEFAULT_BUILD_TYPE "Debug")
  message(STATUS "Using default build type: '${DEFAULT_BUILD_TYPE}'")
  set(CMAKE_BUILD_TYPE "${DEFAULT_BUILD_TYPE}" CACHE STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
else()
  message(STATUS "Using build type: '${CMAKE_BUILD_TYPE}'")
endif()


set(SCRIPTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/scripts)
set(DPA_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX ${CMAKE_BINARY_DIR}/install CACHE PATH "Install path prefix" FORCE)
endif()

add_subdirectory(src)
if (DPA_DOCS)
  add_subdirectory(docs)
endif()
if(DPA_TEST)
  enable_testing()
  add_subdirectory(test)
endif()

# include(GenerateBuildInfo)
# generate_build_info()

set(BUILD_OPTIONS_TXT "${CMAKE_BINARY_DIR}/build-options.txt")

# Create/update the file at configure time
file(WRITE "${BUILD_OPTIONS_TXT}" "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}\n")

get_cmake_property(_vars CACHE_VARIABLES)
list(FILTER _vars INCLUDE REGEX "^DPA_")
list(SORT _vars)
foreach(v IN LISTS _vars)
  file(APPEND "${BUILD_OPTIONS_TXT}" "${v}=${${v}}\n")
endforeach()