cmake_minimum_required(VERSION 3.21)
project(DragonAspectFlight VERSION 1.8.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)
find_package(Python3 COMPONENTS Interpreter QUIET)

option(DAF_BUILD_PLUGIN
    "Build the native Dragon Aspect Flight plugin (disable for dependency-free tests)"
    ON)

if(BUILD_TESTING)
	add_executable(DragonAspectFlightFlightStateTests
		tests/flight_state_tests.cpp)
	target_include_directories(DragonAspectFlightFlightStateTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/include")
	add_test(
		NAME DragonAspectFlight.FlightState
		COMMAND DragonAspectFlightFlightStateTests)
	if(Python3_Interpreter_FOUND)
		add_test(
			NAME DragonAspectFlight.AddressLibraryValidator
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/test_validate_address_library.py")
		add_test(
			NAME DragonAspectFlight.PEImportValidatorUnit
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/test_validate_pe_imports.py")
		add_test(
			NAME DragonAspectFlight.StructuredLoggingContract
			COMMAND "${Python3_EXECUTABLE}" -m pytest -q
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/test_structured_logging_contract.py")
		add_test(
			NAME DragonAspectFlight.AnimationStack
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tools/test_build_flight_animation_stack.py")
		add_test(
			NAME DragonAspectFlight.DiagnosisPlanTools
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/test_diagnosis_plan_tools.py")
		add_test(
			NAME DragonAspectFlight.RuntimeRepairContract
			COMMAND "${Python3_EXECUTABLE}"
				"${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_repair_contract.py")
	endif()
	add_test(
		NAME DragonAspectFlight.CommonLibCapabilityGate
		COMMAND "${CMAKE_COMMAND}"
			"-DDAF_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
			"-DDAF_TEST_ROOT=${CMAKE_BINARY_DIR}/commonlib-capability-gate"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_commonlib_capability_gate.cmake")
endif()

if(DAF_BUILD_PLUGIN)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DafCommonLibBaseline.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DafStaticDependencyGuard.cmake")
    daf_require_static_md_vcpkg_triplet()

    set(COMMONLIBSSE_NG_PATH ""
        CACHE PATH "Explicit path to CommonLibSSE-NG ${DAF_MINIMUM_COMMONLIB_VERSION} or newer")

if(COMMONLIBSSE_NG_PATH STREQUAL "")
    message(FATAL_ERROR
        "DAF_BUILD_PLUGIN=ON requires an explicit CommonLibSSE-NG ${DAF_MINIMUM_COMMONLIB_VERSION} or newer checkout. "
        "Configure with -DCOMMONLIBSSE_NG_PATH=<path>. "
        "For dependency-free reducer tests, configure with -DDAF_BUILD_PLUGIN=OFF.")
endif()

option(ENABLE_SKYRIM_SE "Build for Skyrim SE (1.5.97)" ON)
option(ENABLE_SKYRIM_AE "Build for Skyrim AE (1.6.x and 1.7.104)" ON)
option(ENABLE_SKYRIM_VR "Build for Skyrim VR" ON)

if(NOT EXISTS "${COMMONLIBSSE_NG_PATH}/CMakeLists.txt")
    message(FATAL_ERROR
        "CommonLibSSE-NG not found at ${COMMONLIBSSE_NG_PATH}. "
        "Provide a CommonLibSSE-NG ${DAF_MINIMUM_COMMONLIB_VERSION} or newer checkout via -DCOMMONLIBSSE_NG_PATH.")
endif()

# The 1.7.104 AE runtime uses the Address Library on-disk format 5. Require an
# official, versioned CommonLib checkout with the generic format-5 parser rather
# than accepting a stale library that only happens to contain a runtime name.
# Detect the required capabilities from the public IDDB API and its parser
# implementation instead of trusting a repository name or a machine-specific
# checkout path.
set(_daf_commonlib_version_header "${COMMONLIBSSE_NG_PATH}/include/SKSE/Version.h")
set(_daf_commonlib_iddb_header "${COMMONLIBSSE_NG_PATH}/include/REL/IDDB.h")
set(_daf_commonlib_iddb_source "${COMMONLIBSSE_NG_PATH}/src/REL/IDDB.cpp")
if(NOT EXISTS "${_daf_commonlib_version_header}" OR
   NOT EXISTS "${_daf_commonlib_iddb_header}" OR
   NOT EXISTS "${_daf_commonlib_iddb_source}")
	message(FATAL_ERROR
		"CommonLib is missing the capability sources SKSE/Version.h, REL/IDDB.h, or src/REL/IDDB.cpp. "
		"Use an official CommonLibSSE-NG checkout that supports Address Library format 5.")
endif()

file(READ "${_daf_commonlib_iddb_header}" _daf_commonlib_iddb_header_text)
file(READ "${_daf_commonlib_iddb_source}" _daf_commonlib_iddb_source_text)
if(NOT _daf_commonlib_iddb_header_text MATCHES "SSEv5")
	message(FATAL_ERROR
		"The selected CommonLib does not declare REL::IDDB::Format::SSEv5. "
		"It cannot load the 1.7.104 Address Library format-5 BIN.")
endif()

# Compile the actual public/nested types instead of accepting comments or
# unrelated identifiers that merely contain the capability names.  The v5
# parser members are private implementation details in CommonLib; including
# the module first and temporarily exposing access for this probe lets CMake
# verify their declarations without changing the consumer headers.
set(_daf_commonlib_probe_project
	"${CMAKE_BINARY_DIR}/CMakeFiles/daf_commonlib_capability_probe")
file(MAKE_DIRECTORY "${_daf_commonlib_probe_project}")
file(WRITE "${_daf_commonlib_probe_project}/CMakeLists.txt"
	"cmake_minimum_required(VERSION 3.21)\n"
	"project(DAFCommonLibCapabilityProbe LANGUAGES CXX)\n"
	"add_executable(DAFCommonLibCapabilityProbe probe.cpp)\n"
	"target_include_directories(DAFCommonLibCapabilityProbe PRIVATE \"${COMMONLIBSSE_NG_PATH}/include\")\n"
	"target_compile_definitions(DAFCommonLibCapabilityProbe PRIVATE ENABLE_SKYRIM_SE=1 ENABLE_SKYRIM_AE=1 ENABLE_SKYRIM_VR=1)\n")
file(WRITE "${_daf_commonlib_probe_project}/probe.cpp" [=[
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4455)
#endif

namespace SKSE::stl
{
    using zstring = std::string_view;
    using zwstring = std::wstring_view;

    inline void report_and_fail(std::string_view) {}
    inline bool report_and_error(std::string_view, bool) { return false; }
    inline std::optional<std::string> utf16_to_utf8(std::wstring_view) { return std::string{}; }
    inline std::optional<std::wstring> utf8_to_utf16(std::string_view) { return std::wstring{}; }
}
namespace stl = SKSE::stl;
using namespace std::literals;

#define SKYRIM_REL inline
#define SKYRIM_REL_CONSTEXPR
#define SKYRIM_REL_VR inline
#define SKYRIM_REL_VR_CONSTEXPR

#include "REL/Module.h"
#define private public
#include "REL/IDDB.h"
#undef private

#undef SKYRIM_REL
#undef SKYRIM_REL_CONSTEXPR
#undef SKYRIM_REL_VR
#undef SKYRIM_REL_VR_CONSTEXPR

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

using DafAddressLibraryHeader = REL::IDDB::header_v5_t;
using DafAddressLibraryParser = decltype(&REL::IDDB::load_v5);

static_assert(std::is_class_v<DafAddressLibraryHeader>);
static_assert(std::is_member_function_pointer_v<DafAddressLibraryParser>);
constexpr auto DafAddressLibraryFormat = REL::IDDB::Format::SSEv5;

int main()
{
    return DafAddressLibraryFormat == REL::IDDB::Format::SSEv5 ? 0 : 1;
}
]=])
try_compile(
	_daf_commonlib_probe_compiles
	"${_daf_commonlib_probe_project}/build"
	"${_daf_commonlib_probe_project}"
	DAFCommonLibCapabilityProbe
	CMAKE_FLAGS
		"-DCMAKE_CXX_STANDARD=23"
		"-DCMAKE_CXX_STANDARD_REQUIRED=ON"
	OUTPUT_VARIABLE _daf_commonlib_probe_output)
if(NOT _daf_commonlib_probe_compiles)
	message(FATAL_ERROR
		"The selected CommonLib's generic REL::IDDB format-5 capability probe failed to compile.\n"
		"${_daf_commonlib_probe_output}")
endif()

if(NOT _daf_commonlib_iddb_header_text MATCHES "header_v5_t" OR
   NOT _daf_commonlib_iddb_header_text MATCHES "load_v5" OR
   NOT _daf_commonlib_iddb_source_text MATCHES "Format::SSEv5" OR
   NOT _daf_commonlib_iddb_source_text MATCHES "load_v5")
	message(FATAL_ERROR
		"The selected CommonLib lacks the generic REL::IDDB format-5 parser "
		"(header_v5_t/load_v5 dispatch).")
endif()

# CommonLib's project version is the portable source-level baseline. It is
# checked before add_subdirectory so a checkout that only happens to contain
# similarly named capability tokens cannot silently become the build input.
set(_daf_commonlib_cmake "${COMMONLIBSSE_NG_PATH}/CMakeLists.txt")
file(READ "${_daf_commonlib_cmake}" _daf_commonlib_cmake_text)
string(REGEX MATCH
	"project[ \\t\\r\\n]*\\([^)]*VERSION[ \\t\\r\\n]+([0-9]+\\.[0-9]+\\.[0-9]+)"
	_daf_commonlib_project_match "${_daf_commonlib_cmake_text}")
if(NOT _daf_commonlib_project_match)
	message(FATAL_ERROR
		"Could not determine the CommonLibSSE-NG project version from ${_daf_commonlib_cmake}. "
		"Use CommonLibSSE-NG ${DAF_MINIMUM_COMMONLIB_VERSION} or newer with a versioned CMake project.")
endif()
set(_daf_commonlib_project_version "${CMAKE_MATCH_1}")
if(_daf_commonlib_project_version VERSION_LESS DAF_MINIMUM_COMMONLIB_VERSION)
	message(FATAL_ERROR
		"CommonLibSSE-NG ${_daf_commonlib_project_version} is too old for DAF; "
		"minimum supported project version is ${DAF_MINIMUM_COMMONLIB_VERSION} "
		"(known-good baseline ${DAF_KNOWN_GOOD_COMMONLIB_COMMIT}).")
endif()
message(STATUS
	"DAF CommonLibSSE-NG baseline: project ${_daf_commonlib_project_version} "
	"(minimum ${DAF_MINIMUM_COMMONLIB_VERSION}; known-good ${DAF_KNOWN_GOOD_COMMONLIB_COMMIT})")

# Resolve the two runtime-sensitive packages at top-level scope so their
# imported target contracts can be inspected before CommonLib consumes them.
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
daf_require_static_dependency_target(spdlog::spdlog "spdlog")
daf_require_static_dependency_target(fmt::fmt "fmt")
daf_require_static_md_package_path(spdlog_DIR)
daf_require_static_md_package_path(fmt_DIR)

add_subdirectory("${COMMONLIBSSE_NG_PATH}" "${CMAKE_BINARY_DIR}/CommonLibSSE-NG"
    EXCLUDE_FROM_ALL)

daf_require_static_md_package_path(directxtk_DIR)
daf_require_static_md_package_path(directxmath_DIR)
daf_require_static_md_package_path(RAPIDCSV_INCLUDE_DIRS)

set(SOURCES
    src/main.cpp
    src/FlightManager.cpp
    src/InputHandler.cpp
    src/Papyrus.cpp
    src/Settings.cpp
    src/UI.cpp
    src/DragonAspectMonitor.cpp
    imgui/imgui.cpp
    imgui/imgui_demo.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/misc/cpp/imgui_stdlib.cpp
    imgui/backends/imgui_impl_dx11.cpp
    imgui/backends/imgui_impl_win32.cpp
)

set(HEADERS
    src/PCH.h
	include/DragonAspectFlight/FlightManager.h
	include/DragonAspectFlight/FlightStateHelpers.h
	include/DragonAspectFlight/Compatibility.h
    include/DragonAspectFlight/InputHandler.h
    include/DragonAspectFlight/Papyrus.h
    include/DragonAspectFlight/Settings.h
    include/DragonAspectFlight/UI.h
    include/DragonAspectFlight/DragonAspectMonitor.h
    include/DragonAspectFlight/Version.h
    include/SKSEMenuFramework.h
)

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui/misc/cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends"
    "${COMMONLIBSSE_NG_PATH}/include"
    "${COMMONLIBSSE_NG_PATH}/src"
)

target_link_libraries(${PROJECT_NAME} PUBLIC CommonLibSSE)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    DAF_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    DAF_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    DAF_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    DAF_VERSION_STRING="${PROJECT_VERSION}"
    DAF_BUILD_LABEL="v${PROJECT_VERSION}-se-ae-vr-flight-stack"
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_precompile_headers(${PROJECT_NAME} PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src/PCH.h>"
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /utf-8 /Zc:preprocessor /Zc:__cplusplus /permissive-
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS IMGUI_DEFINE_MATH_OPERATORS
    )
    set_property(TARGET ${PROJECT_NAME} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../SKSE"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/SKSE/Plugins"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PROJECT_NAME}>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/SKSE/Plugins/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight - Bundled Flight Animations"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight - More Draconic External Selectors"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/animations/OpenAnimationReplacer/More Dragonic Dragon Aspect Can Fly"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight - Vampire Lord Fallback"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight - Flight Combat"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/animations/NickNak"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/Effects/NickNak"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/animations/OpenAnimationReplacer/More Dragonic Dragon Aspect Can Fly/Flying Mod"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/animations/OpenAnimationReplacer/More Dragonic Dragon Aspect Can Fly/Elegant Flying Animations"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/behaviors"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/OptionalBehaviorGeneratorSupport"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Pandora_Engine"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Data"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data"
    COMMENT "Staging a Data-shaped Dragon Aspect Flight mod root")

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION "Data/SKSE/Plugins"
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Data/"
    DESTINATION "Data"
)

if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(
        NAME DragonAspectFlight.PEImports
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_pe_imports.py"
            --dll "$<TARGET_FILE:${PROJECT_NAME}>")
endif()
endif()
