include_guard(GLOBAL)
FetchContent_Declare(zenasset_bcdec
    URL https://codeload.github.com/iOrange/bcdec/zip/80859ed3b7afb1c527a2a99d70c61457bea72d0c
    URL_HASH SHA256=90a7a9bdabc22b34b23045a1750d31a77b597c65104f46278fa7aadb4b0c80ff
    SOURCE_SUBDIR zenasset-no-cmake)
FetchContent_MakeAvailable(zenasset_bcdec)
add_library(ZenAssetBlockDecoder STATIC src/Codecs/BlockDecoding.cpp)
set_target_properties(ZenAssetBlockDecoder PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME PrivateBlockDecoder)
target_link_libraries(ZenAssetBlockDecoder PRIVATE ZenAssetFormat)
target_include_directories(ZenAssetBlockDecoder PRIVATE src ${zenasset_bcdec_SOURCE_DIR})
install(TARGETS ZenAssetBlockDecoder EXPORT ZenAssetTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${zenasset_bcdec_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/ZenAsset/licenses/bcdec)
if(ZENASSET_BUILD_TESTING)
    add_executable(ZenAssetBlockDecoderTests tests/BlockDecoderTests.cpp)
    target_link_libraries(ZenAssetBlockDecoderTests PRIVATE ZenAssetFormat ZenAssetBlockDecoder)
    target_include_directories(ZenAssetBlockDecoderTests PRIVATE src)
    add_test(NAME ZenAssetBlockDecoderTests COMMAND ZenAssetBlockDecoderTests)
endif()

if(ZENASSET_BUILD_PROCESSOR)
    FetchContent_Declare(zenasset_bc7enc
        URL https://codeload.github.com/richgel999/bc7enc_rdo/zip/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
        URL_HASH SHA256=e4b9328266568c8c345186bc246328b2dc6a248660b8c56d22e0771f00f83090
        SOURCE_SUBDIR zenasset-no-cmake)
    FetchContent_Declare(zenasset_bc6h
        URL https://codeload.github.com/0xc0de/bc6h_enc/zip/59947031122606cc9926d598454e7e3e9c316e2b
        URL_HASH SHA256=88d0374320825104df16d00436d2315cb4290947066452c6ef1bf7990885d22d
        SOURCE_SUBDIR zenasset-no-cmake)
    FetchContent_MakeAvailable(zenasset_bc7enc zenasset_bc6h)
    # Keep the fetched archive immutable and capture the correction in the SDK fingerprint.
    set(zenasset_bc6h_patched_header "${CMAKE_CURRENT_BINARY_DIR}/Generated/BC6H/bc6h_enc.h")
    add_custom_command(OUTPUT "${zenasset_bc6h_patched_header}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${zenasset_bc6h_SOURCE_DIR}/bc6h_enc.h
            -DOUTPUT=${zenasset_bc6h_patched_header} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchBc6h.cmake
        DEPENDS ${zenasset_bc6h_SOURCE_DIR}/bc6h_enc.h ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchBc6h.cmake
        VERBATIM)
    add_library(ZenAssetBlockEncoder STATIC src/Codecs/BlockEncoding.cpp src/Codecs/Bc6hEncoder.cpp
        "${zenasset_bc6h_patched_header}"
        ${zenasset_bc7enc_SOURCE_DIR}/bc7enc.cpp ${zenasset_bc7enc_SOURCE_DIR}/rgbcx.cpp)
    set_target_properties(ZenAssetBlockEncoder PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME PrivateBlockEncoder)
    target_link_libraries(ZenAssetBlockEncoder PRIVATE ZenAssetFormat)
    target_include_directories(ZenAssetBlockEncoder PRIVATE src ${zenasset_bc7enc_SOURCE_DIR}
        "${CMAKE_CURRENT_BINARY_DIR}/Generated/BC6H")
    target_compile_definitions(ZenAssetBlockEncoder PRIVATE
        bc7enc_compress_block=zenasset_bc7enc_compress_block
        bc7enc_compress_block_init=zenasset_bc7enc_compress_block_init
        bc7enc_compress_block_params=ZenAssetBC7Parameters color_rgba=ZenAssetBC7Color
        rgbcx=zenasset_rgbcx bc6h_enc=zenasset_bc6h_enc)
    if(ZENASSET_INSTALL_PROCESSOR)
        install(TARGETS ZenAssetBlockEncoder EXPORT ZenAssetTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
        install(FILES ${zenasset_bc7enc_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/ZenAsset/licenses/bc7enc_rdo)
        install(FILES ${zenasset_bc6h_SOURCE_DIR}/LICENSE "${zenasset_bc6h_patched_header}"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/ZenAsset/licenses/bc6h_enc)
    endif()
    if(ZENASSET_BUILD_TESTING)
        add_executable(ZenAssetBlockCodecTests tests/BlockCodecTests.cpp)
        target_link_libraries(ZenAssetBlockCodecTests PRIVATE ZenAssetFormat ZenAssetBlockEncoder ZenAssetBlockDecoder)
        target_include_directories(ZenAssetBlockCodecTests PRIVATE src)
        add_test(NAME ZenAssetBlockCodecTests COMMAND ZenAssetBlockCodecTests)
    endif()
endif()
