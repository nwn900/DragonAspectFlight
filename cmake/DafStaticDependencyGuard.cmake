# Reject dependency selections that would make the plugin require unbundled
# spdlog/fmt runtime DLLs.  vcpkg builds have one supported ABI contract:
# static dependencies with the release-compatible dynamic MSVC CRT.

function(daf_require_static_md_vcpkg_triplet)
    if(NOT WIN32)
        return()
    endif()

    set(_daf_uses_vcpkg FALSE)
    if(DEFINED VCPKG_TARGET_TRIPLET OR DEFINED VCPKG_INSTALLED_DIR)
        set(_daf_uses_vcpkg TRUE)
    elseif(DEFINED CMAKE_TOOLCHAIN_FILE)
        file(TO_CMAKE_PATH "${CMAKE_TOOLCHAIN_FILE}" _daf_toolchain)
        string(TOLOWER "${_daf_toolchain}" _daf_toolchain_lower)
        if(_daf_toolchain_lower MATCHES "/vcpkg\.cmake$")
            set(_daf_uses_vcpkg TRUE)
        endif()
    endif()

    if(NOT _daf_uses_vcpkg)
        set(DAF_USING_VCPKG FALSE PARENT_SCOPE)
        message(STATUS
            "DAF static dependency guard: non-vcpkg build; dependency target types will still be verified")
        return()
    endif()

    if(NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
        message(FATAL_ERROR
            "DAF vcpkg builds require -DVCPKG_TARGET_TRIPLET=x64-windows-static-md. "
            "No explicit target triplet was selected; dynamic spdlog/fmt DLLs must not be used.")
    endif()
    if(NOT VCPKG_TARGET_TRIPLET STREQUAL "x64-windows-static-md")
        message(FATAL_ERROR
            "DAF vcpkg triplet '${VCPKG_TARGET_TRIPLET}' is unsupported. "
            "Use -DVCPKG_TARGET_TRIPLET=x64-windows-static-md so spdlog and fmt are linked statically.")
    endif()

    set(DAF_USING_VCPKG TRUE PARENT_SCOPE)
    message(STATUS "DAF vcpkg triplet: ${VCPKG_TARGET_TRIPLET} (static dependencies, dynamic CRT)")
endfunction()

function(daf_require_static_md_package_path variable_name)
    if(NOT DAF_USING_VCPKG)
        return()
    endif()
    if(NOT DEFINED ${variable_name} OR "${${variable_name}}" STREQUAL "")
        message(FATAL_ERROR
            "DAF could not prove the vcpkg package root: ${variable_name} is empty. "
            "Select packages from x64-windows-static-md explicitly.")
    endif()

    file(TO_CMAKE_PATH "${${variable_name}}" _daf_package_path)
    string(TOLOWER "${_daf_package_path}" _daf_package_path_lower)
    if(NOT _daf_package_path_lower MATCHES "/x64-windows-static-md(/|$)")
        message(FATAL_ERROR
            "DAF dependency path ${variable_name}='${${variable_name}}' is not from "
            "x64-windows-static-md. Dynamic or mixed-triplet package resolution is forbidden.")
    endif()
    message(STATUS "DAF static dependency path: ${variable_name}=${${variable_name}}")
endfunction()

function(daf_require_static_dependency_target target_name dependency_name)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR
            "DAF static dependency guard could not find target '${target_name}' for ${dependency_name}.")
    endif()

    get_target_property(_daf_aliased_target "${target_name}" ALIASED_TARGET)
    if(_daf_aliased_target)
        set(_daf_target "${_daf_aliased_target}")
    else()
        set(_daf_target "${target_name}")
    endif()

    get_target_property(_daf_target_type "${_daf_target}" TYPE)
    if(NOT _daf_target_type STREQUAL "STATIC_LIBRARY" AND
       NOT _daf_target_type STREQUAL "INTERFACE_LIBRARY")
        message(FATAL_ERROR
            "DAF requires a provably static ${dependency_name} target, but ${target_name} "
            "has type ${_daf_target_type}. Use x64-windows-static-md or a static non-vcpkg package.")
    endif()

    foreach(_daf_property IN ITEMS
        COMPILE_DEFINITIONS
        INTERFACE_COMPILE_DEFINITIONS
        INTERFACE_LINK_LIBRARIES
        IMPORTED_LOCATION
        IMPORTED_LOCATION_DEBUG
        IMPORTED_LOCATION_RELEASE
        IMPORTED_LOCATION_RELWITHDEBINFO
        IMPORTED_LOCATION_MINSIZEREL
        IMPORTED_IMPLIB
        IMPORTED_IMPLIB_DEBUG
        IMPORTED_IMPLIB_RELEASE
        IMPORTED_IMPLIB_RELWITHDEBINFO
        IMPORTED_IMPLIB_MINSIZEREL)
        get_target_property(_daf_value "${_daf_target}" "${_daf_property}")
        if(_daf_value AND NOT _daf_value MATCHES "-NOTFOUND$")
            string(TOUPPER "${_daf_value}" _daf_value_upper)
            if(_daf_value_upper MATCHES "SPDLOG_SHARED_LIB|FMT_SHARED")
                message(FATAL_ERROR
                    "DAF rejected shared ${dependency_name}: ${target_name} property ${_daf_property} "
                    "contains '${_daf_value}'. Use x64-windows-static-md.")
            endif()
            string(TOLOWER "${_daf_value}" _daf_value_lower)
            if(_daf_value_lower MATCHES "\\.dll($|[;])")
                message(FATAL_ERROR
                    "DAF rejected shared ${dependency_name}: ${target_name} property ${_daf_property} "
                    "references '${_daf_value}'. Use a static dependency target.")
            endif()
        endif()
    endforeach()

    message(STATUS
        "DAF static dependency target: ${target_name} (${_daf_target_type})")
endfunction()
