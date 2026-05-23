# Find system (or vcpkg-provided) external dependencies. Expects WITH_PYTHON to
# be set before inclusion. Exports: PCRE_LIBRARIES, MESHOPTIMIZER_LIBRARIES,
# WOWS_PYTHON_LIBS

set(_vcpkg_build OFF)
if(CMAKE_TOOLCHAIN_FILE MATCHES "[Vv]cpkg")
  set(_vcpkg_build ON)
endif()

# ZLIB — identical under both vcpkg and system
find_package(ZLIB REQUIRED)

# PCRE2
if(_vcpkg_build)
  find_package(pcre2 CONFIG REQUIRED)
  foreach(_t pcre2::pcre2-8 pcre2::pcre2-8-static PCRE2::8BIT pcre2::pcre2
             unofficial::pcre2::pcre2-8)
    if(TARGET ${_t})
      set(PCRE_LIBRARIES ${_t})
      break()
    endif()
  endforeach()
  if(NOT PCRE_LIBRARIES)
    message(FATAL_ERROR "pcre2 found via vcpkg but no known CMake target")
  endif()
else()
  find_library(PCRE_LIBRARIES NAMES pcre2-8 pcre pcre2 REQUIRED)
endif()

# meshoptimizer
if(_vcpkg_build)
  find_package(meshoptimizer CONFIG REQUIRED)
  set(MESHOPTIMIZER_LIBRARIES meshoptimizer::meshoptimizer)
else()
  find_package(meshoptimizer CONFIG QUIET)
  if(meshoptimizer_FOUND)
    set(MESHOPTIMIZER_LIBRARIES meshoptimizer::meshoptimizer)
  else()
    find_library(MESHOPTIMIZER_LIBRARY meshoptimizer REQUIRED)
    set(MESHOPTIMIZER_LIBRARIES ${MESHOPTIMIZER_LIBRARY})
  endif()
endif()

# Python3 (optional feature — controlled by WITH_PYTHON)
if(WITH_PYTHON)
  find_package(
    Python3
    COMPONENTS Interpreter Development
    REQUIRED)
  # FindPython3 imported targets require CMake 3.19+; fall back for older CMake
  # or minimal python3-dev pkg-config layouts.
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

unset(_vcpkg_build)
