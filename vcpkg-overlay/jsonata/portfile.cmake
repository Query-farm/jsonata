include(${VCPKG_ROOT_DIR}/scripts/cmake/vcpkg_from_github.cmake)
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO rayokota/jsonata-cpp
    REF v0.1.1
    SHA512 d2e64e1cce5d6db93f15913b6b8ccdc199c1be2e02247715d048dd1ae3322629db88f2361e73e705491606437829c86ae1c0256b274631de52f8b5081bddc429
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
