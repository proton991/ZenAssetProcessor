cmake_minimum_required(VERSION 3.23)
project(ZenAssetProvokingVertexProbe LANGUAGES CXX)
# Optional evaluation only; Vulkan is not added to the installed SDK.
find_package(ZenAsset CONFIG REQUIRED COMPONENTS Processor Reader)
find_package(Vulkan REQUIRED COMPONENTS glslc)
set(shaders)
foreach(stage vert frag)
    set(shader "${CMAKE_CURRENT_BINARY_DIR}/provoking.${stage}.spv")
    add_custom_command(OUTPUT "${shader}"
        COMMAND Vulkan::glslc --target-env=vulkan1.1
            "${CMAKE_CURRENT_LIST_DIR}/provoking.${stage}" -o "${shader}"
        DEPENDS "provoking.${stage}" VERBATIM)
    list(APPEND shaders "${shader}")
endforeach()
add_executable(provoking_vertex main.cpp RasterProbe.cpp ${shaders})
target_compile_features(provoking_vertex PRIVATE cxx_std_20)
target_link_libraries(provoking_vertex PRIVATE ZenAsset::Processor ZenAsset::Reader Vulkan::Vulkan)
if(MSVC)
    target_compile_options(provoking_vertex PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(provoking_vertex PRIVATE -Wall -Wextra -Wpedantic)
endif()
