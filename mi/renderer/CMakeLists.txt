set(SOURCES
        mi_resource_allocator.cpp
        mi_texture.cpp
        mi_material.cpp
        mi_transform.cpp
        mi_geometry.cpp
        mi_static_mesh.cpp
        mi_volume_primitives.cpp
        mi_renderable.cpp
        mi_scene.cpp
        mi_renderer_view.cpp
        mi_renderer.cpp
        mi_renderer_init.cpp
        mi_buffer_heap.cpp
        mi_noise.cpp
        mi_dirty_tracker.cpp
        mi_cvar.cpp
        # Implementing the renderer
        renderer/r_composition.cpp
        renderer/r_sky.cpp
        renderer/r_static_meshes.cpp
        renderer/r_volume_primitives.cpp
        renderer/r_shadow_map.cpp
        renderer/r_hzb.cpp
        renderer/r_hwrt.cpp
        renderer/r_diffuse_direct_lighting.cpp
        renderer/r_diffuse_indirect_lighting.cpp
        renderer/r_path_tracing.cpp
        renderer/r_debug_views.cpp
        renderer/r_denoise.cpp
        util/radix_sort.cpp
)

add_library(renderer ${SOURCES})

add_resources(renderer shaders "shaders/*.hlsl")

target_include_directories(renderer PUBLIC include)
target_link_libraries(renderer PUBLIC infra core rhi rdg)