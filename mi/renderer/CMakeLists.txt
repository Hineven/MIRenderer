set(SOURCES
        mi_resource_allocator.cpp
        mi_texture.cpp
        mi_material.cpp
        mi_transform.cpp
        mi_geometry.cpp
        mi_static_mesh.cpp
        mi_renderable.cpp
        mi_scene.cpp
        mi_renderer_view.cpp
        mi_renderer.cpp
        mi_helpers.cpp
        mi_buffer_heap.cpp
        # Implementing the renderer
        renderer/r_draw_to_output.cpp
        renderer/r_sky.cpp
        renderer/r_static_meshes.cpp
)

add_library(renderer ${SOURCES})

add_resources(renderer shaders "shaders/*.hlsl")

target_include_directories(renderer PUBLIC include)
target_link_libraries(renderer PUBLIC infra core rhi rdg)