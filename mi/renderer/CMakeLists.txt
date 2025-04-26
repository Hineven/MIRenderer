set(SOURCES
        mi_resource_allocator.cpp
        mi_geometry.cpp
        mi_renderer.cpp
        mi_static_mesh.cpp
        mi_texture.cpp
        mi_material.cpp
        mi_renderable.cpp
        mi_world.cpp
        mi_helpers.cpp
        mi_renderer_view.cpp
        # Implementing the renderer
        renderer/r_draw_to_output.cpp
)

add_library(renderer ${SOURCES})

file(GLOB SHADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsl")
set(SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/resources/shaders)

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

add_custom_target(renderer_shaders DEPENDS ${SHADER_OUTPUTS})
add_dependencies(renderer renderer_shaders)

# Add the shader files to the target sources
target_sources(renderer PRIVATE ${SHADER_FILES})
target_include_directories(renderer PUBLIC include)

target_link_libraries(renderer PUBLIC infra core rhi rdg)