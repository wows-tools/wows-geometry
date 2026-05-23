# Automatically activate the bundled vcpkg toolchain when the submodule is
# present and no explicit toolchain was given. Must be included before
# project().
set(_vcpkg_toolchain
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/vcpkg/scripts/buildsystems/vcpkg.cmake")
if(WIN32
   AND NOT CMAKE_TOOLCHAIN_FILE
   AND EXISTS "${_vcpkg_toolchain}")
  set(CMAKE_TOOLCHAIN_FILE
      "${_vcpkg_toolchain}"
      CACHE FILEPATH "Vcpkg toolchain file")
endif()
unset(_vcpkg_toolchain)
