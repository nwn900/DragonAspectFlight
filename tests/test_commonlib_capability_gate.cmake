cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED DAF_SOURCE_DIR OR DAF_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "DAF_SOURCE_DIR is required")
endif()

if(NOT DEFINED DAF_TEST_ROOT OR DAF_TEST_ROOT STREQUAL "")
    set(DAF_TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/daf-commonlib-capability-gate")
endif()

# This fixture intentionally has no RUNTIME_SSE_1_7_104 symbol.  It models the
# official source-level contract needed by the consumer: a supported project
# version, the public SSEv5 enum, and the generic format-5 parser symbols.
set(_fixture "${DAF_TEST_ROOT}/commonlib")
set(_prefix "${DAF_TEST_ROOT}/packages")
set(_build "${DAF_TEST_ROOT}/build")
file(REMOVE_RECURSE "${DAF_TEST_ROOT}")
file(MAKE_DIRECTORY
    "${_fixture}/include/SKSE"
    "${_fixture}/include/REL"
    "${_fixture}/src/REL"
    "${_prefix}/share/spdlog"
    "${_prefix}/share/fmt")

file(WRITE "${_fixture}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.21)
project(CommonLibSSE_NG VERSION 6.5.0 LANGUAGES CXX)
add_library(CommonLibSSE INTERFACE)
]=])
file(WRITE "${_fixture}/include/SKSE/Version.h" [=[
#pragma once
namespace SKSE {}
]=])
file(WRITE "${_fixture}/include/REL/Module.h" [=[
#pragma once
namespace REL {}
]=])
file(WRITE "${_fixture}/include/REL/IDDB.h" [=[
#pragma once
namespace REL {
class IDDB {
public:
    enum class Format { SSEv5 };
    class header_v5_t {};
    bool load_v5();
};
}
]=])
file(WRITE "${_fixture}/src/REL/IDDB.cpp" [=[
#include "REL/IDDB.h"
namespace REL {
bool IDDB::load_v5() {
    auto format = IDDB::Format::SSEv5;
    (void)format;
    return true;
}
}
]=])
file(WRITE "${_prefix}/share/spdlog/spdlogConfig.cmake"
    "add_library(spdlog::spdlog INTERFACE IMPORTED)\n")
file(WRITE "${_prefix}/share/fmt/fmtConfig.cmake"
    "add_library(fmt::fmt INTERFACE IMPORTED)\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${DAF_SOURCE_DIR}"
        -B "${_build}"
        -DDAF_BUILD_PLUGIN=ON
        -DBUILD_TESTING=OFF
        -DCOMMONLIBSSE_NG_PATH=${_fixture}
        -DCMAKE_PREFIX_PATH=${_prefix}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)

if(NOT _result EQUAL 0)
    message(FATAL_ERROR
        "A supported CommonLib with generic SSEv5 parsing failed configure:\n"
        "${_stdout}\n${_stderr}")
endif()
if(_stdout MATCHES "RUNTIME_SSE_1_7_104")
    message(FATAL_ERROR
        "The capability regression fixture must not require the absent 1.7.104 symbol:\n${_stdout}")
endif()

# A comments-only source must not satisfy the capability probe.  This guards
# against returning to text-token matching for the parser declarations.
set(_negative_fixture "${DAF_TEST_ROOT}/comments-only")
set(_negative_build "${DAF_TEST_ROOT}/comments-only-build")
file(MAKE_DIRECTORY
    "${_negative_fixture}/include/SKSE"
    "${_negative_fixture}/include/REL"
    "${_negative_fixture}/src/REL")
file(WRITE "${_negative_fixture}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.21)
project(CommonLibSSE_NG VERSION 6.5.0 LANGUAGES CXX)
add_library(CommonLibSSE INTERFACE)
]=])
file(WRITE "${_negative_fixture}/include/SKSE/Version.h" [=[
#pragma once
namespace SKSE {}
]=])
file(WRITE "${_negative_fixture}/include/REL/Module.h" [=[
#pragma once
namespace REL {}
]=])
file(WRITE "${_negative_fixture}/include/REL/IDDB.h" [=[
#pragma once
namespace REL {
class IDDB {
public:
    // enum class Format { SSEv5 };
    // class header_v5_t {};
    // bool load_v5();
};
}
]=])
file(WRITE "${_negative_fixture}/src/REL/IDDB.cpp" [=[
// Format::SSEv5 and load_v5 are comments, not parser capability.
]=])

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${DAF_SOURCE_DIR}"
        -B "${_negative_build}"
        -DDAF_BUILD_PLUGIN=ON
        -DBUILD_TESTING=OFF
        -DBUILD_TESTS=OFF
        -DCOMMONLIBSSE_NG_PATH=${_negative_fixture}
        -DCMAKE_PREFIX_PATH=${_prefix}
    RESULT_VARIABLE _negative_result
    OUTPUT_VARIABLE _negative_stdout
    ERROR_VARIABLE _negative_stderr)

if(_negative_result EQUAL 0)
    message(FATAL_ERROR "A comments-only CommonLib fixture unexpectedly passed the capability gate")
endif()
if(NOT _negative_stdout MATCHES "generic REL::IDDB format-5 capability probe failed" AND
   NOT _negative_stderr MATCHES "generic REL::IDDB format-5 capability probe failed")
    message(FATAL_ERROR
        "The comments-only fixture failed for an unexpected reason:\n"
        "${_negative_stdout}\n${_negative_stderr}")
endif()
