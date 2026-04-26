# CMakeLists.txt for DPA Switch Control Plane Library

# Install the entire Python library to the install directory
# Source dir has dash, but install with underscore for Python import compatibility
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DESTINATION lib
    FILES_MATCHING
    PATTERN "*.py"
    PATTERN "*.md"
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
)

# install p4gen also as a tool
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/p4gen.py
    DESTINATION bin RENAME dpa-p4gen
    PERMISSIONS OWNER_READ OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE)

# Also install the README for documentation
# install(
#     FILES ${CMAKE_CURRENT_SOURCE_DIR}/README.md
#     DESTINATION lib/dpa_switch_controller
# )

# Create a simple script that users can source to add to Python path
# configure_file(
#     ${CMAKE_CURRENT_SOURCE_DIR}/setup_path.sh.in
#     ${CMAKE_BINARY_DIR}/setup_dpa_controlplane_path.sh
#     @ONLY
# )
# install(
#     FILES ${CMAKE_BINARY_DIR}/setup_dpa_controlplane_path.sh
#     DESTINATION bin
#     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
#                 GROUP_READ GROUP_EXECUTE
#                 WORLD_READ WORLD_EXECUTE
# )

# message(STATUS "DPA Switch Control Plane library will be installed to: ${CMAKE_INSTALL_PREFIX}/lib/dpa_switch_controlplane")
# message(STATUS "Source directory: dpa-switch-controlplane (with dash)")
# message(STATUS "Install as: dpa_switch_controlplane (with underscore for Python imports)")
