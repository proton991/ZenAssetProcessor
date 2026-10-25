include_guard(GLOBAL)
include(FetchContent)
option(ZENASSET_INSTALL_PROCESSOR "Install Processor and its static dependency packages" ${PROJECT_IS_TOP_LEVEL})
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/ReaderSources.cmake)

# Runtime builds use the reviewed decoder subset shipped with this repository.
# Only Processor needs the full upstream encoder/optimizer sources.
if(ZENASSET_BUILD_PROCESSOR)
    FetchContent_Declare(meshoptimizer
        URL https://codeload.github.com/zeux/meshoptimizer/zip/refs/tags/v1.1
        URL_HASH SHA256=6aecc2d3b4328f1f5f4127fb16a144de80cc9eb35c32387807c8c04b0b6dbbf3
        SOURCE_SUBDIR zenasset-no-cmake)
    FetchContent_MakeAvailable(meshoptimizer)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/MeshoptDecoder.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/BlockCodecs.cmake)
target_link_libraries(ZenAssetReader PRIVATE ZenAssetBlockDecoder)

# Keep the runtime archive physically free of compression and dictionary-builder
# translation units. Names are owned by ZenAsset rather than reusing a parent's
# possibly differently configured zstd target.
if(ZENASSET_BUILD_PROCESSOR)
    FetchContent_Declare(zenasset_zstd
        URL https://codeload.github.com/facebook/zstd/zip/refs/tags/v1.5.7
        URL_HASH SHA256=7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4
        SOURCE_SUBDIR zenasset-no-cmake)
    FetchContent_MakeAvailable(zenasset_zstd)
    zenasset_verify_reader_sources("${zenasset_zstd_SOURCE_DIR}" "zstd/")
endif()
set(ZENASSET_ZSTD_LIBRARY_DIR "${ZENASSET_READER_SOURCE_DIR}/zstd/lib")

add_library(ZenAssetZstdDecoder STATIC
    ${ZENASSET_ZSTD_LIBRARY_DIR}/common/debug.c
    ${ZENASSET_ZSTD_LIBRARY_DIR}/common/entropy_common.c
    ${ZENASSET_ZSTD_LIBRARY_DIR}/common/error_private.c
    ${ZENASSET_ZSTD_LIBRARY_DIR}/common/fse_decompress.c
    ${ZENASSET_ZSTD_LIBRARY_DIR}/common/pool.c
    ${ZENASSET_ZSTD_LIBRARY_DIR}/common/threading.c
    ${ZENASSET_ZSTD_LIBRARY_DIR}/common/xxhash.c
    ${ZENASSET_ZSTD_LIBRARY_DIR}/common/zstd_common.c
    ${ZENASSET_ZSTD_LIBRARY_DIR}/decompress/huf_decompress.c
    ${ZENASSET_ZSTD_LIBRARY_DIR}/decompress/zstd_ddict.c
    ${ZENASSET_ZSTD_LIBRARY_DIR}/decompress/zstd_decompress_block.c
    ${ZENASSET_ZSTD_LIBRARY_DIR}/decompress/zstd_decompress.c)
set_target_properties(ZenAssetZstdDecoder PROPERTIES
    POSITION_INDEPENDENT_CODE ON EXPORT_NAME PrivateZstdDecoder)
target_include_directories(ZenAssetZstdDecoder PUBLIC
    $<BUILD_INTERFACE:${ZENASSET_ZSTD_LIBRARY_DIR}>)
# C implementations are portable across supported compilers. No assembly,
# legacy formats, multi-thread compressor, or unbounded internal worker pool.
target_compile_definitions(ZenAssetZstdDecoder PRIVATE
    ZSTD_DISABLE_ASM ZSTD_LEGACY_SUPPORT=0)
if(MSVC)
    target_compile_definitions(ZenAssetZstdDecoder PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
install(TARGETS ZenAssetZstdDecoder EXPORT ZenAssetTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${ZENASSET_READER_SOURCE_DIR}/zstd/LICENSE ${ZENASSET_READER_SOURCE_DIR}/zstd/COPYING
    DESTINATION ${CMAKE_INSTALL_DATADIR}/ZenAsset/licenses/zstd)
install(FILES ${CMAKE_CURRENT_LIST_DIR}/../THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/ZenAsset)

if(ZENASSET_BUILD_PROCESSOR)
    set(ZENASSET_ZSTD_ENCODER_DIR "${zenasset_zstd_SOURCE_DIR}/lib/compress")
    add_library(ZenAssetZstdEncoder STATIC
        ${ZENASSET_ZSTD_ENCODER_DIR}/fse_compress.c
        ${ZENASSET_ZSTD_ENCODER_DIR}/hist.c
        ${ZENASSET_ZSTD_ENCODER_DIR}/huf_compress.c
        ${ZENASSET_ZSTD_ENCODER_DIR}/zstd_compress_literals.c
        ${ZENASSET_ZSTD_ENCODER_DIR}/zstd_compress_sequences.c
        ${ZENASSET_ZSTD_ENCODER_DIR}/zstd_compress_superblock.c
        ${ZENASSET_ZSTD_ENCODER_DIR}/zstd_compress.c
        ${ZENASSET_ZSTD_ENCODER_DIR}/zstd_double_fast.c
        ${ZENASSET_ZSTD_ENCODER_DIR}/zstd_fast.c
        ${ZENASSET_ZSTD_ENCODER_DIR}/zstd_lazy.c
        ${ZENASSET_ZSTD_ENCODER_DIR}/zstd_ldm.c
        ${ZENASSET_ZSTD_ENCODER_DIR}/zstd_opt.c
        ${ZENASSET_ZSTD_ENCODER_DIR}/zstd_preSplit.c
        ${ZENASSET_ZSTD_ENCODER_DIR}/zstdmt_compress.c)
    set_target_properties(ZenAssetZstdEncoder PROPERTIES
        POSITION_INDEPENDENT_CODE ON EXPORT_NAME PrivateZstdEncoder)
    target_link_libraries(ZenAssetZstdEncoder PUBLIC ZenAssetZstdDecoder)
    target_compile_definitions(ZenAssetZstdEncoder PRIVATE
        ZSTD_DISABLE_ASM ZSTD_LEGACY_SUPPORT=0)
    if(MSVC)
        target_compile_definitions(ZenAssetZstdEncoder PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(ZENASSET_BUILD_TESTING)
        add_executable(ZenAssetCodecBoundaryTests tests/CodecBoundaryTests.cpp)
        target_link_libraries(ZenAssetCodecBoundaryTests PRIVATE
            ZenAssetMeshoptDecoder ZenAssetMeshoptEncoder ZenAssetZstdEncoder meshoptimizer)
        target_include_directories(ZenAssetCodecBoundaryTests PRIVATE src)
        target_compile_features(ZenAssetCodecBoundaryTests PRIVATE cxx_std_20)
        add_test(NAME ZenAssetCodecBoundaryTests COMMAND ZenAssetCodecBoundaryTests)
    endif()
endif()
