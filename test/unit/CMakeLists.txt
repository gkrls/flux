include(FindDoctest)
include_directories(${doctest_SOURCE_DIR})
# add_subdirectory(dpa)

include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/test/unit/include)

link_directories(${CMAKE_BINARY_DIR}/src/dpa)

###############################################################################
###############################################################################
###############################################################################

include_directories(
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>  # for generated config.h
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>      # for actual headers
)

# dpa/util/bitset
add_executable(test-dpa-util-bitset test_dpa_util_bitset.cc)
target_link_libraries(test-dpa-util-bitset doctest dpa)
add_test(dpa::util::bitset test-dpa-util-bitset)
