cmake_minimum_required(VERSION 4.2)
project(quiche_py LANGUAGES CXX)

if(DEFINED SKBUILD_PROJECT_DIR)
  set(PACKAGE_ROOT "${SKBUILD_PROJECT_DIR}")
else()
  set(PACKAGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

get_filename_component(PACKAGE_ROOT_ABS "${PACKAGE_ROOT}" ABSOLUTE)

# ----------------------------------------------------------------------------
# QUICHE: 既定では deps.json から clone する。QUICHE_ROOT で既存 checkout も指定可
# ----------------------------------------------------------------------------
set(QUICHE_ROOT "" CACHE PATH
  "Path to a local google/quiche (directory containing MODULE.bazel). Unset/empty: clone (see deps.json; network on first configure).")

set(_quiche_ready FALSE)
if(QUICHE_ROOT)
  get_filename_component(QUICHE_ROOT "${QUICHE_ROOT}" ABSOLUTE CACHE PATH
    "Path to a local google/quiche (directory containing MODULE.bazel). Unset/empty: clone (see deps.json; network on first configure)."
    FORCE)
  if(EXISTS "${QUICHE_ROOT}/MODULE.bazel")
    set(_quiche_ready TRUE)
  else()
    message(FATAL_ERROR
      "QUICHE_ROOT is set but is not a QUICHE checkout (expected MODULE.bazel): ${QUICHE_ROOT}")
  endif()
endif()

if(NOT _quiche_ready)
  if(NOT EXISTS "${PACKAGE_ROOT_ABS}/deps.json")
    message(FATAL_ERROR
      "Need ${PACKAGE_ROOT_ABS}/deps.json to fetch QUICHE, or set QUICHE_ROOT to a local checkout.")
  endif()
  file(READ "${PACKAGE_ROOT_ABS}/deps.json" _deps_json)
  string(JSON _quiche_repo GET "${_deps_json}" "quiche" "repository")
  string(JSON _quiche_ref GET "${_deps_json}" "quiche" "ref")
  # コミットハッシュ指定時は shallow では取れないサーバが多いためフル取得する。
  # CMake の正規表現は {40} などのインターバル量指定子をサポートしないため、
  # 文字列長と [0-9a-fA-F]+ の両方で 40 桁ハッシュを判定する。
  string(LENGTH "${_quiche_ref}" _quiche_ref_len)
  if(_quiche_ref_len EQUAL 40 AND _quiche_ref MATCHES "^[0-9a-fA-F]+$")
    set(_quiche_shallow FALSE)
  else()
    set(_quiche_shallow TRUE)
  endif()
  include(FetchContent)
  message(STATUS "Fetching QUICHE: ${_quiche_repo} @ ${_quiche_ref} (offline: set -DQUICHE_ROOT=... to a local tree)")
  if(_quiche_shallow)
    FetchContent_Declare(
      quiche
      GIT_REPOSITORY "${_quiche_repo}"
      GIT_TAG "${_quiche_ref}"
      GIT_SHALLOW TRUE
    )
  else()
    FetchContent_Declare(
      quiche
      GIT_REPOSITORY "${_quiche_repo}"
      GIT_TAG "${_quiche_ref}"
    )
  endif()
  FetchContent_MakeAvailable(quiche)
  get_filename_component(QUICHE_ROOT "${quiche_SOURCE_DIR}" ABSOLUTE)
  set(QUICHE_ROOT "${QUICHE_ROOT}" CACHE PATH
    "Path to a local google/quiche (directory containing MODULE.bazel). Unset/empty: clone (see deps.json; network on first configure)."
    FORCE)
endif()

# 本パッケージ (BUILD.bazel + src) を QUICHE ワークツリーへ overlay する。
# MODULE.bazel / quiche/BUILD.bazel は改変しない (Bazel の select は
# @bazel_tools の組込み conditions を使い、io_tool_support にも依存しない)。
# QUICHE_ROOT 指定時も ${QUICHE_ROOT}/quiche-py/ 配下だけを書き込む。
get_filename_component(_qp_in_quiche "${QUICHE_ROOT}/quiche-py" ABSOLUTE)
if(NOT "${PACKAGE_ROOT_ABS}" STREQUAL "${_qp_in_quiche}")
  file(MAKE_DIRECTORY "${_qp_in_quiche}")
  file(COPY "${PACKAGE_ROOT_ABS}/BUILD.bazel" DESTINATION "${_qp_in_quiche}")
  file(COPY "${PACKAGE_ROOT_ABS}/src" DESTINATION "${_qp_in_quiche}")
  message(STATUS "quiche-py Bazel overlay: ${_qp_in_quiche} (from this package)")
else()
  message(STATUS "quiche-py Bazel overlay: in-tree at ${_qp_in_quiche}")
endif()

message(STATUS "quiche-py package root: ${PACKAGE_ROOT_ABS}")
message(STATUS "QUICHE_ROOT (Bazel CWD): ${QUICHE_ROOT}")

if(WIN32)
  message(FATAL_ERROR "quiche-py: Windows is not supported yet (Bazel dylib wiring).")
endif()

# QUICHE は .bazelversion を使う。bazel は Bazelisk (またはそのラッパ) であること。
if(NOT BAZEL_EXECUTABLE)
  find_program(BAZEL_EXECUTABLE
    NAMES bazelisk bazel
    DOC "Bazelisk (recommended) or `bazel` symlinked to Bazelisk")
endif()

if(NOT BAZEL_EXECUTABLE)
  message(FATAL_ERROR
    "No Bazel launcher found. Install Bazelisk: https://github.com/bazelbuild/bazelisk "
    "(e.g. brew install bazelisk). Or pass -DBAZEL_EXECUTABLE=/path/to/bazelisk to CMake.")
endif()

get_filename_component(_bazel_name "${BAZEL_EXECUTABLE}" NAME_WE)
if(NOT _bazel_name STREQUAL "bazelisk")
  execute_process(
    COMMAND "${BAZEL_EXECUTABLE}" version
    OUTPUT_VARIABLE _bazel_version_out
    ERROR_VARIABLE _bazel_version_err
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  set(_bazel_version_combined "${_bazel_version_out}\n${_bazel_version_err}")
  if(NOT _bazel_version_combined MATCHES "Bazelisk")
    message(FATAL_ERROR
      "'${BAZEL_EXECUTABLE}' is not Bazelisk; QUICHE needs the Bazel version from .bazelversion.\n"
      "Install: brew install bazelisk\n"
      "Or: -DBAZEL_EXECUTABLE=/path/to/bazelisk")
  endif()
endif()

set(QUICHE_BAZEL_TARGET "//quiche-py:quiche_for_python" CACHE STRING "Bazel target for the QUICHE shared library")
set(QUICHE_BAZEL_COMPILATION_MODE "opt" CACHE STRING "Bazel compilation mode (-c opt|dbg|fastbuild)")

message(STATUS "Running: ${BAZEL_EXECUTABLE} build -c ${QUICHE_BAZEL_COMPILATION_MODE} --features=-layering_check --host_features=-layering_check ${QUICHE_BAZEL_TARGET}")
execute_process(
  COMMAND ${BAZEL_EXECUTABLE} build -c ${QUICHE_BAZEL_COMPILATION_MODE} --features=-layering_check --host_features=-layering_check ${QUICHE_BAZEL_TARGET}
  WORKING_DIRECTORY ${QUICHE_ROOT}
  RESULT_VARIABLE _quiche_bazel_rc
  OUTPUT_VARIABLE _quiche_bazel_out
  ERROR_VARIABLE _quiche_bazel_err
)
if(NOT _quiche_bazel_rc EQUAL 0)
  message(FATAL_ERROR "Bazel build failed (rc=${_quiche_bazel_rc}):\n${_quiche_bazel_err}\n${_quiche_bazel_out}")
endif()

execute_process(
  COMMAND ${BAZEL_EXECUTABLE} info bazel-bin -c ${QUICHE_BAZEL_COMPILATION_MODE}
  WORKING_DIRECTORY ${QUICHE_ROOT}
  OUTPUT_VARIABLE BAZEL_BIN
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _bazel_info_rc
)
if(NOT _bazel_info_rc EQUAL 0)
  message(FATAL_ERROR "bazel info bazel-bin failed (rc=${_bazel_info_rc})")
endif()
string(REGEX REPLACE "\n.*" "" BAZEL_BIN "${BAZEL_BIN}")
get_filename_component(BAZEL_BIN_ABS "${BAZEL_BIN}" ABSOLUTE)

if(APPLE)
  set(QUICHE_SHLIB "${BAZEL_BIN_ABS}/quiche-py/libquiche_for_python.dylib")
else()
  set(QUICHE_SHLIB "${BAZEL_BIN_ABS}/quiche-py/libquiche_for_python.so")
endif()

if(NOT EXISTS "${QUICHE_SHLIB}")
  message(FATAL_ERROR "Expected QUICHE shared library missing after Bazel build:\n  ${QUICHE_SHLIB}")
endif()
message(STATUS "QUICHE shared library: ${QUICHE_SHLIB}")

get_filename_component(QUICHE_SHLIB_DIR "${QUICHE_SHLIB}" DIRECTORY)

find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
find_package(nanobind CONFIG REQUIRED)

set(PY_PACKAGE "quiche")

nanobind_add_module(quiche_ext
  FREE_THREADED
  src/bindings/module.cpp
)

target_include_directories(quiche_ext PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/bindings"
)

target_compile_features(quiche_ext PRIVATE cxx_std_20)

target_link_libraries(quiche_ext PRIVATE "${QUICHE_SHLIB}")

if(APPLE)
  set_target_properties(quiche_ext PROPERTIES
    INSTALL_RPATH "@loader_path"
    BUILD_RPATH "${QUICHE_SHLIB_DIR}"
  )
elseif(UNIX)
  set_target_properties(quiche_ext PROPERTIES
    INSTALL_RPATH "\$ORIGIN"
    BUILD_RPATH "${QUICHE_SHLIB_DIR}"
  )
endif()

target_compile_options(quiche_ext PRIVATE -fvisibility=hidden)

install(TARGETS quiche_ext
  LIBRARY DESTINATION ${PY_PACKAGE}
  RUNTIME DESTINATION ${PY_PACKAGE}
)

install(FILES "${QUICHE_SHLIB}" DESTINATION ${PY_PACKAGE})

nanobind_add_stub(
  quiche_ext_stub
  MODULE quiche_ext
  OUTPUT ${PY_PACKAGE}/quiche_ext.pyi
  PYTHON_PATH $<TARGET_FILE_DIR:quiche_ext>
  DEPENDS quiche_ext
  PATTERN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/bindings/stubgen_patterns.txt
  EXCLUDE_DOCSTRINGS
)

# stub を wheel に同梱する。editable 時は ty が src/ を見るためソースツリーへもコピーする。
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PY_PACKAGE}/quiche_ext.pyi"
  DESTINATION ${PY_PACKAGE}
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/quiche/py.typed"
  DESTINATION ${PY_PACKAGE}
)
add_custom_command(
  TARGET quiche_ext_stub
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_BINARY_DIR}/${PY_PACKAGE}/quiche_ext.pyi"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/quiche/quiche_ext.pyi"
  COMMENT "Copy quiche_ext.pyi into src/ for editable type checking"
)
