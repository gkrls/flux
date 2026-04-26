set(P4_BINARY_DIR "${CMAKE_BINARY_DIR}/p4")
set(P4_INCLUDE ${SDE_INSTALL}/share/p4c/p4include)

file(MAKE_DIRECTORY ${P4_BINARY_DIR})

set(P4_PREPROCESS_BASIC_OPTS
  -x assembler-with-cpp
  -D__p4c__=1
  -D__p4c_major__=${P4C_VERSION_MAJOR}
  -D__p4c_minor__=${P4C_VERSION_MINOR}
  -D__p4c_patchlevel__=${P4C_VERSION_PATCH}
  -D__p4c_version__=\"${P4C_VERSION_MAJOR}.${P4C_VERSION_MINOR}.${P4C_VERSION_PATCH}\"
  -C -undef -nostdinc
  -x assembler-with-cpp)

function(p4_compile tgt src main tofino_version) #preprocess compile compile_commands
  set(target ${tgt})
  set(${target}_binary_dir           ${P4_BINARY_DIR}/${target})
  set(${target}_compiler_log         ${${target}_binary_dir}/${target}.p4c.log)
  set(${target}_compiler_out_archive ${${target}_binary_dir}/${target}.tar.bz2)
  set(${target}_compiler_out         ${${target}_binary_dir}/${target})
  set(${target}_compiler_commands    ${${target}_binary_dir}/${target}.p4c.commands)
  set(${target}_config_txt           ${${target}_binary_dir}/config.txt)

  set(${target}_preprocessor_out     ${${target}_binary_dir}/${target}.pp.p4)
  set(${target}_preprocessor_opts    ${P4_PREPROCESS_BASIC_OPTS})
  list(APPEND ${target}_preprocessor_opts -D__TARGET_TOFINO__=${tofino_version} -P)

  # message(STATUS ${${target}_preprocessor_opts})

  file(MAKE_DIRECTORY ${${target}_binary_dir})
  file(GLOB_RECURSE ${target}_p4_sources ${src}/*.p4)
  list(APPEND ${target}_p4_sources ${main})

  set(compile_cmd ${P4C} ${main} --target tofino --program-name ${target} -o ${${target}_compiler_out} --archive 2>&1 | tee ${${target}_compiler_log})
  set(compile_dry_cmd ${P4C} ${main} --test-only --target tofino --program-name ${target} -o ${${target}_compiler_out} > ${${target}_compiler_commands})
  set(preprocess_cmd cc -E ${${target}_preprocessor_opts} -I ${P4_INCLUDE} ${main} -o ${${target}_preprocessor_out})

  # Create separate targets for each step
  add_custom_command(
    OUTPUT ${${target}_preprocessor_out}
    COMMAND ${preprocess_cmd}
    DEPENDS ${${target}_p4_sources} ${P4_COMMON_SOURCES}
    COMMENT "Preprocessing ${main}"
  )

  add_custom_command(
    OUTPUT ${${target}_compiler_out_archive} ${${target}_compiler_log}
    COMMAND ${compile_cmd}
    DEPENDS ${${target}_preprocessor_out}
    COMMENT "Compiling ${main}"
  )

  add_custom_command(
    OUTPUT ${${target}_compiler_commands}
    COMMAND ${compile_dry_cmd}
    DEPENDS ${${target}_compiler_out_archive}
    COMMENT "Generating compiler commands for ${main}"
  )

  # Main target that depends on all outputs
  add_custom_target(${target}
    DEPENDS ${${target}_compiler_out_archive} ${${target}_compiler_log} ${${target}_compiler_commands} ${${target}_preprocessor_out}
    COMMENT "Building P4 program ${target}"
  )

  add_custom_target(p4i-${tgt} COMMAND ${P4I} --open ${${target}_compiler_archive} ${target} VERBATIM)
endfunction()



# function(p4_build tgt sources main arch)
#   # Parse optional arguments
#   cmake_parse_arguments(PARSE_ARGV 4 P4_BUILD "" "" "FLAGS")
  
#   set(target ${tgt})
#   set(${target}_binary_dir           ${P4_BINARY_DIR}/${target})
#   set(${target}_compiler_log         ${${target}_binary_dir}/${target}.p4c.log)
#   set(${target}_compiler_out_archive ${${target}_binary_dir}/${target}.tar.bz2)
#   set(${target}_compiler_out         ${${target}_binary_dir}/${target})
#   set(${target}_compiler_commands    ${${target}_binary_dir}/${target}.p4c.commands)
#   set(${target}_config_txt           ${${target}_binary_dir}/config.txt)

#   set(${target}_preprocessor_out     ${${target}_binary_dir}/${target}.pp.p4)
#   set(${target}_preprocessor_opts    ${P4_PREPROCESS_BASIC_OPTS})
#   list(APPEND ${target}_preprocessor_opts -D__TARGET_TOFINO__=${arch} -P)

#   # Add any additional flags to the preprocessor options
#   if(P4_BUILD_FLAGS)
#     list(APPEND ${target}_preprocessor_opts ${P4_BUILD_FLAGS})
#   endif()

#   message(STATUS ${${target}_preprocessor_opts})

#   file(MAKE_DIRECTORY ${${target}_binary_dir})
#   file(GLOB_RECURSE ${target}_p4_sources ${sources}/*.p4)
#   list(APPEND ${target}_p4_sources ${main})

#   set(compile_cmd ${P4C} ${main} --target tofino --program-name ${target} -o ${${target}_compiler_out} --archive 2>&1 | tee ${${target}_compiler_log})
#   set(compile_dry_cmd ${P4C} ${main} --test-only --target tofino --program-name ${target} -o ${${target}_compiler_out} > ${${target}_compiler_commands})
#   set(preprocess_cmd cc -E ${${target}_preprocessor_opts} -I ${P4_INCLUDE} ${main} -o ${${target}_preprocessor_out})

#   add_custom_target(${target} ALL
#     COMMAND ${compile_cmd} && ${compile_dry_cmd} && ${preprocess_cmd}
#     DEPENDS ${${target}_p4_sources} ${P4_COMMON_SOURCES} #p4-common
#     COMMENT "Compiling ${main}"
#   )

#   add_custom_target(p4i-${tgt} COMMAND ${P4I} --open ${${target}_compiler_archive} ${target} VERBATIM)
# endfunction()


