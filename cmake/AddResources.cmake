# add_resources(renderer shaders "shaders/*.hlsl")
function(add_resources target suffix files)
    file(GLOB_RECURSE RESOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/${files}")
    # Get the relative path of RESOURCE_FILES to the project root
    set(RESOURCE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/resources")
    message(STATUS "Resource output directory for ${target}_${suffix}: ${RESOURCE_OUTPUT_DIR}")

    file(MAKE_DIRECTORY ${RESOURCE_OUTPUT_DIR})

    foreach(RESOURCE_FILE ${RESOURCE_FILES})
        get_filename_component(FILENAME ${RESOURCE_FILE} NAME)
        file(RELATIVE_PATH RPATH ${CMAKE_SOURCE_DIR} ${RESOURCE_FILE})
        get_filename_component(RPATH_DIR ${RPATH} DIRECTORY)

        set(OUTPUT_SUBDIR "${RESOURCE_OUTPUT_DIR}/${RPATH_DIR}")
        file(MAKE_DIRECTORY ${OUTPUT_SUBDIR})

        set(OUTPUT_FILE "${RESOURCE_OUTPUT_DIR}/${RPATH}")
        add_custom_command(
                OUTPUT ${OUTPUT_FILE}
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${RESOURCE_FILE} ${OUTPUT_FILE}
                DEPENDS ${RESOURCE_FILE}
                COMMENT "Copying resource ${RPATH} to ${OUTPUT_FILE}"
        )
        list(APPEND RESOURCE_OUTPUTS ${OUTPUT_FILE})
    endforeach()

    set(resource_target "${target}_${suffix}")
    add_custom_target(${resource_target} DEPENDS ${RESOURCE_OUTPUTS})
    add_dependencies(${target} ${resource_target})

    # make sure that the build responds to changes in the resources
    target_sources(${target} PRIVATE ${RESOURCE_FILES})
    set_source_files_properties(${RESOURCE_FILES} PROPERTIES
            HEADER_FILE_ONLY ON  # Tell FXC to ignore these files. Do not compile them directly.
            VS_TOOL_OVERRIDE ""
    )
endfunction()