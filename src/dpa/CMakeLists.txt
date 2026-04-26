message(STATUS "Configuring libdpa")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# fmt
FetchContent_Declare(fmt GIT_REPOSITORY https://github.com/fmtlib/fmt.git GIT_TAG 10.2.1)
FetchContent_MakeAvailable(fmt)
include_directories(${fmt_SOURCE_DIR}/include)
file(COPY ${fmt_SOURCE_DIR}/include/fmt DESTINATION ${CMAKE_BINARY_DIR}/include)
install(FILES $<TARGET_FILE:fmt> DESTINATION lib)
install(DIRECTORY ${fmt_SOURCE_DIR}/include/fmt DESTINATION include)


# nlohman json
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz DOWNLOAD_EXTRACT_TIMESTAMP true)
FetchContent_MakeAvailable(json)
include_directories(${json_SOURCE_DIR}/include)
install(DIRECTORY ${json_SOURCE_DIR}/include/nlohmann DESTINATION include)

# Create include directory once
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/include)

configure_file(config.h.in ${CMAKE_BINARY_DIR}/include/dpa/config.h)
install(FILES "${CMAKE_BINARY_DIR}/include/dpa/config.h" DESTINATION include/dpa)

file(GLOB_RECURSE DPA_SRC_HEADERS "*.h")
foreach(HEADER ${DPA_SRC_HEADERS})
  file(RELATIVE_PATH REL_PATH ${CMAKE_CURRENT_SOURCE_DIR} ${HEADER})
  get_filename_component(HEADER_DIR ${REL_PATH} DIRECTORY)
  install(FILES ${HEADER} DESTINATION include/dpa/${HEADER_DIR})
endforeach()

# add_custom_target(dpa_headers ALL DEPENDS ${GENERATED_HEADERS_LIST})

file(GLOB_RECURSE DPA_SOURCES "*.cpp" "*.cc")
# add compat shim so lib built on 24.04 links on 22.04
list(APPEND DPA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compat_isoc23.c")

# message(STATUS "DPA_SOURCES: ${DPA_SOURCES}")

add_library(dpa_objects OBJECT ${DPA_SOURCES})
# need -fPIC for libdpa to be used a torch plugin
target_link_libraries(dpa_objects PUBLIC nlohmann_json::nlohmann_json)
set_target_properties(dpa_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(dpa_objects
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include> # for generated config.h
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src> # for actual headers
  $<INSTALL_INTERFACE:include> # for installed use
)

add_library(dpa STATIC
  $<TARGET_OBJECTS:dpa_objects>
  $<TARGET_OBJECTS:fmt::fmt>
)
target_link_libraries(dpa PUBLIC nlohmann_json::nlohmann_json)

set_target_properties(dpa PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)


message(STATUS "DPA_AVX512_AVAILABLE=${DPA_AVX512_AVAILABLE} DPA_AVX2_AVAILABLE=${DPA_AVX2_AVAILABLE}")
if(DPA_AVX512_AVAILABLE)
  target_compile_options(dpa_objects PRIVATE -mavx512f -mavx512dq -mavx512bw -mavx2 -mfma)
  message(STATUS "dpa_objects: added AVX512 flags")
elseif(DPA_AVX2_AVAILABLE)
  target_compile_options(dpa_objects PRIVATE -mavx2 -mfma)
  message(STATUS "dpa_objects: added AVX2 flags")
else()
  message(STATUS "dpa_objects: no SIMD flags added")
endif()

if(DPA_DPDK)
  # set(PKG_CONFIG_USE_STATIC_LIBS ON)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk) # ubuntu/debian name
  if(NOT DPDK_FOUND)
    pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET dpdk)
  endif()

  # create dpdk.conf
  execute_process(COMMAND pkg-config --variable=libdir libdpdk OUTPUT_VARIABLE DPDK_PKG_LIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND pkg-config --variable=pmddir libdpdk OUTPUT_VARIABLE DPDK_PMDDIR     OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND pkg-config --cflags libdpdk          OUTPUT_VARIABLE DPDK_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND pkg-config --libs-only-l libdpdk     OUTPUT_VARIABLE DPDK_LIBS_L OUTPUT_STRIP_TRAILING_WHITESPACE)

  separate_arguments(_cflags NATIVE_COMMAND "${DPDK_CFLAGS}")
  list(FILTER _cflags EXCLUDE REGEX "^-m(arch|tune)=")
  string(REPLACE ";" " " _cflags "${_cflags}")

  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/dpdk.conf"
       CONTENT "libdir=${DPDK_PKG_LIBDIR}\npmddir=${DPDK_PMDDIR}\ncflags=${_cflags}\nlibs=${DPDK_LIBS_L}\n")

  install(FILES "${CMAKE_BINARY_DIR}/dpdk.conf" DESTINATION share/dpa)

  get_target_property(_dpdk_opts PkgConfig::DPDK INTERFACE_COMPILE_OPTIONS)
  if(_dpdk_opts)
    list(FILTER _dpdk_opts EXCLUDE REGEX "^-m(arch|tune)=")
    set_target_properties(PkgConfig::DPDK PROPERTIES INTERFACE_COMPILE_OPTIONS "${_dpdk_opts}")
  endif()

  # Needed so backend_dpdk.cc can #include <rte_eal.h>
  target_link_libraries(dpa_objects PRIVATE PkgConfig::DPDK)
  # target_link_libraries(dpa PUBLIC PkgConfig::DPDK)
  target_include_directories(dpa PRIVATE ${DPDK_INCLUDE_DIRS})
  if(DPA_DEBUG)
    target_compile_definitions(dpa CONFIG_RTE_LIBRTE_MBUF_DEBUG=y)
  endif()

  get_target_property(_chk PkgConfig::DPDK INTERFACE_COMPILE_OPTIONS)

  # pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)
  message(STATUS "DPDK_cflags: ${DPDK_CFLAGS_OTHER}")
  message(STATUS "DPDK opts after strip: ${_chk}")
  message(STATUS "DPDK_include_dir: ${DPDK_INCLUDE_DIRS}")
  message(STATUS "DPDK_library_dir: ${DPDK_LIBRARY_DIRS}")
  # message(STATUS "DPDK_link_libraries: ${DPDK_LINK_LIBRARIES}")
  # target_link_libraries(dpa PRIVATE PkgConfig::DPDK)
endif()

install(TARGETS dpa
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

message(STATUS "Configuring libdpa - done")
