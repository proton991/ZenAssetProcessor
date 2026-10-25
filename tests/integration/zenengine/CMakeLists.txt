# Opt-in consumer suite. Configure ZenEngine with ZEN_EXTERNAL_TESTS_DIR pointing here.
if(NOT TARGET ZenEngineAssetTools OR NOT TARGET gtest_main)
    message(FATAL_ERROR "Configure ZenEngine with BUILD_TESTING=ON, ZEN_BUILD_ASSET_TOOLS=ON and ZEN_EXTERNAL_TESTS_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
endif()

add_executable(ZenAssetEngineTests
    CommonTest/SceneModelCatalogTests.cpp
    CommonTest/SceneImportTests.cpp
    CommonTest/SceneAssetIOTests.cpp
    CommonTest/SceneLoaderTests.cpp
    CommonTest/FbxSceneImportTests.cpp
    CommonTest/SceneAnimationTests.cpp
    CommonTest/SceneImportCompatibilityTests.cpp
    CommonTest/SceneImportStressTests.cpp
    CommonTest/SceneImportMorphAttributeTests.cpp
    CommonTest/ScenePhysicalUnitsTests.cpp
    CommonTest/SceneImportPaddingTests.cpp
    CommonTest/SceneAnimationPointerTestSupport.cpp
    CommonTest/SceneAnimationPointerTests.cpp
    CommonTest/ProcessedSceneAdapterTests.cpp
    CommonTest/AssetImportRegressionTests.cpp)
target_link_libraries(ZenAssetEngineTests PRIVATE ZenEngineAssetTools ZenAsset::Processor gtest_main draco::draco)
target_include_directories(ZenAssetEngineTests PRIVATE "${PROJECT_SOURCE_DIR}/ZenCore/Source")
if(TARGET ZenAssetUfbx)
    target_link_libraries(ZenAssetEngineTests PRIVATE ZenAssetUfbx meshoptimizer)
    FetchContent_GetProperties(draco SOURCE_DIR ZEN_TEST_DRACO_SOURCE_DIR BINARY_DIR ZEN_TEST_DRACO_BINARY_DIR)
    target_include_directories(ZenAssetEngineTests PRIVATE "${ZEN_TEST_DRACO_SOURCE_DIR}/src"
        "${ZEN_TEST_DRACO_BINARY_DIR}" "${PROJECT_BINARY_DIR}")
else()
    target_link_libraries(ZenAssetEngineTests PRIVATE ZenAsset::PrivateUfbx meshoptimizer::meshoptimizer)
endif()
add_test(NAME ZenAssetEngineTests COMMAND ZenAssetEngineTests)
set_tests_properties(ZenAssetEngineTests PROPERTIES LABELS "asset;integration;cpu" TIMEOUT 600)

foreach(NAME GLTFCorpusImport SceneLoadBenchmark ScenePackageSmoke ScenePackageBenchmark SceneUploadBenchmark)
    add_executable(${NAME} "CommonTest/${NAME}.cpp")
    target_link_libraries(${NAME} PRIVATE ZenCore ZenAsset::Reader)
endforeach()
target_link_libraries(GLTFCorpusImport PRIVATE ZenEngineAssetTools)
target_link_libraries(SceneLoadBenchmark PRIVATE ZenEngineAssetTools)

add_executable(ZenAssetTextureIntegrationTest CommonTest/ArraySceneTextureIntegrationTests.cpp)
target_link_libraries(ZenAssetTextureIntegrationTest PRIVATE ZenCore ZenVulkanTestMain)
target_include_directories(ZenAssetTextureIntegrationTest PRIVATE "${PROJECT_SOURCE_DIR}/ZenSamples/CommonTest")
set(ASSET_SHADER_PATH "${CMAKE_CURRENT_BINARY_DIR}/shaders/")
set(ASSET_SHADER_BINARIES)
foreach(SHADER scene_array_views.comp scene_spatial_views.comp scene_normal_sampling.comp)
    set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/CommonTest/Shaders/${SHADER}")
    set(OUTPUT "${ASSET_SHADER_PATH}${SHADER}.spv")
    add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ASSET_SHADER_PATH}"
        COMMAND ${GLSL_VALIDATOR} --target-env spirv1.3 -V "${SOURCE}"
            "-I${PROJECT_SOURCE_DIR}/ZenCore/Include" "-I${PROJECT_SOURCE_DIR}/Data/Shaders" -o "${OUTPUT}"
        DEPENDS "${SOURCE}" "${PROJECT_SOURCE_DIR}/Data/Shaders/Common/material.glsl"
            "${PROJECT_SOURCE_DIR}/Data/Shaders/Common/bindless_heap.glsl"
            "${PROJECT_SOURCE_DIR}/ZenCore/Include/Graphics/Shared/Bindless.h" VERBATIM)
    list(APPEND ASSET_SHADER_BINARIES "${OUTPUT}")
endforeach()
add_custom_target(ZenAssetIntegrationShaders DEPENDS ${ASSET_SHADER_BINARIES})
add_dependencies(ZenAssetTextureIntegrationTest ZenAssetIntegrationShaders)
target_compile_definitions(ZenAssetTextureIntegrationTest PRIVATE RDG_REFLECTION_TEST_PATH="${ASSET_SHADER_PATH}")
add_test(NAME ZenAssetTextureIntegrationTest COMMAND ZenAssetTextureIntegrationTest)
set_tests_properties(ZenAssetTextureIntegrationTest PROPERTIES LABELS "asset;integration;vulkan" TIMEOUT 300)

if(TARGET ZenEditorServices)
    add_executable(EditorAssetRenderingTest Editor/EditorAssetRenderingTests.cpp)
    target_link_libraries(EditorAssetRenderingTest PRIVATE ZenEditorServices gtest_main)
    target_include_directories(EditorAssetRenderingTest PRIVATE "${PROJECT_SOURCE_DIR}/ZenEditor/Tests"
        "${PROJECT_SOURCE_DIR}/ZenSamples/CommonTest" CommonTest)
    target_compile_definitions(EditorAssetRenderingTest PRIVATE
        ZEN_EDITOR_FIXTURES="${PROJECT_SOURCE_DIR}/ZenEditor/Tests/Fixtures/"
        ZEN_EDITOR_SHARED_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/CommonTest/Assets/"
        ZEN_ASSET_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/Editor/Fixtures/")
    add_test(NAME EditorAssetRenderingTest COMMAND EditorAssetRenderingTest)
    set_tests_properties(EditorAssetRenderingTest PROPERTIES LABELS "asset;integration;vulkan" TIMEOUT 600)
endif()
