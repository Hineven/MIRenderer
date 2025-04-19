set(SOURCES
    3d_viewer.cpp
    render_frame.cpp
)

add_executable(3d_viewer ${SOURCES})

# ImGUI
find_package(imgui)
if (imgui_FOUND)
    target_link_libraries(3d_viewer PRIVATE imgui::imgui)
else()
    message(FATAL_ERROR "imgui not found")
endif()

file(GLOB SHADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/*.hlsl")
set(SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/resources)

file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

foreach(SHADER_FILE ${SHADER_FILES})
    get_filename_component(FILENAME ${SHADER_FILE} NAME)
    set(OUTPUT_FILE "${SHADER_OUTPUT_DIR}/${FILENAME}")
    add_custom_command(
            OUTPUT ${OUTPUT_FILE}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SHADER_FILE} ${OUTPUT_FILE}
            DEPENDS ${SHADER_FILE}
            COMMENT "Copying shader ${FILENAME}"
    )
    list(APPEND SHADER_OUTPUTS ${OUTPUT_FILE})
endforeach()

add_custom_target(3d_viewer_shaders DEPENDS ${SHADER_OUTPUTS})
add_dependencies(3d_viewer 3d_viewer_shaders)

# Add the shader files to the target sources
target_sources(3d_viewer PRIVATE ${SHADER_FILES})

target_link_libraries(3d_viewer PUBLIC infra core rhi rdg glfw)