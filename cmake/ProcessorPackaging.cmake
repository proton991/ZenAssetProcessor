# Included after Processor.cmake has created the source-enabled target.
if(ZENASSET_INSTALL_PROCESSOR)
    foreach(target ZenAssetUfbx ZenAssetBasis ZenAssetStb)
        get_target_property(aliased_target ${target} ALIASED_TARGET)
        if(aliased_target)
            message(FATAL_ERROR "Installing Processor requires owned ${target} rather than a parent target alias. Use a standalone dependency build for distribution.")
        endif()
    endforeach()

    install(TARGETS ZenAssetProcessor ZenAssetUfbx ZenAssetBasis ZenAssetStb ZenAssetZstdEncoder ZenAssetMeshoptEncoder
        ZenAssetTinyObj ZenAssetEarcut
        EXPORT ZenAssetTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES ${ZENASSET_DEPENDENCY_ufbx_SOURCE}/ufbx.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ZenAsset/Dependencies/ufbx)
    install(FILES ${CMAKE_CURRENT_LIST_DIR}/ZenAssetProcessorDependencies.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZenAsset)
    install(FILES ${CMAKE_CURRENT_LIST_DIR}/../THIRD_PARTY_NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/ZenAsset)
    set(ZENASSET_PROCESSOR_CONFIG_DEPENDENCIES
        [=[include("${CMAKE_CURRENT_LIST_DIR}/ZenAssetProcessorDependencies.cmake")]=])

    function(zenasset_install_notices dependency)
        foreach(notice IN LISTS ARGN)
            set(source "${ZENASSET_DEPENDENCY_${dependency}_SOURCE}/${notice}")
            if(NOT EXISTS "${source}")
                message(FATAL_ERROR "Required ${dependency} redistribution notice is missing: ${source}")
            endif()
            install(FILES "${source}" DESTINATION ${CMAKE_INSTALL_DATADIR}/ZenAsset/licenses/${dependency})
        endforeach()
    endfunction()

    zenasset_install_notices(simdjson LICENSE LICENSE-MIT)
    zenasset_install_notices(fastgltf LICENSE.md)
    zenasset_install_notices(draco LICENSE AUTHORS)
    zenasset_install_notices(meshoptimizer LICENSE.md)
    zenasset_install_notices(libwebp COPYING PATENTS AUTHORS)
    zenasset_install_notices(ufbx LICENSE)
    zenasset_install_notices(basis_universal LICENSE README.md)
    install(DIRECTORY ${ZENASSET_DEPENDENCY_basis_universal_SOURCE}/LICENSES/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/ZenAsset/licenses/basis_universal/third-party)
    zenasset_install_notices(stb LICENSE)
    zenasset_install_notices(zenasset_tinyobj LICENSE)
    zenasset_install_notices(zenasset_earcut LICENSE)
endif()
