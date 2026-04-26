message(STATUS "Setting up dependency 'doctest'")

# Use FetchContent module to handle the download
include(FetchContent)

# Declare doctest repository
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
)

# Just populate (download) the content without processing its CMakeLists.txt
FetchContent_Populate(doctest)

# Create our own interface library
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE ${doctest_SOURCE_DIR})

# Set variables for compatibility
set(DOCTEST_INCLUDE_DIR ${doctest_SOURCE_DIR})

message(STATUS "Setting up dependency 'doctest' - done")
