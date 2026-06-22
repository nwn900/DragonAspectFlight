cmake_minimum_required(VERSION 3.21)
project(DragonAspectFlight VERSION 0.9.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMMONLIBSSE_NG_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../CommonLibSSE-NG"
    CACHE PATH "Path to CommonLibSSE-NG root")

option(ENABLE_SKYRIM_SE "Build for Skyrim SE (1.5.97)" ON)
option(ENABLE_SKYRIM_AE "Build for Skyrim AE (1.6.x)" OFF)
option(ENABLE_SKYRIM_VR "Build for Skyrim VR" OFF)
option(DAF_MATERIALIZE_EXTERNAL_ANIMATION_LINKS
    "Materialize external More Draconic animation hardlinks into the staged build output"
    OFF)

set(DAF_MORE_DRACONIC_OAR_ROOT
    "C:/Games/Nolvus/Instances/Nolvus Awakening/MODS/mods/[NoDelete] [001.00202] More Draconic Aspect Can Fly (With Collisions)/meshes/actors/character/animations/OpenAnimationReplacer/More Dragonic Dragon Aspect Can Fly"
    CACHE PATH "Path to the installed More Dragonic Dragon Aspect Can Fly OAR root")

if(DAF_MATERIALIZE_EXTERNAL_ANIMATION_LINKS)
    find_program(POWERSHELL_EXECUTABLE powershell)
    if(NOT POWERSHELL_EXECUTABLE)
        find_program(POWERSHELL_EXECUTABLE pwsh)
    endif()

    if(NOT POWERSHELL_EXECUTABLE)
        message(FATAL_ERROR "PowerShell is required to materialize external More Draconic animation hardlinks")
    endif()
endif()

if(NOT EXISTS "${COMMONLIBSSE_NG_PATH}/CMakeLists.txt")
    message(FATAL_ERROR
        "CommonLibSSE-NG not found at ${COMMONLIBSSE_NG_PATH}. "
        "Clone it from https://github.com/CharmedBaryon/CommonLibSSE-NG")
endif()

add_subdirectory("${COMMONLIBSSE_NG_PATH}" "${CMAKE_BINARY_DIR}/CommonLibSSE-NG"
    EXCLUDE_FROM_ALL)

set(SOURCES
    src/main.cpp
    src/FlightManager.cpp
    src/InputHandler.cpp
    src/Papyrus.cpp
)

set(HEADERS
    src/PCH.h
    include/DragonAspectFlight/FlightManager.h
    include/DragonAspectFlight/InputHandler.h
    include/DragonAspectFlight/Papyrus.h
)

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${COMMONLIBSSE_NG_PATH}/include"
    "${COMMONLIBSSE_NG_PATH}/src"
)

target_link_libraries(${PROJECT_NAME} PUBLIC CommonLibSSE)

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
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    )
    set_property(TARGET ${PROJECT_NAME} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../SKSE/Plugins"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PROJECT_NAME}>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../SKSE/Plugins/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight - Bundled Flight Animations"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight - More Draconic External Selectors"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/animations/OpenAnimationReplacer/More Dragonic Dragon Aspect Can Fly"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/animations/OpenAnimationReplacer/Dragon Aspect Flight - Vampire Lord Fallback"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/meshes/actors/character/behaviors"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data/OptionalBehaviorGeneratorSupport"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Pandora_Engine"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Data"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data"
    COMMENT "Copying DragonAspectFlight.dll to SKSE/Plugins")

if(DAF_MATERIALIZE_EXTERNAL_ANIMATION_LINKS)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/MaterializeExternalAnimationLinks.ps1"
            -ManifestPath "${CMAKE_CURRENT_SOURCE_DIR}/tools/dragon-aspect-flight-animation-links.json"
            -TargetDataRoot "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../Data"
            -ExternalOarRoot "${DAF_MORE_DRACONIC_OAR_ROOT}"
        COMMENT "Materializing optional external More Draconic animation hardlinks")
endif()

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION "Data/SKSE/Plugins"
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Data/"
    DESTINATION "Data"
)
