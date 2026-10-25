# meshoptimizer 1.1 ships encoders and decoders in the same translation units.
# Generate decoder-only copies using exact, reviewed sections of the pinned
# upstream files. Check both the complete source hash and every section marker;
# dependency upgrades require explicitly reviewing this small extraction map.
# The original fetched sources are never changed.
function(zenasset_meshopt_section_bounds variable first last)
    string(FIND "${${variable}}" "${first}" begin)
    string(FIND "${${variable}}" "${first}" duplicate REVERSE)
    string(FIND "${${variable}}" "${last}" end)
    string(FIND "${${variable}}" "${last}" duplicate_end REVERSE)
    if(begin LESS 0 OR end LESS begin OR NOT begin EQUAL duplicate OR NOT end EQUAL duplicate_end)
        message(FATAL_ERROR "Pinned meshoptimizer extraction markers changed: ${first} -> ${last}")
    endif()
    set(begin ${begin} PARENT_SCOPE)
    set(end ${end} PARENT_SCOPE)
endfunction()

function(zenasset_generate_meshopt_decoders source_directory output_directory)
    zenasset_generate_meshopt_decoder("${source_directory}/src/indexcodec.cpp"
        "${output_directory}/indexcodec.cpp"
        f98c84b7ee1bc15b075fdba430b8c2d69b21ff9ff7e782b9ff84a6db8f8c8e19)
    zenasset_generate_meshopt_decoder("${source_directory}/src/vertexcodec.cpp"
        "${output_directory}/vertexcodec.cpp"
        6e76ae9946f6a550f5a357d1a365e8abad4489cea533b6973e054b4fc8a85f6b)
    file(COPY_FILE "${source_directory}/LICENSE.md" "${output_directory}/LICENSE.md" ONLY_IF_DIFFERENT)
endfunction()

function(zenasset_remove_meshopt_section variable first last)
    zenasset_meshopt_section_bounds(${variable} "${first}" "${last}")
    string(SUBSTRING "${${variable}}" 0 ${begin} prefix)
    string(SUBSTRING "${${variable}}" ${end} -1 suffix)
    set(${variable} "${prefix}${suffix}" PARENT_SCOPE)
endfunction()

function(zenasset_select_meshopt_section variable first last output)
    zenasset_meshopt_section_bounds(${variable} "${first}" "${last}")
    math(EXPR size "${end} - ${begin}")
    string(SUBSTRING "${${variable}}" ${begin} ${size} section)
    set(${output} "${section}" PARENT_SCOPE)
endfunction()

function(zenasset_generate_meshopt_decoder source output hash)
    file(SHA256 "${source}" actual_hash)
    if(NOT actual_hash STREQUAL hash)
        message(FATAL_ERROR "meshoptimizer decoder split requires pristine pinned v1.1 source: ${source}")
    endif()
    file(READ "${source}" code)
    get_filename_component(name "${source}" NAME)
    if(name STREQUAL "indexcodec.cpp")
        zenasset_remove_meshopt_section(code "static int gEncodeIndexVersion" "const int kDecodeIndexVersion")
        zenasset_remove_meshopt_section(code "static const unsigned char kCodeAuxEncodingTable" "static void pushEdgeFifo")
        zenasset_remove_meshopt_section(code "static int getVertexFifo" "static void pushVertexFifo")
        zenasset_remove_meshopt_section(code "static void encodeVByte" "static unsigned int decodeVByte")
        zenasset_remove_meshopt_section(code "static void encodeIndex" "static unsigned int decodeIndex")
        zenasset_remove_meshopt_section(code "static int getCodeAuxIndex" "static void writeTriangle")
        zenasset_remove_meshopt_section(code "size_t meshopt_encodeIndexBuffer(" "int meshopt_decodeIndexVersion(")
        zenasset_remove_meshopt_section(code "size_t meshopt_encodeIndexSequence(" "int meshopt_decodeIndexSequence(")
    elseif(name STREQUAL "vertexcodec.cpp")
        zenasset_remove_meshopt_section(code "static int gEncodeVertexVersion" "const int kDecodeVertexVersion")
        zenasset_remove_meshopt_section(code "const int kEncodeDefaultLevel" "static size_t getVertexBlockSize")
        zenasset_remove_meshopt_section(code "template <typename T>\ninline T zigzag" "template <typename T>\ninline T unzigzag")
        zenasset_remove_meshopt_section(code "#if TRACE\nstruct Stats" "#if defined(SIMD_FALLBACK) || (!defined(SIMD_SSE)")
        zenasset_remove_meshopt_section(code "size_t meshopt_encodeVertexBufferLevel(" "int meshopt_decodeVertexVersion(")
    else()
        message(FATAL_ERROR "Unexpected meshoptimizer decoder source ${name}")
    endif()
    # Use a minimal private declaration header so generated objects do not even
    # include upstream encoder declarations or inline encoder wrappers.
    string(REPLACE "#include \"meshoptimizer.h\"" "#include \"Codecs/MeshoptDecoder.h\"" code "${code}")
    string(REPLACE "namespace meshopt" "namespace zenasset_meshopt_detail" code "${code}")
    string(REPLACE "meshopt::" "zenasset_meshopt_detail::" code "${code}")
    string(REPLACE "meshopt_decode" "zenasset_meshopt_decode" code "${code}")
    if(code MATCHES "meshopt_encode|gEncode|kEncode|encodeBytes|encodeDeltas|encodeVertexBlock|encodeVByte|encodeIndex\\(")
        message(FATAL_ERROR "Encoder definitions remain in generated meshoptimizer ${name}")
    endif()
    set(code "// Generated from pinned meshoptimizer v1.1; see cmake/MeshoptDecoder.cmake.\n${code}")
    file(CONFIGURE OUTPUT "${output}" CONTENT "${code}" @ONLY NEWLINE_STYLE UNIX)
endfunction()

