if(DPA_HOST)
    # Compiler/Linker options
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_ok OUTPUT msg)
    if(ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        message(WARNING "IPO/LTO not enabled: ${msg}")
    endif()

    add_compile_options(
        $<$<CONFIG:Release>:-march=native>
        $<$<CONFIG:Release>:-mtune=native>
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
    )

    add_link_options(
        $<$<CONFIG:Release>:-Wl,--gc-sections>
    )

    include(CheckCPUFeatures)
    include(CheckCXXCompilerFlag)

    set(DPA_AVX512_OK 0)
    set(DPA_AVX2_OK 0)

    if(DPA_AVX_512)
        check_cpu_feature("avx512f" _cpu_f)
        check_cpu_feature("avx512dq" _cpu_dq)
        check_cpu_feature("avx512bw" _cpu_bw)
        check_cxx_compiler_flag("-mavx512f -mavx512dq -mavx512bw" _cc_512)
        if(_cpu_f AND _cpu_dq AND _cpu_bw AND _cc_512)
            set(DPA_AVX512_OK 1)
        endif()
    endif()

    if(DPA_AVX AND NOT DPA_AVX512_OK)
        check_cpu_feature("avx2" _cpu_avx2)
        check_cxx_compiler_flag("-mavx2 -mfma" _cc_avx2)
        if(_cpu_avx2 AND _cc_avx2)
            set(DPA_AVX2_OK 1)
        endif()
    endif()

    if(DPA_AVX512_OK)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx512f -mavx512dq -mavx512bw -mavx2 -mfma")
        set(DPA_AVX512_AVAILABLE 1)
        set(DPA_AVX2_AVAILABLE 1)
        message(STATUS "SIMD: AVX512")
    elseif(DPA_AVX2_OK)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2 -mfma")
        set(DPA_AVX2_AVAILABLE 1)
        message(STATUS "SIMD: AVX2")
    else()
        message(STATUS "SIMD: disabled")
    endif()

    
    # Set C++ standard
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(STATUS "Debug build: keeping default optimization")
    else()
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
        set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -O3")
    endif()

    add_subdirectory(dpa)
    if (DPA_TORCH)
        add_subdirectory(dpa_torch_plugin)
    endif()
endif()

if(DPA_SWITCH)
    include(FindTofino)
    find_package(Tofino QUIET)

    if(Tofino_FOUND)
        message(STATUS "Tofino SDE found:")
        message(STATUS "  Tofino_SDE: ${Tofino_SDE}")
        message(STATUS "  Tofino_SDE_INSTALL: ${Tofino_SDE_INSTALL}")
        message(STATUS "  Tofino_SDE_VERSION: ${Tofino_SDE_VERSION}")
        message(STATUS "  Tofino_P4C: ${Tofino_P4C}")
        message(STATUS "  Tofino_P4C: ${Tofino_P4I}")
    else()
        message(WARNING
            "DPA_SWITCH is ON, but Tofino SDE was not found. \n"
            "  * dpa_switch will be installed but the Tofino SDE is needed to compile the P4 programs\n"
            "  * dpa_switch_controller will be built and installed but the cli controller is unusable outside a BFRT session")
    endif()

    add_subdirectory(dpa_switch)
    add_subdirectory(dpa_switch_controller)

endif()

# add_subdirectory(tools)

