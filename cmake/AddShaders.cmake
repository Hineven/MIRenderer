function(add_shaders target)
    file(GLOB_RECURSE SHADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsl")

    set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/resources/shaders/${target}")
    message(STATUS "Shader output directory for ${target}: ${SHADER_OUTPUT_DIR}")

    file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

    foreach(SHADER_FILE ${SHADER_FILES})
        get_filename_component(FILENAME ${SHADER_FILE} NAME)
        file(RELATIVE_PATH RPATH ${CMAKE_CURRENT_SOURCE_DIR}/shaders ${SHADER_FILE})
        get_filename_component(RPATH_DIR ${RPATH} DIRECTORY)

        set(OUTPUT_SUBDIR "${SHADER_OUTPUT_DIR}/${RPATH_DIR}")
        file(MAKE_DIRECTORY ${OUTPUT_SUBDIR})

        set(OUTPUT_FILE "${SHADER_OUTPUT_DIR}/${RPATH}")
        add_custom_command(
                OUTPUT ${OUTPUT_FILE}
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SHADER_FILE} ${OUTPUT_FILE}
                DEPENDS ${SHADER_FILE}
                COMMENT "Copying shader ${RPATH} to ${OUTPUT_FILE}"
        )
        list(APPEND SHADER_OUTPUTS ${OUTPUT_FILE})
    endforeach()

    set(shader_target "${target}_shaders")
    add_custom_target(${shader_target} DEPENDS ${SHADER_OUTPUTS})
    add_dependencies(${target} ${shader_target})

    # make sure that the build responds to changes in the shader files
    target_sources(${target} PRIVATE ${SHADER_FILES})
endfunction()

function(add_assets target)
    file(GLOB_RECURSE ASSET_FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")

    set(ASSET_OUTPUT_DIR "${CMAKE_BINARY_DIR}/resources/assets/${target}")
    message(STATUS "Asset output directory for ${target}: ${ASSET_OUTPUT_DIR}")

    file(MAKE_DIRECTORY ${ASSET_OUTPUT_DIR})

    foreach(ASSET_FILE ${ASSET_FILES})
        get_filename_component(FILENAME ${ASSET_FILE} NAME)
        file(RELATIVE_PATH RPATH ${CMAKE_CURRENT_SOURCE_DIR}/assets ${ASSET_FILE})
        get_filename_component(RPATH_DIR ${RPATH} DIRECTORY)

        set(OUTPUT_SUBDIR "${ASSET_OUTPUT_DIR}/${RPATH_DIR}")
        file(MAKE_DIRECTORY ${OUTPUT_SUBDIR})

        set(OUTPUT_FILE "${ASSET_OUTPUT_DIR}/${RPATH}")
        add_custom_command(
                OUTPUT ${OUTPUT_FILE}
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${ASSET_FILE} ${OUTPUT_FILE}
                DEPENDS ${ASSET_FILE}
                COMMENT "Copying asset ${RPATH} to ${OUTPUT_FILE}"
        )
        list(APPEND ASSET_OUTPUTS ${OUTPUT_FILE})
    endforeach()

    set(asset_target "${target}_assets")
    add_custom_target(${asset_target} DEPENDS ${ASSET_OUTPUTS})
    add_dependencies(${target} ${asset_target})

    # make sure that the build responds to changes in the asset files
    target_sources(${target} PRIVATE ${ASSET_FILES})
endfunction()