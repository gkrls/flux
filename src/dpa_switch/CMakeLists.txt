
install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tofino_tna
  DESTINATION lib/dpa_switch
  FILES_MATCHING PATTERN "*.p4"
)

install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tofino_tna_su
  DESTINATION lib/dpa_switch
  FILES_MATCHING PATTERN "*.p4"
)

