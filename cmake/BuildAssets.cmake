# A build-system entry point for the CLI's content-aware incremental builder.
# The target runs whenever requested: source timestamps alone cannot determine
# whether dependencies, processing settings, or the compiler have changed.
include_guard(GLOBAL)

function(zenasset_add_manifest_build)
    cmake_parse_arguments(PARSE_ARGV 0 ZA "ALL"
        "TARGET;MANIFEST;OUTPUT_DIRECTORY;COMPILER;JOBS;MEMORY_BUDGET;JOB_MEMORY" "")
    if(ZA_UNPARSED_ARGUMENTS OR ZA_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "zenasset_add_manifest_build: unknown argument or missing value")
    endif()
    if(NOT ZA_TARGET OR NOT ZA_MANIFEST)
        message(FATAL_ERROR "zenasset_add_manifest_build requires TARGET and MANIFEST")
    endif()
    if(TARGET "${ZA_TARGET}")
        message(FATAL_ERROR "zenasset_add_manifest_build target already exists: ${ZA_TARGET}")
    endif()

    get_filename_component(manifest "${ZA_MANIFEST}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(command_args build "${manifest}")
    if(DEFINED ZA_OUTPUT_DIRECTORY)
        get_filename_component(output "${ZA_OUTPUT_DIRECTORY}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
        list(APPEND command_args --output-dir "${output}")
    endif()
    foreach(pair IN ITEMS "JOBS|--jobs" "MEMORY_BUDGET|--memory-budget" "JOB_MEMORY|--job-memory")
        string(REPLACE "|" ";" pair "${pair}")
        list(GET pair 0 key)
        list(GET pair 1 option)
        if(DEFINED ZA_${key})
            if(NOT "${ZA_${key}}" MATCHES "^[1-9][0-9]*$")
                message(FATAL_ERROR "zenasset_add_manifest_build ${key} requires a positive integer")
            endif()
            list(APPEND command_args "${option}" "${ZA_${key}}")
        endif()
    endforeach()

    set(dependencies "${manifest}")
    if(DEFINED ZA_COMPILER)
        get_filename_component(compiler "${ZA_COMPILER}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        list(APPEND dependencies "${compiler}")
    elseif(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "zenasset_add_manifest_build needs COMPILER pointing to a host zenassetc when cross-compiling")
    elseif(TARGET ZenAsset::Compiler)
        set(compiler "$<TARGET_FILE:ZenAsset::Compiler>")
        list(APPEND dependencies ZenAsset::Compiler)
    else()
        message(FATAL_ERROR "zenasset_add_manifest_build needs the Compiler component or an explicit COMPILER executable")
    endif()

    set(all_arg)
    if(ZA_ALL)
        set(all_arg ALL)
    endif()
    add_custom_target("${ZA_TARGET}" ${all_arg}
        COMMAND "${compiler}" ${command_args}
        DEPENDS ${dependencies}
        COMMENT "Checking and cooking assets from ${manifest}"
        VERBATIM)
endfunction()
