include(${VCPKG_ROOT_DIR}/scripts/cmake/vcpkg_from_github.cmake)
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO rayokota/jsonata-cpp
    REF v0.1.3
    SHA512 6aad513bb2bae13659b8b7eb5175f445fce3a55872edd41bced2d6e647591b1c20d7a85885bf1c6260897d90394b5b251975f20958c74bbdbcfc597a2277bd10
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
