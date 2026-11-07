include(${VCPKG_ROOT_DIR}/scripts/cmake/vcpkg_from_github.cmake)
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO rayokota/jsonata-cpp
    REF v0.3.0
    SHA512 0a351b02259bf2fb37df96bbdc0da618d349aae4ae812e7c84187a071d1ade4718464c854725096e30a2ca791fbf46671db4cdc986bb979c3f0d3ac7d10f061d
    PATCHES
        # Utils::convertNulls threw (and caught) std::bad_any_cast twice for every scalar result
        convert-nulls-without-exceptions.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
)

vcpkg_cmake_build()
vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/jsonata)

# There can be some empty include directories
set(VCPKG_POLICY_ALLOW_EMPTY_FOLDERS enabled)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
