include(${VCPKG_ROOT_DIR}/scripts/cmake/vcpkg_from_github.cmake)
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO rayokota/jsonata-cpp
    REF v0.1.2
    SHA512 1ce5e905652baac7b73d8d2a8f5be070ebc669a44153c1651f1ac97026c81f5dddeff6db275564abfdb442bfe324cf64d0f7bca16ba5508051fd45d381c46f63
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
