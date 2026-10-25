cmake_minimum_required(VERSION 3.23)
project(ZenAssetInstalledProcessorConsumer LANGUAGES CXX)
find_package(ZenAsset CONFIG REQUIRED COMPONENTS Processor)
add_executable(ZenAssetInstalledProcessorConsumer main.cpp)
target_link_libraries(ZenAssetInstalledProcessorConsumer PRIVATE ZenAsset::Processor ZenAsset::Reader)
enable_testing()
add_test(NAME InstalledProcessorImportsAndCompressesTexturedTriangle
    COMMAND ZenAssetInstalledProcessorConsumer ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/textured_triangle.gltf
        ${CMAKE_CURRENT_BINARY_DIR}/textured_triangle.zenasset 1)

add_test(NAME InstalledProcessorReportsExternalFileProgress
    COMMAND ZenAssetInstalledProcessorConsumer ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/textured_triangle_external.gltf
        ${CMAKE_CURRENT_BINARY_DIR}/textured_triangle_external.zenasset 3)
