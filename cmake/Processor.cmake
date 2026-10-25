include(${CMAKE_CURRENT_LIST_DIR}/ProcessorDependencies.cmake)
zenasset_processor_dependencies()

add_library(ZenAssetProcessor STATIC
    src/Processor/Import.cpp
    src/Processor/ImportProgress.cpp
    src/Processor/Cook.cpp
    src/Importers/GltfAssetReader.cpp
    src/Importers/GltfAnimationBindings.cpp
    src/Importers/FbxSceneImporter.cpp
    src/Importers/FbxDeformation.cpp
    src/Importers/FbxAnimation.cpp
    src/Importers/ObjSceneImporter.cpp
    src/Importers/ObjMaterial.cpp
    src/Importers/ImageDecoder.cpp
    src/Importers/ImportErrors.cpp
    src/Importers/AssetHash.cpp
    src/Importers/StbImage.c
    src/Processing/SceneAssetProcessing.cpp
    src/Processing/SurfaceGeometry.cpp)
add_library(ZenAsset::Processor ALIAS ZenAssetProcessor)
set_target_properties(ZenAssetProcessor PROPERTIES EXPORT_NAME Processor CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_compile_features(ZenAssetProcessor PUBLIC cxx_std_20)
target_link_libraries(ZenAssetProcessor PUBLIC ZenAssetFormat PRIVATE
    fastgltf simdjson draco::draco meshoptimizer webpdecoder ZenAssetUfbx ZenAssetBasis ZenAssetStb ZenAssetTinyObj ZenAssetEarcut)
target_include_directories(ZenAssetProcessor PRIVATE src ${ZENASSET_DECODER_INCLUDE_DIRS})
if(MSVC)
    target_compile_options(ZenAssetProcessor PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(ZenAssetProcessor PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/ProcessorPackaging.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/GeometryProcessing.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/TextureProcessing.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/Build.cmake)

if(ZENASSET_BUILD_TESTING)
    add_executable(ZenAssetGeometryBudgetTests tests/GeometryBudgetTests.cpp)
    target_include_directories(ZenAssetGeometryBudgetTests PRIVATE src)
    target_link_libraries(ZenAssetGeometryBudgetTests PRIVATE ZenAssetProcessor)
    add_test(NAME ZenAssetGeometryBudgetTests COMMAND ZenAssetGeometryBudgetTests)
    add_executable(ZenAssetGltfGeometryBudgetTests tests/GltfGeometryBudgetTests.cpp)
    target_include_directories(ZenAssetGltfGeometryBudgetTests PRIVATE src ${ZENASSET_DECODER_INCLUDE_DIRS})
    target_link_libraries(ZenAssetGltfGeometryBudgetTests PRIVATE ZenAssetProcessor ZenAssetReader meshoptimizer draco::draco)
    add_test(NAME ZenAssetGltfGeometryBudgetTests COMMAND ZenAssetGltfGeometryBudgetTests)
    add_executable(ZenAssetFbxGeometryBudgetTests tests/FbxGeometryBudgetTests.cpp)
    target_include_directories(ZenAssetFbxGeometryBudgetTests PRIVATE src)
    target_link_libraries(ZenAssetFbxGeometryBudgetTests PRIVATE ZenAssetProcessor)
    add_executable(ZenAssetObjImportTests tests/ObjImportTests.cpp)
    target_include_directories(ZenAssetObjImportTests PRIVATE src)
    target_link_libraries(ZenAssetObjImportTests PRIVATE ZenAssetProcessor ZenAssetReader)
    add_test(NAME ZenAssetObjImportTests COMMAND ZenAssetObjImportTests)
    add_executable(ZenAssetCookTests tests/CookTests.cpp)
    target_link_libraries(ZenAssetCookTests PRIVATE ZenAssetProcessor ZenAssetReader)
    target_include_directories(ZenAssetCookTests PRIVATE src)
    add_test(NAME ZenAssetCookTests COMMAND ZenAssetCookTests)
    add_executable(ZenAssetProcessingTests tests/ProcessingTests.cpp)
    target_link_libraries(ZenAssetProcessingTests PRIVATE ZenAssetProcessor)
    target_include_directories(ZenAssetProcessingTests PRIVATE src)
    add_test(NAME ZenAssetProcessingTests COMMAND ZenAssetProcessingTests)
    add_executable(ZenAssetImportTests tests/ImportTests.cpp)
    target_link_libraries(ZenAssetImportTests PRIVATE ZenAssetProcessor)
    add_test(NAME ZenAssetImportControlFlow COMMAND ZenAssetImportTests)
    add_executable(ZenAssetFileProgressTests tests/FileProgressTests.cpp)
    target_link_libraries(ZenAssetFileProgressTests PRIVATE ZenAssetProcessor)
    add_test(NAME ZenAssetFileProgressTests COMMAND ZenAssetFileProgressTests)
    add_subdirectory(tools/validation)
    set(ZENASSET_TEST_CORPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/zenengine/CommonTest/Assets"
        CACHE PATH "Source fixtures for semantic tests; empty disables corpus tests")
    if(ZENASSET_TEST_CORPUS_DIR)
        add_test(NAME ZenAssetFbxGeometryBudgetTests COMMAND ZenAssetFbxGeometryBudgetTests
            ${ZENASSET_TEST_CORPUS_DIR}/FBX)
        add_test(NAME ZenAssetFileProgressFixtures COMMAND ZenAssetFileProgressTests
            ${ZENASSET_TEST_CORPUS_DIR}/texture_roles.gltf
            ${ZENASSET_TEST_CORPUS_DIR}/FBX/blender_293_embedded_textures_7400_binary.fbx)
        add_test(NAME ZenAssetCookFixtures COMMAND ZenAssetCookTests
            ${ZENASSET_TEST_CORPUS_DIR}/complete_scene.glb
            ${ZENASSET_TEST_CORPUS_DIR}/vertex_attributes.gltf
            ${ZENASSET_TEST_CORPUS_DIR}/FBX/combined_morph_skin_ascii.fbx)
        add_test(NAME ZenAssetProcessingFbxRegression COMMAND ZenAssetProcessingTests
            ${ZENASSET_TEST_CORPUS_DIR}/FBX/combined_morph_skin_ascii.fbx)
        add_test(NAME ZenAssetImportLimits COMMAND ZenAssetImportTests --limits
            ${ZENASSET_TEST_CORPUS_DIR}/FBX/blender_293_embedded_textures_7400_binary.fbx)
        add_test(NAME ZenAssetImportGltfLimits COMMAND ZenAssetImportTests --limits
            ${ZENASSET_TEST_CORPUS_DIR}/texture_roles.gltf)
        add_test(NAME ZenAssetImportFixtures COMMAND ZenAssetImportTests
            ${ZENASSET_TEST_CORPUS_DIR}/complete_scene.glb
            ${ZENASSET_TEST_CORPUS_DIR}/typed_animation_targets.gltf
            ${ZENASSET_TEST_CORPUS_DIR}/vertex_attributes.gltf
            ${ZENASSET_TEST_CORPUS_DIR}/FBX/blender_279_sausage_7400_binary.fbx
            ${ZENASSET_TEST_CORPUS_DIR}/FBX/combined_morph_skin_ascii.fbx)
    endif()
endif()
