target_sources(ZenAssetReader PRIVATE src/Reader/Scene.cpp src/Reader/MeshDecoding.cpp)
target_link_libraries(ZenAssetReader PRIVATE ZenAssetMeshoptDecoder)
if(TARGET ZenAssetProcessor)
    target_sources(ZenAssetProcessor PRIVATE src/Writer/Scene.cpp src/Writer/Compression.cpp src/Writer/MeshEncoding.cpp)
    target_link_libraries(ZenAssetProcessor PUBLIC ZenAssetReader PRIVATE ZenAssetZstdEncoder ZenAssetMeshoptEncoder)
    if(ZENASSET_BUILD_TESTING)
        add_executable(ZenAssetMeshEncodingTests tests/MeshEncodingTests.cpp)
        target_include_directories(ZenAssetMeshEncodingTests PRIVATE src)
        target_link_libraries(ZenAssetMeshEncodingTests PRIVATE ZenAssetProcessor)
        add_test(NAME ZenAssetMeshEncodingTests COMMAND ZenAssetMeshEncodingTests)
        add_executable(ZenAssetCompressionWriterTests tests/CompressionWriterTests.cpp)
        target_include_directories(ZenAssetCompressionWriterTests PRIVATE src)
        target_link_libraries(ZenAssetCompressionWriterTests PRIVATE ZenAssetProcessor)
        add_test(NAME ZenAssetCompressionWriterTests COMMAND ZenAssetCompressionWriterTests)
        add_executable(ZenAssetSceneIOTests tests/SceneIOTests.cpp)
        target_include_directories(ZenAssetSceneIOTests PRIVATE src)
        target_link_libraries(ZenAssetSceneIOTests PRIVATE ZenAssetProcessor)
        add_test(NAME ZenAssetSceneIOTests COMMAND ZenAssetSceneIOTests)
        if(ZENASSET_TEST_CORPUS_DIR)
            add_test(NAME ZenAssetMeshEncodingFixtures COMMAND ZenAssetMeshEncodingTests
                "${ZENASSET_TEST_CORPUS_DIR}/complete_scene.glb"
                "${ZENASSET_TEST_CORPUS_DIR}/typed_animation_targets.gltf"
                "${ZENASSET_TEST_CORPUS_DIR}/vertex_attributes.gltf"
                "${ZENASSET_TEST_CORPUS_DIR}/FBX/combined_morph_skin_ascii.fbx")
            add_test(NAME ZenAssetCompressionFixtures COMMAND ZenAssetCompressionWriterTests
                "${ZENASSET_TEST_CORPUS_DIR}/complete_scene.glb"
                "${ZENASSET_TEST_CORPUS_DIR}/texture_roles.gltf"
                "${ZENASSET_TEST_CORPUS_DIR}/typed_animation_targets.gltf"
                "${ZENASSET_TEST_CORPUS_DIR}/vertex_attributes.gltf"
                "${ZENASSET_TEST_CORPUS_DIR}/FBX/combined_morph_skin_ascii.fbx")
            add_test(NAME ZenAssetSceneIOFixtures COMMAND ZenAssetSceneIOTests
                "${ZENASSET_TEST_CORPUS_DIR}/complete_scene.glb"
                "${ZENASSET_TEST_CORPUS_DIR}/texture_roles.gltf"
                "${ZENASSET_TEST_CORPUS_DIR}/typed_animation_targets.gltf"
                "${ZENASSET_TEST_CORPUS_DIR}/vertex_attributes.gltf"
                "${ZENASSET_TEST_CORPUS_DIR}/FBX/combined_morph_skin_ascii.fbx")
        endif()
    endif()
endif()
