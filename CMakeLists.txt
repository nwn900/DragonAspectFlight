cmake_minimum_required(VERSION 3.21)
project(DragonAspectFlight VERSION 1.8.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMMONLIBSSE_NG_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../CommonLibVR-NG"
    CACHE PATH "Path to the maintained CommonLibVR ng root")

option(ENABLE_SKYRIM_SE "Build for Skyrim SE (1.5.97)" ON)
option(ENABLE_SKYRIM_AE "Build for Skyrim AE (1.6.x)" ON)
option(ENABLE_SKYRIM_VR "Build for Skyrim VR" ON)

if(NOT EXISTS "${COMMONLIBSSE_NG_PATH}/CMakeLists.txt")
    message(FATAL_ERROR
        "CommonLibVR ng not found at ${COMMONLIBSSE_NG_PATH}. "
        "Clone the ng branch from https://github.com/alandtse/CommonLibVR")
endif()

add_subdirectory("${COMMONLIBSSE_NG_PATH}" "${CMAKE_BINARY_DIR}/CommonLibSSE-NG"
    EXCLUDE_FROM_ALL)

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
