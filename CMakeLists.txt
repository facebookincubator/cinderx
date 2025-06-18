# (c) Meta Platforms, Inc. and affiliates.

message("=====================================================================")
message("=                            WARNING                                =")
message("=====================================================================")
message("=                                                                   =")
message("= This CMake build is not well supported, and is primarily used for =")
message("= experimentation.  Do not expect this to work on your machine.     =")
message("=====================================================================")
message("")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 5)

cmake_minimum_required(VERSION 3.12)
project(_cinderx)

if (NOT ("${CMAKE_C_COMPILER_ID}" MATCHES "Clang" AND "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang"))
  message(FATAL_ERROR "This build requires using clang for C and C++ compilation, please switch to that")
endif()

set(LINUX (${CMAKE_SYSTEM_NAME} MATCHES "Linux"))
set(MACOS (${CMAKE_SYSTEM_NAME} MATCHES "Darwin"))

if (${MACOS})
  message("=====================================================================")
  message("= You appear to be building on macOS. This is known to not work and =")
  message("= is not supported, but we'll allow the build to move forward for   =")
  message("= experimentation purposes.                                         =")
  message("=====================================================================")
  message("")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 5)
endif()

##############################################################################
# Figure out capabilities / supported features and set compiler flags
# appropriately.

if (NOT DEFINED PY_VERSION)
  set(PY_VERSION 3.12)
endif()
if (NOT DEFINED META_PYTHON)
  set(META_PYTHON 1)
endif()

if (NOT DEFINED ENABLE_FUNC_EVENT_MODIFY_QUALNAME)
  set(ENABLE_FUNC_EVENT_MODIFY_QUALNAME ${META_PYTHON} AND ${PY_VERSION} EQUAL 3.12)
endif()
if (NOT DEFINED ENABLE_LAZY_IMPORTS)
  set(ENABLE_LAZY_IMPORTS ${META_PYTHON} AND ${PY_VERSION} EQUAL 3.12)
endif()
if (NOT DEFINED ENABLE_PARALLEL_GC)
  set(ENABLE_PARALLEL_GC ${META_PYTHON} AND ${PY_VERSION} EQUAL 3.12)
endif()
if (NOT DEFINED ENABLE_SYMBOLIZER)
  set(ENABLE_SYMBOLIZER ${LINUX})
endif()
if (NOT DEFINED ENABLE_USDT)
  set(ENABLE_USDT ${LINUX})
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SHARED_FLAGS "-DPy_BUILD_CORE -DPy_BUILD_CORE_MODULE -fPIC -Wall -Wextra -Wno-c99-designator -Wno-c++11-narrowing -Wno-cast-function-type-mismatch -Wno-deprecated-declarations -Wno-missing-field-initializers -Wno-null-pointer-subtraction -Wno-sign-compare -Wno-unused-function -Wno-unused-parameter")

if (${ENABLE_FUNC_EVENT_MODIFY_QUALNAME})
  set(SHARED_FLAGS "-DENABLE_FUNC_EVENT_MODIFY_QUALNAME ${SHARED_FLAGS}")
endif()
if (${ENABLE_LAZY_IMPORTS})
  set(SHARED_FLAGS "-DENABLE_LAZY_IMPORTS ${SHARED_FLAGS}")
endif()
if (${ENABLE_PARALLEL_GC})
  set(SHARED_FLAGS "-DENABLE_PARALLEL_GC ${SHARED_FLAGS}")
endif()
if (${ENABLE_SYMBOLIZER})
  set(SHARED_FLAGS "-DENABLE_SYMBOLIZER ${SHARED_FLAGS}")
endif()
if (${ENABLE_USDT})
  set(SHARED_FLAGS "-DENABLE_USDT ${SHARED_FLAGS}")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SHARED_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SHARED_FLAGS}")

set(GENERATED_HEADER_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_HEADER_DIR})

##############################################################################
# Dependencies

include(FetchContent)

########################################
# python

if (${META_PYTHON})
  FetchContent_Declare(
    python
    GIT_REPOSITORY https://github.com/facebookincubator/cinder
    GIT_TAG meta/3.12
  )
  FetchContent_MakeAvailable(python)

  # Generate pyconfig.h by running the configure script.  Avoid doing so on each
  # run as it's slow.
  if (NOT EXISTS "${Python_SOURCE_DIR}/pyconfig.h")
    execute_process(COMMAND "./configure" WORKING_DIRECTORY "${Python_SOURCE_DIR}")
  endif()

  # pyconfig.h gets saved to the top-level source directory, not to Include/.
  set(Python_INCLUDE_DIRS "${python_SOURCE_DIR}/Include" "${python_SOURCE_DIR}")

  set(PYTHON_INTERNAL_DIR "${python_SOURCE_DIR}/Include/internal")
else()
  find_package(Python ${PY_VERSION} EXACT REQUIRED COMPONENTS Development)

  # "python" vs "Python" naming issues between Meta Python and CPython, this was
  # the best workaround at the time.
  set(PYTHON_INTERNAL_DIR "${Python_SOURCE_DIR}/Include/internal")
endif()

# Some of our files are partially generated from CPython source, and they
# expect to be able to include files relative to Include/internal.
set(Python_INCLUDE_DIRS ${Python_INCLUDE_DIRS} "${PYTHON_INTERNAL_DIR}")

########################################
# asmjit

set(ASMJIT_STATIC TRUE)

FetchContent_Declare(
  asmjit
  GIT_REPOSITORY https://github.com/asmjit/asmjit
  GIT_TAG cecc73f2979e9704c81a2c2ec79a7475b31c56ac # 2025-May-10
)
FetchContent_MakeAvailable(asmjit)

########################################
# fmt

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt
  GIT_TAG 11.2.0 # 2025-May-03
)
FetchContent_MakeAvailable(fmt)

########################################
# nlohmann-json

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json
  GIT_TAG v3.12.0 # 2025-Apr-11
)
FetchContent_MakeAvailable(nlohmann_json)

########################################
# parallel-hashmap

FetchContent_Declare(
  parallel-hashmap
  GIT_REPOSITORY https://github.com/greg7mdp/parallel-hashmap
  GIT_TAG 896f1a03e429c45d9fe9638e892fc1da73befadd # 2025-Apr-11
)
FetchContent_MakeAvailable(parallel-hashmap)

########################################
# usdt

FetchContent_Declare(
  usdt
  GIT_REPOSITORY https://github.com/libbpf/usdt
  GIT_TAG f4ea2f524efa80d062f4d586d78daafb83dc7d24 # 2025-Apr-24
)
FetchContent_MakeAvailable(usdt)

# Get directory format for `#include <usdt/usdt.h>` to work.
set(USDT_DIR ${GENERATED_HEADER_DIR}/usdt)
file(MAKE_DIRECTORY ${USDT_DIR})
file(COPY ${usdt_SOURCE_DIR}/usdt.h DESTINATION ${USDT_DIR})

########################################
# zlib

find_package(ZLIB)

##############################################################################
# Build cinderx

include_directories(
  ${PROJECT_SOURCE_DIR}/..
  ${GENERATED_HEADER_DIR}
  ${Python_INCLUDE_DIRS}
  ${parallel-hashmap_SOURCE_DIR}
  ${usdt_SOURCE_DIR}/..
)

########################################
# Common/

file(GLOB_RECURSE COMMON_SOURCES ${PROJECT_SOURCE_DIR}/Common/*.cpp)
add_library(common ${COMMON_SOURCES})
target_link_libraries(common PRIVATE fmt::fmt ZLIB::ZLIB)

########################################
# UpstreamBorrow/

if (${PY_VERSION} EQUAL 3.12)
  set(BORROWED_C ${PROJECT_SOURCE_DIR}/UpstreamBorrow/borrowed-3.12.gen_cached.c)
endif()

add_library(borrowed ${BORROWED_C})

########################################
# CachedProperties/

file(GLOB_RECURSE CACHED_PROPERTY_SOURCES ${PROJECT_SOURCE_DIR}/CachedProperties/*.c)
add_library(cached-properties ${CACHED_PROPERTY_SOURCES})
target_link_libraries(cached-properties PRIVATE common borrowed)

########################################
# Interpreter/

set(INTERP_SOURCES ${PROJECT_SOURCE_DIR}/Interpreter/iter_helpers.c)

if (${PY_VERSION} EQUAL 3.12)
  set(SOURCE_CINDERX_OPCODE_TARGETS_H ${PROJECT_SOURCE_DIR}/Interpreter/cinderx_opcode_targets_312.h)
  set(SOURCE_CINDER_OPCODE_H ${PROJECT_SOURCE_DIR}/Interpreter/cinder_opcode_312.h)
  set(SOURCE_OPCODE_H ${PROJECT_SOURCE_DIR}/Interpreter/opcode_312.h)

  if (${META_PYTHON})
    set(INTERP_SOURCES ${INTERP_SOURCES} ${PROJECT_SOURCE_DIR}/Interpreter/interpreter-3.12.c)
  endif()
endif()

# Generate cinderx/Interpreter header files.

set(CINDERX_OPCODE_TARGETS_H ${GENERATED_HEADER_DIR}/cinderx/Interpreter/cinderx_opcode_targets.h)
set(CINDER_OPCODE_H ${GENERATED_HEADER_DIR}/cinderx/Interpreter/cinder_opcode.h)
set(OPCODE_H ${GENERATED_HEADER_DIR}/cinderx/Interpreter/opcode.h)

file(READ ${SOURCE_CINDERX_OPCODE_TARGETS_H} FILE_CONTENTS)
file(WRITE ${CINDERX_OPCODE_TARGETS_H} "${FILE_CONTENTS}")

file(READ ${SOURCE_CINDER_OPCODE_H} FILE_CONTENTS)
file(WRITE ${CINDER_OPCODE_H} "${FILE_CONTENTS}")

file(READ ${SOURCE_OPCODE_H} FILE_CONTENTS)
file(WRITE ${OPCODE_H} "${FILE_CONTENTS}")

add_library(interpreter ${INTERP_SOURCES})
target_include_directories(
  interpreter
  PUBLIC
  ${PROJECT_SOURCE_DIR}/Interpreter/Includes
)

########################################
# Immortalize/

file(GLOB_RECURSE IMMORTALIZE_SOURCES ${PROJECT_SOURCE_DIR}/Immortalize/*.cpp)
add_library(immortalize ${IMMORTALIZE_SOURCES})
target_link_libraries(immortalize PRIVATE fmt::fmt)

########################################
# Upgrade/

file(GLOB_RECURSE UPGRADE_SOURCES ${PROJECT_SOURCE_DIR}/Upgrade/*.cpp)
add_library(upgrade ${UPGRADE_SOURCES})
target_link_libraries(upgrade PRIVATE fmt::fmt)

########################################
# StaticPython/

file(GLOB_RECURSE STATIC_PYTHON_SOURCES ${PROJECT_SOURCE_DIR}/StaticPython/*.c)
add_library(static-python ${STATIC_PYTHON_SOURCES})
target_link_libraries(static-python PRIVATE fmt::fmt common borrowed)

########################################
# Jit/

file(GLOB_RECURSE JIT_SOURCES ${PROJECT_SOURCE_DIR}/Jit/*.cpp ${PROJECT_SOURCE_DIR}/Jit/*.c)

# Drop Jit/elf for macOS builds.
if (${MACOS})
  list(FILTER JIT_SOURCES EXCLUDE REGEX ".*/elf/.*")
endif()

add_library(jit ${JIT_SOURCES})
target_link_libraries(jit PRIVATE asmjit::asmjit interpreter fmt::fmt nlohmann_json::nlohmann_json)

########################################
# ParallelGC/

if (${ENABLE_PARALLEL_GC})
  file(GLOB_RECURSE PARALLEL_GC_SOURCES ${PROJECT_SOURCE_DIR}/ParallelGC/*.c)
else()
  set(PARALLEL_GC_SOURCES "${GENERATED_HEADER_DIR}/dummy.c")
  file(WRITE ${PARALLEL_GC_SOURCES} "")
endif()
add_library(parallel-gc ${PARALLEL_GC_SOURCES})

########################################
# _cinderx.cpp

set(SOURCES
  # Manually listed out to not accidentally include stuff in .git.
  ${PROJECT_SOURCE_DIR}/_cinderx.cpp
  ${PROJECT_SOURCE_DIR}/_cinderx-lib.cpp
  ${PROJECT_SOURCE_DIR}/async_lazy_value.cpp
  ${PROJECT_SOURCE_DIR}/module_state.cpp
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE
  borrowed cached-properties common immortalize interpreter jit parallel-gc static-python
  asmjit::asmjit fmt::fmt)

# macOS doesn't allow depending on other shared libraries by default.
if (${MACOS})
  target_link_options(${PROJECT_NAME} PRIVATE -undefined dynamic_lookup)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX "")
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "_cinderx.so")
