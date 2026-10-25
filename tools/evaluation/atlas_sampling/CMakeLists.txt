cmake_minimum_required(VERSION 3.23)
project(ZenAssetAtlasSamplingProbe LANGUAGES CXX)
# Evaluation only: Vulkan is not a dependency of any shipped ZenAsset target.
find_package(Vulkan REQUIRED COMPONENTS glslc)
option(ZENASSET_PROBE_FLOAT64 "Enable diagnostic layouts 10, 11 and 13 requiring shaderFloat64" OFF)
set(root "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(shaders)
foreach(stage vert frag)
    set(shader "${CMAKE_CURRENT_BINARY_DIR}/atlas_sampling.${stage}.spv")
    add_custom_command(OUTPUT "${shader}"
        COMMAND Vulkan::glslc --target-env=vulkan1.1 -DZENASSET_PROBE_FLOAT64=$<BOOL:${ZENASSET_PROBE_FLOAT64}>
                "${CMAKE_CURRENT_LIST_DIR}/atlas_sampling.${stage}" -o "${shader}"
        DEPENDS "atlas_sampling.${stage}" VERBATIM)
    list(APPEND shaders "${shader}")
endforeach()
add_executable(atlas_sampling AtlasSampling.cpp
    "${root}/src/Processing/AtlasPacking.cpp" "${root}/src/Processing/ArrayPacking.cpp"
    "${root}/src/Format/Scene.cpp" ${shaders})
target_include_directories(atlas_sampling PRIVATE "${root}/include" "${root}/src")
target_compile_features(atlas_sampling PRIVATE cxx_std_20)
target_compile_definitions(atlas_sampling PRIVATE ZENASSET_PROBE_FLOAT64=$<BOOL:${ZENASSET_PROBE_FLOAT64}>)
target_link_libraries(atlas_sampling PRIVATE Vulkan::Vulkan)
if(MSVC)
    target_compile_options(atlas_sampling PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(atlas_sampling PRIVATE -Wall -Wextra -Wpedantic)
endif()
