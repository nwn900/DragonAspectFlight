if(NOT DEFINED DAF_SOURCE_DIR)
    message(FATAL_ERROR "DAF_SOURCE_DIR is required")
endif()

set(oar_root
    "${DAF_SOURCE_DIR}/Data/meshes/actors/character/animations/OpenAnimationReplacer/More Dragonic Dragon Aspect Can Fly")
set(flying_config "${oar_root}/Flying Mod/config.json")
set(elegant_config "${oar_root}/Elegant Flying Animations/config.json")

foreach(legacy_config IN ITEMS
    "${oar_root}/Dragon Aspect Flight - Flying Mod Patch/config.json"
    "${oar_root}/Dragon Aspect Flight - Elegant Flying Patch/config.json")
    if(EXISTS "${legacy_config}")
        message(FATAL_ERROR
            "Legacy sibling-folder OAR indirection must not ship: ${legacy_config}")
    endif()
endforeach()

foreach(config IN ITEMS "${flying_config}" "${elegant_config}")
    if(NOT EXISTS "${config}")
        message(FATAL_ERROR
            "Missing exact-path More Draconic config overlay: ${config}")
    endif()

    file(READ "${config}" config_json)
    foreach(required_token IN ITEMS
        "\"condition\": \"OR\""
        "\"condition\": \"AND\""
        "\"condition\": \"HasMagicEffect\""
        "\"condition\": \"IsInAir\""
        "\"graphVariable\": \"bDAF_FlightActive\""
        "\"graphVariable\": \"iDAF_FlightState\"")
        string(FIND "${config_json}" "${required_token}" token_offset)
        if(token_offset EQUAL -1)
            message(FATAL_ERROR
                "${config} does not preserve the required original-or-DAF condition contract: ${required_token}")
        endif()
    endforeach()

    string(FIND "${config_json}" "overrideAnimationsFolder" override_offset)
    if(NOT override_offset EQUAL -1)
        message(FATAL_ERROR
            "${config} must parse animations from its merged virtual directory, not overrideAnimationsFolder")
    endif()
endforeach()

file(READ "${elegant_config}" elegant_json)
string(FIND "${elegant_json}" "\"condition\": \"IsFemale\"" female_offset)
if(female_offset EQUAL -1)
    message(FATAL_ERROR "Elegant Flying Animations overlay must remain female-only")
endif()

file(GLOB_RECURSE bundled_hkx LIST_DIRECTORIES false "${DAF_SOURCE_DIR}/Data/*.hkx")
if(bundled_hkx)
    list(LENGTH bundled_hkx bundled_hkx_count)
    message(FATAL_ERROR
        "Reference-only package must not bundle HKX files; found ${bundled_hkx_count}")
endif()

message(STATUS "DAF exact-path OAR reference overlay contract passed")
