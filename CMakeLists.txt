cmake_minimum_required(VERSION 3.12)

include(cmake/VcpkgInit.cmake)

project(wows-model-exporter)

# Project version (semantic versioning)
set(wows-model-exporter_VERSION_MAJOR 0)
set(wows-model-exporter_VERSION_MINOR 1)
set(wows-model-exporter_VERSION_PATCH 0)
set(wows-model-exporter_VERSION
    ${wows-model-exporter_VERSION_MAJOR}.${wows-model-exporter_VERSION_MINOR}.${wows-model-exporter_VERSION_PATCH}
)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")

# ── Language standards ────────────────────────────────────────────────────────
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

# ── Options ───────────────────────────────────────────────────────────────────
# Feature
option(
  WITH_PYTHON
  "Build with embedded CPython support (required for GameParams/ship-export features)"
  ON)

# Bundled components — OFF means use system library; ON forces the submodule
# copy
option(
  BUNDLE_WOWS_DEPACK
  "Bundle wows-depack from the submodule instead of a system-installed version"
  OFF)
option(
  BUNDLE_TINYGLTF
  "Bundle tinygltf from the submodule instead of a system-installed version"
  OFF)
option(
  BUNDLE_ARGP
  "Bundle argp-standalone from the submodule (default: auto-detect, system first)"
  OFF)

# Build
option(DEBUG "Compile with debug symbols" OFF)
option(STATIC "Compile statically" OFF)
option(USE_CLANG "Build with clang" OFF)
option(USE_GCC "Build with gcc" OFF)
option(FORCELE "Force little-endian architecture" OFF)
option(COVERAGE "Enable code coverage" OFF)
option(BUILD_DOC "Build documentation" OFF)
option(BUILD_TESTS "Build tests" OFF)

# ── Compiler / linker selection ───────────────────────────────────────────────
if(USE_CLANG)
  set(CMAKE_CXX_COMPILER "clang++")
  set(CMAKE_CC_COMPILER "clang")
endif()
if(USE_GCC)
  set(CMAKE_CXX_COMPILER "g++")
  set(CMAKE_CC_COMPILER "gcc")
endif()

if(DEBUG)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g")
  set(CMAKE_BUILD_TYPE Debug)
endif()

if(STATIC)
  set(SHARED "")
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
  set(BUILD_SHARED_LIBRARIES OFF)
  set(CMAKE_EXE_LINKER_FLAGS "-static")
else()
  set(SHARED "SHARED")
endif()

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
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
  set(EXTERNAL_ICONV "iconv")
  add_definitions(-DDARWIN)
endif()

add_compile_definitions(BFD_VERSION="${wows-model-exporter_VERSION}")

# ── System dependencies ───────────────────────────────────────────────────────
include(cmake/SystemDeps.cmake)

# ── Portability shims (always bundled from local deps/) ───────────────────────
set(COMPAT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/compat)
set(MMAN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/mman-win32)

add_subdirectory(deps/fmem EXCLUDE_FROM_ALL)

# ── BUNDLE_ARGP — system first, bundled fallback ──────────────────────────────
if(NOT BUNDLE_ARGP)
  include(CheckIncludeFile)
  check_include_file("argp.h" HAVE_ARGP_H)
endif()
if(BUNDLE_ARGP OR NOT HAVE_ARGP_H)
  if(BUNDLE_ARGP)
    message(STATUS "Bundling argp-standalone (BUNDLE_ARGP=ON)")
  else()
    message(STATUS "System argp not found — using bundled argp-standalone")
  endif()
  add_subdirectory(deps/argp EXCLUDE_FROM_ALL)
  set(ARGP_LIBRARIES argp-standalone)
  set(ARGP_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/deps/argp/include)
  # GCC 15 + mingw-w64: intrinsic headers pulled via windows.h → winnt.h have
  # type errors in C compilation units; block the include chain for this target.
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
else()
  message(STATUS "Using system argp")
  find_library(_ARGP_LIB argp)
  if(_ARGP_LIB)
    set(ARGP_LIBRARIES ${_ARGP_LIB})
  else()
    set(ARGP_LIBRARIES "") # Linux: argp is in glibc, no separate lib needed
  endif()
  set(ARGP_INCLUDE_DIRS "")
endif()

# ── BUNDLE_TINYGLTF ───────────────────────────────────────────────────────────
if(BUNDLE_TINYGLTF)
  set(TINYGLTF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/tinygltf)
  if(NOT EXISTS "${TINYGLTF_DIR}/tiny_gltf.h")
    message(
      FATAL_ERROR
        "BUNDLE_TINYGLTF=ON but tiny_gltf.h not found in ${TINYGLTF_DIR}. "
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

# ── Include / link search paths ───────────────────────────────────────────────
link_directories(${CMAKE_BINARY_DIR}/ /usr/local/lib /usr/lib/
                 ${CMAKE_CURRENT_SOURCE_DIR}/../lib/)

if(WIN32)
  include_directories(BEFORE ${COMPAT_DIR})
endif()
include_directories(./inc/ ${ZLIB_INCLUDE_DIRS} ${PCRE_INCLUDE_DIRS}
                    ${CMAKE_CURRENT_SOURCE_DIR}/deps ${COMPAT_DIR})
if(NOT CMAKE_CROSSCOMPILING)
  include_directories(/usr/include/ /sw/include/)
endif()

# ── wows-geometry library ─────────────────────────────────────────────────────
file(GLOB WOWS_SOURCES lib/*.c lib/*.cpp)
if(NOT WITH_PYTHON)
  list(FILTER WOWS_SOURCES EXCLUDE REGEX ".*/game_params\\.cpp$")
  list(FILTER WOWS_SOURCES EXCLUDE REGEX ".*/ship_export\\.cpp$")
endif()

add_library(wows-geometry ${SHARED} ${WOWS_SOURCES})
set_target_properties(
  wows-geometry PROPERTIES VERSION ${wows-model-exporter_VERSION}
                           SOVERSION ${wows-model-exporter_VERSION_MAJOR}
                           WINDOWS_EXPORT_ALL_SYMBOLS ON)

if(WITH_PYTHON)
  target_compile_definitions(wows-geometry PRIVATE WOWS_WITH_PYTHON)
endif()
if(BUNDLE_TINYGLTF)
  target_compile_definitions(wows-geometry PRIVATE WOWS_DDS_NO_STB_WRITE)
endif()

target_link_libraries(wows-geometry PRIVATE fmem::fmem)

if(WIN32)
  target_sources(wows-geometry PRIVATE ${COMPAT_DIR}/posix_compat.c
                                       ${MMAN_DIR}/mmap.c)
  target_include_directories(wows-geometry BEFORE PRIVATE ${COMPAT_DIR}
                                                          ${MMAN_DIR})
else()
  target_compile_definitions(wows-geometry PRIVATE _GNU_SOURCE)
endif()

# ── BUNDLE_WOWS_DEPACK ────────────────────────────────────────────────────────
if(BUNDLE_WOWS_DEPACK)
  set(WOWS_DEPACK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/wows-depack)
  file(GLOB WOWS_DEPACK_SOURCES ${WOWS_DEPACK_DIR}/lib/*.c)
  if(NOT WOWS_DEPACK_SOURCES)
    message(
      FATAL_ERROR
        "BUNDLE_WOWS_DEPACK=ON but no sources found in ${WOWS_DEPACK_DIR}/lib. "
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

# ── Executables ───────────────────────────────────────────────────────────────
add_executable(wows-geometry-cli wows-geometry.c)
target_include_directories(wows-geometry-cli PRIVATE ${ARGP_INCLUDE_DIRS})
if(WIN32)
  target_include_directories(wows-geometry-cli PRIVATE ${MMAN_DIR})
endif()
target_link_libraries(wows-geometry-cli PRIVATE wows-geometry ${ARGP_LIBRARIES})

if(WITH_PYTHON)
  add_executable(wows-gltf-exporter wows-gltf-exporter.cpp)
  target_include_directories(wows-gltf-exporter PRIVATE ${ARGP_INCLUDE_DIRS})
  target_link_libraries(wows-gltf-exporter PRIVATE wows-geometry
                                                   ${TINYGLTF_LIBRARIES}
                                                   ${ARGP_LIBRARIES})

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
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
  find_library(CUNIT_LIBRARY cunit)
  if(CUNIT_LIBRARY)
    include(CTest)
    add_executable(wows-geometry-test tests/tests.c)
    target_link_libraries(wows-geometry-test PRIVATE wows-geometry
                                                     ${CUNIT_LIBRARY})
    add_test(NAME wows-geometry-test COMMAND wows-geometry-test)
    add_custom_target(
      tests
      COMMAND wows-geometry-test
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Running tests")
    add_dependencies(tests wows-geometry-test)

    if(COVERAGE)
      set(CMAKE_BUILD_TYPE Debug)
      set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}   -fprofile-arcs -ftest-coverage")
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
    endif()
  else()
    message(WARNING "CUnit not found; not building tests.")
  endif()
endif()

# ── Install ───────────────────────────────────────────────────────────────────
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
install(FILES inc/wows-geometry.h DESTINATION ${INCLUDE_INSTALL_DIR})

# ── Documentation ─────────────────────────────────────────────────────────────
if(BUILD_DOC)
  find_package(Doxygen)
  if(DOXYGEN_FOUND)
    set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in)
    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
    configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)
    add_custom_target(
      doc_doxygen ALL
      COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      COMMENT "Generating API documentation with Doxygen"
      VERBATIM)
  else()
    message("Doxygen must be installed to generate documentation")
  endif()
endif()

# ── Release tagging ───────────────────────────────────────────────────────────
add_custom_target(
  tag
  COMMAND git tag -a ${wows-model-exporter_VERSION} -m
          "tagging version ${wows-model-exporter_VERSION}"
  COMMAND git push origin ${wows-model-exporter_VERSION})
