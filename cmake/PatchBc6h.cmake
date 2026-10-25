# Generate a corrected build-local copy; never modify the fetched dependency.
# The scalar MapColorsQuantized path loaded uninitialized INTColor::pad and
# included it in its RGB error. Norm reads exactly RGB in every configuration.
# Default integer colors must also define pad before implicit endpoint assignment.
# RoughMSE also passed an uninitialized alpha through INTColor::Set.
if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "BC6H patch requires INPUT and OUTPUT")
endif()
file(SHA256 "${INPUT}" upstream_hash)
if(NOT upstream_hash STREQUAL "43fdc8b6b9e13d700298781e389665d62b9043c34d25df93ede82f8940f03c6d")
    message(FATAL_ERROR "Unexpected bc6h_enc.h revision; review the local RGB/padding correction before updating its hash")
endif()
file(READ "${INPUT}" header)
set(before [=[        const XMVECTOR vcolors = XMLoadSInt4(reinterpret_cast<const XMINT4*>(&aColors[i]));

        // Compute ErrorMetricRGB
        XMVECTOR tpal  = XMLoadSInt4(reinterpret_cast<const XMINT4*>(&aPalette[0]));
        tpal           = XMVectorSubtract(vcolors, tpal);
        float fBestErr = XMVectorDot(tpal, tpal);]=])
set(after [=[        // RGB only: INTColor padding is not a color component.
        float fBestErr = Norm(aColors[i], aPalette[0]);]=])
string(REPLACE "${before}" "${after}" header "${header}")
set(before [=[            tpal             = XMLoadSInt4(reinterpret_cast<const XMINT4*>(&aPalette[j]));
            tpal             = XMVectorSubtract(vcolors, tpal);
            const float fErr = XMVectorDot(tpal, tpal);]=])
set(after [=[            const float fErr = Norm(aColors[i], aPalette[j]);]=])
string(REPLACE "${before}" "${after}" header "${header}")
string(REPLACE "    INTColor() = default;" "    INTColor() noexcept : r(0), g(0), b(0), pad(0) {}" header "${header}")
string(REPLACE "        HDRColorA epA, epB;" "        HDRColorA epA{}, epB{};" header "${header}")
set(header "// ZenAsset build-local correction: RGB-only palette error and initialized integer/endpoint lanes.\n${header}")
string(SHA256 patched_hash "${header}")
if(NOT patched_hash STREQUAL "cc3620f2d38d7465ce1359d3c1eddd4cf49cf5e92937d5187fdbf030a64011b7")
    message(FATAL_ERROR "BC6H local correction did not produce the reviewed header")
endif()
set(previous "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" previous)
endif()
if(NOT previous STREQUAL header)
    get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_directory}")
    file(WRITE "${OUTPUT}" "${header}")
endif()
