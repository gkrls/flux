include(FindPackageHandleStandardArgs)

set(Tofino_FOUND FALSE)

if(NOT DEFINED SDE AND NOT DEFINED ENV{SDE})
  return() # not found
endif()

if(NOT DEFINED SDE)
  get_filename_component(sde_abs_path "$ENV{SDE}" ABSOLUTE)
  set(SDE "${sde_abs_path}")
endif()

set(SDE_INSTALL "${SDE}/install")
set(SDE_INSTALL_BIN "${SDE_INSTALL}/bin")
if(NOT EXISTS "${SDE_INSTALL}")
  return() # not found
endif()

# Version
if(EXISTS "${SDE_INSTALL}/share/VERSION")
  file(READ "${SDE_INSTALL}/share/VERSION" SDE_VERSION_CONTENT)
  string(REGEX MATCH "([0-9]+)\\.([0-9]+)\\.([0-9]+)" SDE_VERSION "${SDE_VERSION_CONTENT}")
endif()

# Tools
find_program(Tofino_P4C NAMES bf-p4c PATHS "${SDE_INSTALL_BIN}" NO_DEFAULT_PATH)
find_program(Tofino_P4I NAMES p4i    PATHS "${SDE_INSTALL_BIN}" NO_DEFAULT_PATH)
if(NOT Tofino_P4C OR NOT Tofino_P4I)
  return() # not found
endif()

# Found
set(Tofino_SDE "${SDE}")
set(Tofino_SDE_INSTALL "${SDE_INSTALL}")
set(Tofino_SDE_VERSION "${SDE_VERSION}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Tofino REQUIRED_VARS Tofino_SDE_INSTALL Tofino_P4C Tofino_P4I VERSION_VAR Tofino_SDE_VERSION)
