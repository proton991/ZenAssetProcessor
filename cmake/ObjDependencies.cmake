include_guard(GLOBAL)

# Source import only. This module is called from ProcessorDependencies, never
# from the Format/Reader graph. Keep upstream sources and headers private.
function(zenasset_obj_dependencies)
    FetchContent_Declare(zenasset_tinyobj
        URL https://codeload.github.com/tinyobjloader/tinyobjloader/zip/45636bdcef1a4fec140346b90c0b50bf0bc3e23b
        URL_HASH SHA256=9de28c152b94c074c7ffc4fd984d342764b6c1e70957ee9a7bc04bb187c9cf46
        SOURCE_SUBDIR zenasset-no-cmake)
    FetchContent_Declare(zenasset_earcut
        URL https://codeload.github.com/mapbox/earcut.hpp/zip/4811a2b69b91f6127a75e780de6e2113609ddabb
        URL_HASH SHA256=977dd15799f9b0060f1ffcb0c5c3c59ee496eca78e2f97796dbede271e60ce0f
        SOURCE_SUBDIR zenasset-no-cmake)
    FetchContent_MakeAvailable(zenasset_tinyobj zenasset_earcut)

    add_library(ZenAssetTinyObj STATIC ${zenasset_tinyobj_SOURCE_DIR}/tiny_obj_loader.cc)
    set_target_properties(ZenAssetTinyObj PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME PrivateTinyObj)
    target_compile_features(ZenAssetTinyObj PRIVATE cxx_std_11)
    target_include_directories(ZenAssetTinyObj SYSTEM PUBLIC $<BUILD_INTERFACE:${zenasset_tinyobj_SOURCE_DIR}>)
    # The importer validates polygons and invokes earcut itself. Disabling the
    # bundled fast_float also excludes its implementation from this target.
    target_compile_definitions(ZenAssetTinyObj PUBLIC
        $<BUILD_INTERFACE:TINYOBJLOADER_DISABLE_FAST_FLOAT>
        $<BUILD_INTERFACE:tinyobj=zenasset_tinyobj>)

    add_library(ZenAssetEarcut INTERFACE)
    set_target_properties(ZenAssetEarcut PROPERTIES EXPORT_NAME PrivateEarcut)
    target_include_directories(ZenAssetEarcut SYSTEM INTERFACE $<BUILD_INTERFACE:${zenasset_earcut_SOURCE_DIR}/include>)
    target_compile_definitions(ZenAssetEarcut INTERFACE $<BUILD_INTERFACE:mapbox=zenasset_mapbox>)
endfunction()
