cmake_minimum_required(VERSION 3.23)
project(ZenAssetProcessor VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(ZENASSET_BUILD_TESTING "Build ZenAsset tests" ${PROJECT_IS_TOP_LEVEL})
if(ZENASSET_BUILD_TESTING)
    enable_testing()
endif()

option(ZENASSET_BUILD_PROCESSOR "Build source importers and asset processing" OFF)

add_library(ZenAssetFormat STATIC
    src/Format/Version.cpp
    src/Format/Processing.cpp
    src/Format/Scene.cpp
    src/Format/Utf8.cpp
    src/Format/FileIO.cpp
    src/Format/Validation.cpp
    src/Format/AssetKernels.cpp)
add_library(ZenAsset::Format ALIAS ZenAssetFormat)
set_target_properties(ZenAssetFormat PROPERTIES EXPORT_NAME Format)
target_compile_features(ZenAssetFormat PUBLIC cxx_std_20)
target_include_directories(ZenAssetFormat PRIVATE src)
file(GLOB_RECURSE ZENASSET_FINGERPRINT_INPUTS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/*.cmake)
list(APPEND ZENASSET_FINGERPRINT_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
list(JOIN ZENASSET_FINGERPRINT_INPUTS "\n" ZENASSET_FINGERPRINT_INPUT_TEXT)
set(ZENASSET_FINGERPRINT_INPUT_LIST ${CMAKE_CURRENT_BINARY_DIR}/FingerprintInputs.txt)
file(CONFIGURE OUTPUT ${ZENASSET_FINGERPRINT_INPUT_LIST} CONTENT "${ZENASSET_FINGERPRINT_INPUT_TEXT}\n" @ONLY)
set(ZENASSET_FINGERPRINT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/Generated/BuildFingerprint.h)
add_custom_command(OUTPUT ${ZENASSET_FINGERPRINT_HEADER}
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${ZENASSET_FINGERPRINT_HEADER}
        -DINPUT_LIST=${ZENASSET_FINGERPRINT_INPUT_LIST} -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildFingerprint.cmake
    DEPENDS ${ZENASSET_FINGERPRINT_INPUTS} ${ZENASSET_FINGERPRINT_INPUT_LIST}
    VERBATIM)
target_sources(ZenAssetFormat PRIVATE ${ZENASSET_FINGERPRINT_HEADER})
target_include_directories(ZenAssetFormat PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/Generated)
target_include_directories(ZenAssetFormat PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_library(ZenAssetReader STATIC src/Reader/Package.cpp src/Reader/Transport.cpp)
add_library(ZenAsset::Reader ALIAS ZenAssetReader)
set_target_properties(ZenAssetReader PROPERTIES EXPORT_NAME Reader)
target_link_libraries(ZenAssetReader PUBLIC ZenAssetFormat)
target_include_directories(ZenAssetReader PRIVATE src)

foreach(target ZenAssetFormat ZenAssetReader)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

include(cmake/CodecDependencies.cmake)
target_link_libraries(ZenAssetReader PRIVATE ZenAssetZstdDecoder)
if(ZENASSET_BUILD_PROCESSOR)
    include(cmake/Processor.cmake)
endif()
include(cmake/SceneIO.cmake)
include(cmake/ImageReader.cmake)
include(cmake/CommandLine.cmake)

if(ZENASSET_BUILD_TESTING)
    add_executable(ZenAssetFormatTests tests/FormatRecordTests.cpp)
    target_link_libraries(ZenAssetFormatTests PRIVATE ZenAssetFormat)
    add_test(NAME ZenAssetFormatTests COMMAND ZenAssetFormatTests)
    add_executable(ZenAssetPackageTests tests/PackageTests.cpp)
    target_link_libraries(ZenAssetPackageTests PRIVATE ZenAssetReader)
    target_include_directories(ZenAssetPackageTests PRIVATE src)
    add_test(NAME ZenAssetPackageTests COMMAND ZenAssetPackageTests)
    if(TARGET ZenAssetZstdEncoder)
        add_executable(ZenAssetTransportTests tests/TransportTests.cpp)
        target_include_directories(ZenAssetTransportTests PRIVATE src)
        target_link_libraries(ZenAssetTransportTests PRIVATE ZenAssetReader ZenAssetZstdEncoder)
        add_test(NAME ZenAssetTransportTests COMMAND ZenAssetTransportTests)
    endif()
endif()
include(cmake/Fuzzing.cmake)

install(TARGETS ZenAssetFormat ZenAssetReader EXPORT ZenAssetTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/ZenAsset)
configure_package_config_file(cmake/ZenAssetConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ZenAssetConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZenAsset)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ZenAssetConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(EXPORT ZenAssetTargets NAMESPACE ZenAsset:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZenAsset)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ZenAssetConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ZenAssetConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZenAsset)
