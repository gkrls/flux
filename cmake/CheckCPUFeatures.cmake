# Check if CPU supports a specific feature
function(check_cpu_feature FEATURE VARIABLE)
  execute_process(
    COMMAND grep -c "${FEATURE}" /proc/cpuinfo
    OUTPUT_VARIABLE FEATURE_COUNT
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(${FEATURE_COUNT} GREATER 0)
    set(${VARIABLE} TRUE PARENT_SCOPE)
    message(STATUS "CPU supports ${FEATURE}")
  else()
    set(${VARIABLE} FALSE PARENT_SCOPE)
    message(STATUS "CPU does NOT support ${FEATURE}")
  endif()
endfunction()