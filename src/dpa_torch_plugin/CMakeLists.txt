include(FetchContent)

set(TORCH_VER "2.7.1")
set(TORCH_TAG "v${TORCH_VER}")
set(GLOO_TAG "5354032ea08eadd7fc4456477f7f7c6308818509")

file(GLOB PLUGIN_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.inc"
)
set(CPP_SOURCES ${PLUGIN_SOURCES})
list(FILTER CPP_SOURCES EXCLUDE REGEX "\\.py$")

# --- Gloo headers (always needed: plugin .cpp transitively includes <gloo/*.h>) ---
message(STATUS "Fetching Gloo headers for plugin build...")
FetchContent_Declare(
  gloo
  GIT_REPOSITORY https://github.com/facebookincubator/gloo.git
  GIT_TAG ${GLOO_TAG}
)
FetchContent_Populate(gloo)

set(GLOO_INCLUDE_DIR ${gloo_SOURCE_DIR})

# Generate gloo/config.h
set(GLOO_VERSION_MAJOR 0)
set(GLOO_VERSION_MINOR 5)
set(GLOO_VERSION_PATCH 0)
set(GLOO_USE_CUDA 1)
set(GLOO_USE_NCCL 0)
set(GLOO_USE_ROCM 0)
set(GLOO_USE_RCCL 0)
set(GLOO_USE_REDIS 0)
set(GLOO_USE_MPI 0)
set(GLOO_USE_AVX 0)
set(GLOO_USE_LIBUV 0)
set(GLOO_HAVE_TRANSPORT_TCP_TLS 1)
set(GLOO_HAVE_TRANSPORT_TCP 0)
set(GLOO_HAVE_TRANSPORT_IBVERBS 0)
set(GLOO_HAVE_TRANSPORT_UV 0)
configure_file(${gloo_SOURCE_DIR}/gloo/config.h.in ${gloo_SOURCE_DIR}/gloo/config.h)

file(GLOB_RECURSE GLOO_HEADERS "${gloo_SOURCE_DIR}/gloo/*.h")
list(FILTER GLOO_HEADERS EXCLUDE REGEX "redis_store\\.h$")

foreach(HEADER ${GLOO_HEADERS})
  file(RELATIVE_PATH HEADER_REL_PATH ${gloo_SOURCE_DIR} ${HEADER})
  get_filename_component(HEADER_DIR ${HEADER_REL_PATH} DIRECTORY)
  install(FILES ${HEADER} DESTINATION include/${HEADER_DIR})
endforeach()

# --- Intellisense-only targets (LSP support during plugin development) ---
# if(DPA_DEVELOP)
execute_process(
  COMMAND python -c "import torch; print(';'.join(torch.utils.cpp_extension.include_paths()))"
  OUTPUT_VARIABLE TORCH_INCLUDE_DIRS
  ERROR_VARIABLE TORCH_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE TORCH_RESULT
)
execute_process(
  COMMAND python -c "import sysconfig; print(sysconfig.get_path('include'))"
  OUTPUT_VARIABLE PYTHON_INCLUDE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE PYTHON_RESULT
)


if (DPA_DEVELOP)
  if(TORCH_RESULT EQUAL 0 AND TORCH_INCLUDE_DIRS)
    message(STATUS "Found PyTorch headers for intellisense support")
  else()
    message(WARNING "Downloading LibTorch for intellisense...")
    FetchContent_Declare(
      libtorch
      URL "https://download.pytorch.org/libtorch/cpu/libtorch-cxx11-abi-shared-with-deps-${TORCH_VER}%2Bcpu.zip"
      DOWNLOAD_EXTRACT_TIMESTAMP ON
    )
    FetchContent_MakeAvailable(libtorch)
    set(TORCH_INCLUDE_DIRS
      "${libtorch_SOURCE_DIR}/include"
      "${libtorch_SOURCE_DIR}/include/torch/csrc/api/include"
    )
  endif()
endif()

find_package(CUDAToolkit REQUIRED)

add_library(intellisense_helper EXCLUDE_FROM_ALL ${CPP_SOURCES})
if(DPA_DPDK)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)
  if(NOT DPDK_FOUND)
    pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET dpdk)
  endif()
  target_include_directories(intellisense_helper PRIVATE ${DPDK_INCLUDE_DIRS})
endif()
target_compile_features(intellisense_helper PRIVATE cxx_std_17)
target_compile_definitions(intellisense_helper PRIVATE C10_CUDA_NO_CMAKE_CONFIGURE_FILE=1)
target_include_directories(intellisense_helper PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
target_include_directories(intellisense_helper PRIVATE ${TORCH_INCLUDE_DIRS})
target_include_directories(intellisense_helper PRIVATE ${PYTHON_INCLUDE_DIR})
target_include_directories(intellisense_helper PUBLIC ${gloo_SOURCE_DIR})
target_link_libraries(intellisense_helper PUBLIC dpa_objects)
message(STATUS "TORCH_INCLUDE_DIRS: ${TORCH_INCLUDE_DIRS}")


add_custom_target(torch-plugin
  COMMAND python ${CMAKE_SOURCE_DIR}/scripts/dpa-pyvenv-check.py
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target install
  COMMAND pip wheel --no-build-isolation --no-deps
  --wheel-dir ${CMAKE_BINARY_DIR}/lib
  ${CMAKE_SOURCE_DIR}/src/dpa_torch_plugin
  COMMAND pip install --force-reinstall --no-deps
  ${CMAKE_BINARY_DIR}/lib/DPA-*.whl
  DEPENDS dpa
  USES_TERMINAL
  COMMENT "Building and installing DPA PyTorch plugin"
)
