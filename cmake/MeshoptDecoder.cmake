include(${CMAKE_CURRENT_LIST_DIR}/MeshoptExtraction.cmake)

set(ZENASSET_MESHOPT_DECODER_DIRECTORY "${ZENASSET_READER_SOURCE_DIR}/meshoptimizer")
if(ZENASSET_BUILD_PROCESSOR)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/Generated/MeshoptDecoder")
    zenasset_generate_meshopt_decoders("${meshoptimizer_SOURCE_DIR}" "${generated}")
    zenasset_verify_reader_sources("${generated}" "meshoptimizer/")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${meshoptimizer_SOURCE_DIR}/src/indexcodec.cpp" "${meshoptimizer_SOURCE_DIR}/src/vertexcodec.cpp")
endif()
add_library(ZenAssetMeshoptDecoder STATIC
    "${ZENASSET_MESHOPT_DECODER_DIRECTORY}/indexcodec.cpp"
    "${ZENASSET_MESHOPT_DECODER_DIRECTORY}/vertexcodec.cpp")
target_include_directories(ZenAssetMeshoptDecoder PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
set_target_properties(ZenAssetMeshoptDecoder PROPERTIES
    POSITION_INDEPENDENT_CODE ON EXPORT_NAME PrivateMeshoptDecoder)
install(TARGETS ZenAssetMeshoptDecoder EXPORT ZenAssetTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${ZENASSET_MESHOPT_DECODER_DIRECTORY}/LICENSE.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/ZenAsset/licenses/meshoptimizer)

if(ZENASSET_BUILD_PROCESSOR)
    # The upstream sequence encoder reads mutable process-global version state.
    # Retain its exact algorithm with a private, fixed v1 header so source-import
    # library users cannot race or silently change package output by calling the
    # upstream meshopt_encodeIndexVersion setter.
    file(READ "${meshoptimizer_SOURCE_DIR}/src/indexcodec.cpp" sequence_code)
    zenasset_select_meshopt_section(sequence_code "const unsigned char kSequenceHeader"
        "static int gEncodeIndexVersion" sequence_header)
    zenasset_select_meshopt_section(sequence_code "static void encodeVByte"
        "static unsigned int decodeVByte" sequence_helper)
    zenasset_select_meshopt_section(sequence_code "size_t meshopt_encodeIndexSequence("
        "int meshopt_decodeIndexSequence(" sequence_functions)
    string(REPLACE "int version = gEncodeIndexVersion;" "const int version = 1;"
        sequence_functions "${sequence_functions}")
    string(REPLACE "namespace meshopt" "namespace zenasset_meshopt_encoder_detail"
        sequence_functions "${sequence_functions}")
    string(REPLACE "meshopt_encodeIndexSequenceBound" "zenasset_meshopt_encodeIndexSequenceV1Bound"
        sequence_functions "${sequence_functions}")
    string(REPLACE "meshopt_encodeIndexSequence(" "zenasset_meshopt_encodeIndexSequenceV1("
        sequence_functions "${sequence_functions}")
    if(sequence_functions MATCHES "gEncode|meshopt_decode|namespace meshopt")
        message(FATAL_ERROR "Unexpected global state remains in private meshoptimizer sequence encoder")
    endif()
    set(sequence_output "// Generated from pinned meshoptimizer v1.1; see cmake/MeshoptDecoder.cmake.\n// This file is part of meshoptimizer library; see its installed MIT license.\n#include \"Codecs/MeshoptEncoder.h\"\nnamespace zenasset_meshopt_encoder_detail\n{\n${sequence_header}${sequence_helper}}\n${sequence_functions}")
    set(sequence_path "${CMAKE_CURRENT_BINARY_DIR}/Generated/MeshoptEncoder/indexsequence.cpp")
    file(CONFIGURE OUTPUT "${sequence_path}" CONTENT "${sequence_output}" @ONLY)
    add_library(ZenAssetMeshoptEncoder STATIC "${sequence_path}")
    target_include_directories(ZenAssetMeshoptEncoder PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
    set_target_properties(ZenAssetMeshoptEncoder PROPERTIES
        POSITION_INDEPENDENT_CODE ON EXPORT_NAME PrivateMeshoptEncoder)
endif()
