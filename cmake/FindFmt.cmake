message(STATUS "Setting up dependency 'fmt'")

include(FetchContent)


# Disable fmt's default install rules
set(FMT_INSTALL OFF CACHE BOOL "Disable fmt installation" FORCE)

FetchContent_MakeAvailable(fmt)

# message(STATUS "FMT: ${fmt_SOURCE_DIR}")
set(FMT_INCLUDE_DIR ${fmt_SOURCE_DIR}/include)
include_directories(${FMT_INCLUDE_DIR})

# convenient way to have all headers we need in one place
file(COPY ${FMT_INCLUDE_DIR}/fmt DESTINATION ${CMAKE_BINARY_DIR}/include)

# Custom install rules for only libfmtd.a (or libfmt.a) and headers
# install(FILES ${fmt_BINARY_DIR}/libfmt.a DESTINATION lib)
install(FILES $<TARGET_FILE:fmt> DESTINATION lib)
install(DIRECTORY ${FMT_INCLUDE_DIR}/fmt DESTINATION include)

message(STATUS "Setting up dependency 'fmt' - done")