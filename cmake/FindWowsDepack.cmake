# Look for the wows-depack library and headers

find_path(WowsDepack_INCLUDE_DIR wows-depack.h)
find_library(WowsDepack_LIBRARY NAMES wows-depack)

if(WowsDepack_INCLUDE_DIR AND WowsDepack_LIBRARY)
  set(WowsDepack_FOUND TRUE)
endif()

if(WowsDepack_FOUND)
  message(
    STATUS "Found WowsDepack: ${WowsDepack_INCLUDE_DIR}, ${WowsDepack_LIBRARY}")
else()
  message(STATUS "Could not find WowsDepack")
endif()

if(WowsDepack_FOUND)
  add_library(WowsDepack::WowsDepack INTERFACE IMPORTED)
  target_include_directories(WowsDepack::WowsDepack
                             INTERFACE ${WowsDepack_INCLUDE_DIR})
  target_link_libraries(WowsDepack::WowsDepack INTERFACE ${WowsDepack_LIBRARY})
endif()
