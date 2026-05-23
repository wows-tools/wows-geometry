cmake_minimum_required(VERSION 3.12)

# Windows: use bundled vcpkg submodule (manifest mode installs deps on
# configure).
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

project(wows-model-exporter)

# Project version (sematic versionning)
set(wows-model-exporter_VERSION_MAJOR 0)
set(wows-model-exporter_VERSION_MINOR 1)
set(wows-model-exporter_VERSION_PATCH 0)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W3 /D_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall)
endif()

option(
  WITH_PYTHON
  "Build with embedded CPython support (required for GameParams/ship-export features)"
  ON)

find_library(CUNIT_LIBRARY cunit)

if(CMAKE_TOOLCHAIN_FILE MATCHES "[Vv]cpkg")
  find_package(ZLIB REQUIRED)
  find_package(pcre2 CONFIG REQUIRED)
  find_package(meshoptimizer CONFIG REQUIRED)
  set(MESHOPTIMIZER_LIBRARIES meshoptimizer::meshoptimizer)
  if(TARGET pcre2::pcre2-8)
    set(PCRE_LIBRARIES pcre2::pcre2-8)
  elseif(TARGET pcre2::pcre2-8-static)
    set(PCRE_LIBRARIES pcre2::pcre2-8-static)
  elseif(TARGET PCRE2::8BIT)
    set(PCRE_LIBRARIES PCRE2::8BIT)
  elseif(TARGET pcre2::pcre2)
    set(PCRE_LIBRARIES pcre2::pcre2)
  elseif(TARGET unofficial::pcre2::pcre2-8)
    set(PCRE_LIBRARIES unofficial::pcre2::pcre2-8)
  else()
    message(FATAL_ERROR "pcre2 found but no known CMake target")
  endif()
  if(WITH_PYTHON)
    find_package(
      Python3
      COMPONENTS Interpreter Development
      REQUIRED)
  endif()
else()
  find_package(meshoptimizer CONFIG QUIET)
  if(meshoptimizer_FOUND)
    set(MESHOPTIMIZER_LIBRARIES meshoptimizer::meshoptimizer)
  else()
    find_library(MESHOPTIMIZER_LIBRARY meshoptimizer REQUIRED)
    set(MESHOPTIMIZER_LIBRARIES ${MESHOPTIMIZER_LIBRARY})
  endif()
  find_package(ZLIB REQUIRED)
  find_library(PCRE_LIBRARIES NAMES pcre2-8 pcre pcre2 REQUIRED)
  if(WITH_PYTHON)
    find_package(
      Python3
      COMPONENTS Interpreter Development
      REQUIRED)
  endif()
endif()

# FindPython3 imported targets (Python3::Python) require CMake 3.19+; fall back
# on older CMake or minimal python3-dev pkg-config layouts.
if(WITH_PYTHON)
  if(TARGET Python3::Python)
    set(WOWS_PYTHON_LIBS Python3::Python)
  elseif(TARGET Python3::Module)
    set(WOWS_PYTHON_LIBS Python3::Module)
  elseif(Python3_LIBRARIES)
    add_library(wows_python INTERFACE)
    target_link_libraries(wows_python INTERFACE ${Python3_LIBRARIES})
    target_include_directories(wows_python INTERFACE ${Python3_INCLUDE_DIRS})
    set(WOWS_PYTHON_LIBS wows_python)
  else()
    message(FATAL_ERROR "Python3 development libraries not found")
  endif()
else()
  set(WOWS_PYTHON_LIBS "")
endif()

set(wows-model-exporter_VERSION
    ${wows-model-exporter_VERSION_MAJOR}.${wows-model-exporter_VERSION_MINOR}.${wows-model-exporter_VERSION_PATCH}
)

add_custom_target(
  tag
  COMMAND git tag -a ${wows-model-exporter_VERSION} -m
          "tagging version ${wows-model-exporter_VERSION}"
  COMMAND git push origin ${wows-model-exporter_VERSION})

# set version as a definition
set(CMAKE_C_FLAGS
    "${CMAKE_CXX_FLAGS} -DBFD_VERSION='\"${wows-model-exporter_VERSION}\"'")
set(CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -DBFD_VERSION='\"${wows-model-exporter_VERSION}\"'")

# Options
option(DEBUG "compile with debug symbol" OFF)
option(STATIC "compile statically" OFF)
option(USE_CLANG "build application with clang" OFF)
option(USE_GCC "build application with gcc" OFF)
option(FORCELE "force little endian architecture" OFF)
option(COVERAGE "Enable code coverage" OFF)
option(BUILD_DOC "Build documentation" OFF)
option(BUILD_TESTS "Build tests" OFF)
option(
  BUNDLE_WOWS_DEPACK
  "Bundle wows-depack from the submodule instead of using a system-installed version"
  OFF)
option(
  BUNDLE_TINYGLTF
  "Bundle tinygltf from the submodule instead of using a system-installed version"
  OFF)

if(USE_CLANG)
  set(CMAKE_CXX_COMPILER "clang++")
  set(CMAKE_CC_COMPILER "clang")
endif(USE_CLANG)

if(USE_GCC)
  set(CMAKE_CXX_COMPILER "g++")
  set(CMAKE_CC_COMPILER "gcc")
endif(USE_GCC)

if(DEBUG)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g")
  set(CMAKE_BUILD_TYPE Debug)
endif(DEBUG)

if(STATIC)
  set(SHARED "")
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
  set(BUILD_SHARED_LIBRARIES OFF)
  set(CMAKE_EXE_LINKER_FLAGS "-static")
else(STATIC)
  set(SHARED "SHARED")
endif(STATIC)

# gcov/lcov and libgcov do not mix with Clang's LLVM profiling in the same link.
if(COVERAGE AND BUILD_TESTS)
  if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR NOT CMAKE_CXX_COMPILER_ID
                                               STREQUAL "GNU")
    message(
      FATAL_ERROR
        "COVERAGE with BUILD_TESTS requires GCC for both C and CXX (found "
        "C=${CMAKE_C_COMPILER_ID}, CXX=${CMAKE_CXX_COMPILER_ID}). "
        "Reconfigure with e.g. -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ "
        "or -DUSE_GCC=ON.")
  endif()
endif()

if(UNIX)
  link_libraries(m)
endif(UNIX)

# Build external dependancies if we are on OSX
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  # Mac OS X specific code
  set(EXTERNAL_ICONV "iconv")
  add_definitions(-DDARWIN)
endif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")

# ── Bundled portability deps (local deps/ — independent of optional submodules)
# ──

# compat: endian macros on all platforms + POSIX shims on Windows
set(COMPAT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/compat)

# fmem: portable open_memstream — selects the right backend per platform
add_subdirectory(deps/fmem EXCLUDE_FROM_ALL)

# mman-win32: mmap/munmap shim for Windows
set(MMAN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/mman-win32)

# argp: use system argp where available, fall back to bundled argp-standalone
include(CheckIncludeFile)
check_include_file("argp.h" HAVE_ARGP_H)
if(HAVE_ARGP_H)
  # On Linux argp lives in glibc (no separate lib); macOS/brew needs -largp
  find_library(_ARGP_LIB argp)
  if(_ARGP_LIB)
    set(ARGP_LIBRARIES ${_ARGP_LIB})
  else()
    set(ARGP_LIBRARIES "")
  endif()
  set(ARGP_INCLUDE_DIRS "")
  message(STATUS "Using system argp")
else()
  message(STATUS "System argp not found, building bundled argp-standalone")
  add_subdirectory(deps/argp EXCLUDE_FROM_ALL)
  set(ARGP_LIBRARIES argp-standalone)
  set(ARGP_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/deps/argp/include)
  # GCC 15 + mingw-w64: intrinsic headers pulled via windows.h → winnt.h have
  # type errors in C compilation units. Block the chain for this C-only target.
  if(WIN32 AND NOT MSVC)
    target_compile_definitions(
      argp-standalone
      PRIVATE _X86INTRIN_H_INCLUDED
              _MMINTRIN_H_INCLUDED
              _XMMINTRIN_H_INCLUDED
              _EMMINTRIN_H_INCLUDED
              _PMMINTRIN_H_INCLUDED
              _TMMINTRIN_H_INCLUDED
              _SMMINTRIN_H_INCLUDED
              _NMMINTRIN_H_INCLUDED
              _WMMINTRIN_H_INCLUDED
              _IMMINTRIN_H_INCLUDED)
  endif()
endif()

# linking directories
link_directories(${CMAKE_BINARY_DIR}/ /usr/local/lib /usr/lib/
                 ${CMAKE_CURRENT_SOURCE_DIR}/../lib/)

# headers directories (compat dir is global so bundled depack lib/*.c find it
# too)
if(WIN32)
  include_directories(BEFORE ${COMPAT_DIR})
endif()
include_directories(./inc/ ${ZLIB_INCLUDE_DIRS} ${PCRE_INCLUDE_DIRS}
                    ${CMAKE_CURRENT_SOURCE_DIR}/deps ${COMPAT_DIR})
# Skip host system paths when cross-compiling (they belong to the host, not
# target)
if(NOT CMAKE_CROSSCOMPILING)
  include_directories(/usr/include/ /sw/include/)
endif()

file(GLOB WOWS_SOURCES lib/*.c lib/*.cpp)
if(NOT WITH_PYTHON)
  list(FILTER WOWS_SOURCES EXCLUDE REGEX ".*/game_params\\.cpp$")
  list(FILTER WOWS_SOURCES EXCLUDE REGEX ".*/ship_export\\.cpp$")
endif()
add_library(wows-geometry ${SHARED} ${WOWS_SOURCES})
if(WITH_PYTHON)
  target_compile_definitions(wows-geometry PRIVATE WOWS_WITH_PYTHON)
endif()
if(BUNDLE_TINYGLTF)
  target_compile_definitions(wows-geometry PRIVATE WOWS_DDS_NO_STB_WRITE)
endif()
set_target_properties(
  wows-geometry PROPERTIES VERSION ${wows-model-exporter_VERSION}
                           SOVERSION ${wows-model-exporter_VERSION_MAJOR})

# Compile the executable
add_executable(wows-geometry-cli wows-geometry.c)
# Portability shims — always from local deps/, regardless of BUNDLE_WOWS_DEPACK
target_link_libraries(wows-geometry PRIVATE fmem::fmem)
if(WIN32)
  target_sources(wows-geometry PRIVATE ${COMPAT_DIR}/posix_compat.c
                                       ${MMAN_DIR}/mmap.c)
  # Prefer deps/compat/fcntl.h and posix_compat.h over the UCRT headers.
  target_include_directories(wows-geometry BEFORE PRIVATE ${COMPAT_DIR}
                                                        ${MMAN_DIR})
else()
  target_compile_definitions(wows-geometry PRIVATE _GNU_SOURCE)
endif()

if(BUNDLE_TINYGLTF)
  set(TINYGLTF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/tinygltf)
  if(NOT EXISTS "${TINYGLTF_DIR}/tiny_gltf.h")
    message(
      FATAL_ERROR
        "BUNDLE_TINYGLTF=ON but tiny_gltf.h not found in ${TINYGLTF_DIR}. "
        "Did you forget to initialize the submodule? "
        "Run: git submodule update --init deps/tinygltf")
  endif()
  add_library(tinygltf_bundled STATIC
              ${CMAKE_CURRENT_SOURCE_DIR}/deps/bundled/tinygltf_impl.cpp)
  target_include_directories(tinygltf_bundled PUBLIC ${TINYGLTF_DIR})
  set_target_properties(tinygltf_bundled PROPERTIES POSITION_INDEPENDENT_CODE
                                                    ON)
  set(TINYGLTF_LIBRARIES tinygltf_bundled)
else()
  find_package(TinyGLTF CONFIG QUIET)
  if(TinyGLTF_FOUND)
    set(TINYGLTF_LIBRARIES TinyGLTF::TinyGLTF)
  else()
    find_package(tinygltf CONFIG REQUIRED)
    if(TARGET unofficial::tinygltf::tinygltf)
      set(TINYGLTF_LIBRARIES unofficial::tinygltf::tinygltf)
    else()
      set(TINYGLTF_LIBRARIES tinygltf::tinygltf)
    endif()
  endif()
endif()

if(BUNDLE_WOWS_DEPACK)
  # Build depack sources directly (unmodified submodule).  Windows POSIX shims
  # (fcntl, dirent, mmap, S_ISDIR/S_ISREG, endian, …) come from deps/compat/ in
  # this repo — not from deps/wows-depack/deps/.
  set(WOWS_DEPACK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/wows-depack)
  file(GLOB WOWS_DEPACK_SOURCES ${WOWS_DEPACK_DIR}/lib/*.c)
  if(NOT WOWS_DEPACK_SOURCES)
    message(
      FATAL_ERROR
        "BUNDLE_WOWS_DEPACK=ON but no sources found in ${WOWS_DEPACK_DIR}/lib. "
        "Did you forget to initialize the submodule? "
        "Run: git submodule update --init --recursive")
  endif()
  target_sources(wows-geometry PRIVATE ${WOWS_DEPACK_SOURCES})
  target_include_directories(wows-geometry PRIVATE ${WOWS_DEPACK_DIR}/inc)
else()
  find_package(WowsDepack)
  if(WowsDepack_FOUND)
    target_link_libraries(wows-geometry PUBLIC WowsDepack::WowsDepack)
  elseif(NOT BUILD_DOC)
    message(
      FATAL_ERROR
        "WowsDepack not found. Install the library or set -DBUNDLE_WOWS_DEPACK=ON."
    )
  else()
    message(
      WARNING "WowsDepack not found; wows-geometry compilation will fail. "
              "This is expected for doc-only builds (make doc_doxygen).")
  endif()
endif()

target_link_libraries(
  wows-geometry PUBLIC ZLIB::ZLIB ${PCRE_LIBRARIES} ${MESHOPTIMIZER_LIBRARIES}
                       ${TINYGLTF_LIBRARIES} ${WOWS_PYTHON_LIBS})

target_include_directories(wows-geometry-cli PRIVATE ${ARGP_INCLUDE_DIRS})
if(WIN32)
  target_include_directories(wows-geometry-cli PRIVATE ${MMAN_DIR})
endif()
target_link_libraries(wows-geometry-cli PRIVATE wows-geometry ${ARGP_LIBRARIES})

if(WITH_PYTHON)
  # Ship stitcher: GameParams via libpython, geometry via libwows-geometry
  add_executable(wows-gltf-exporter wows-gltf-exporter.cpp)
  target_link_libraries(wows-gltf-exporter PRIVATE wows-geometry
                                                   ${TINYGLTF_LIBRARIES})

  # Ship lister: enumerate ships from GameParams.data (uses wows-depack API
  # directly)
  add_executable(wows-list-ships wows-list-ships.cpp)
  target_include_directories(wows-list-ships PRIVATE ${ARGP_INCLUDE_DIRS})
  if(BUNDLE_WOWS_DEPACK)
    target_include_directories(wows-list-ships PRIVATE ${WOWS_DEPACK_DIR}/inc)
    if(WIN32)
      target_include_directories(wows-list-ships PRIVATE ${MMAN_DIR})
    endif()
  endif()
  target_link_libraries(wows-list-ships PRIVATE wows-geometry fmem::fmem
                                                ${ARGP_LIBRARIES})
endif(WITH_PYTHON)

if(BUILD_TESTS)
  if(CUNIT_LIBRARY)

    include(CTest)
    add_test(NAME wows-geometry-test COMMAND wows-geometry-test)
    add_executable(wows-geometry-test tests/tests.c)
    target_link_libraries(wows-geometry-test PRIVATE wows-geometry
                                                     ${CUNIT_LIBRARY})

    # Add the test target
    add_custom_target(
      tests
      COMMAND wows-geometry-test
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Running tests")
    add_dependencies(tests wows-geometry-test)

    if(COVERAGE)
      set(CMAKE_BUILD_TYPE Debug)
      set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
      include(ProcessorCount)
      ProcessorCount(n_cores)
      include(CodeCoverage)
      setup_target_for_coverage(
        NAME
        coverage
        EXECUTABLE
        ctest
        -j
        ${n_cores}
        DEPENDENCIES
        wows-geometry-test)

      add_dependencies(coverage wows-geometry-test)
    endif(COVERAGE)

  else(CUNIT_LIBRARY)
    message(WARNING "CUnit not found; not building tests.")
  endif(CUNIT_LIBRARY)
endif(BUILD_TESTS)

if(NOT LIB_INSTALL_DIR)
  if(WIN32)
    set(LIB_INSTALL_DIR ".")
  else()
    set(LIB_INSTALL_DIR lib)
  endif()
endif()

if(NOT BIN_INSTALL_DIR)
  if(WIN32)
    set(BIN_INSTALL_DIR ".")
  else()
    set(BIN_INSTALL_DIR bin)
  endif()
endif()

if(NOT INCLUDE_INSTALL_DIR)
  set(INCLUDE_INSTALL_DIR include)
endif()

# install binaries and library
if(WITH_PYTHON)
  set(_INSTALL_PYTHON_TARGETS wows-gltf-exporter wows-list-ships)
else()
  set(_INSTALL_PYTHON_TARGETS "")
endif()
install(
  TARGETS wows-geometry wows-geometry-cli ${_INSTALL_PYTHON_TARGETS}
  RUNTIME DESTINATION ${BIN_INSTALL_DIR}
  LIBRARY DESTINATION ${LIB_INSTALL_DIR}
  ARCHIVE DESTINATION ${LIB_INSTALL_DIR})

# check if Doxygen is installed
if(BUILD_DOC)
  # set input and output files
  find_package(Doxygen)
  if(DOXYGEN_FOUND)
    set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in)
    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

    # request to configure the file
    configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)
    message("Doxygen build started")

    # note the option ALL which allows to build the docs together with the
    # application
    add_custom_target(
      doc_doxygen ALL
      COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      COMMENT "Generating API documentation with Doxygen"
      VERBATIM)
  else(DOXYGEN_FOUND)
    message(
      "Doxygen need to be installed to generate the doxygen documentation")
  endif(DOXYGEN_FOUND)
endif(BUILD_DOC)

# install header file
install(FILES inc/wows-geometry.h DESTINATION ${INCLUDE_INSTALL_DIR})
