set(SOURCES
        mi_renderer_export.cpp
        mi_console.cpp
        mi_resource_allocator.cpp
        mi_texture.cpp
        mi_volume_texture.cpp
        mi_material.cpp
        mi_transform.cpp
        mi_geometry.cpp
        mi_static_mesh.cpp
        mi_volume_grid.cpp
        mi_giga_voxel.cpp
        mi_giga_voxel_heap.cpp
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
        renderer/r_geometry_buffer.cpp
        renderer/r_composition.cpp
        renderer/r_sky.cpp
        renderer/r_static_meshes.cpp
        renderer/r_giga_voxel.cpp
        renderer/r_shadow_map.cpp
        renderer/r_hzb.cpp
        renderer/r_hwrt.cpp
        renderer/r_direct_lighting.cpp
        renderer/r_diffuse_direct_lighting.cpp
        renderer/r_volume_grid_direct_lighting.cpp
        renderer/r_diffuse_indirect_lighting.cpp
        renderer/r_path_tracing.cpp
        renderer/r_debug_views.cpp
        renderer/r_denoiser.cpp
        renderer/r_world_radiance_cache.cpp
        renderer/r_light_structure.cpp
        renderer/r_light_cluster_hiearchy.cpp
        dlss/ngx_context.cpp
        dlss/dlss_rr_context.cpp
        util/radix_sort.cpp
        util/scan_sum.cpp
)

add_library(renderer ${SOURCES})

add_resources(renderer shaders "shaders/*.hlsl")

target_include_directories(renderer PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(renderer PUBLIC infra core rhi rdg)

# DLSS / NGX SDK
set(NGX_SDK_ROOT ${CMAKE_SOURCE_DIR}/external/DLSS)
target_include_directories(renderer PRIVATE ${NGX_SDK_ROOT}/include)
if(WIN32)
    set(NGX_LIB_DEBUG ${NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib)
    set(NGX_LIB_RELEASE ${NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib)
    target_link_libraries(renderer PRIVATE
        $<$<CONFIG:Debug>:${NGX_LIB_DEBUG}>
        $<$<CONFIG:RelWithDebInfo>:${NGX_LIB_RELEASE}>
        $<$<CONFIG:Release>:${NGX_LIB_RELEASE}>
        $<$<CONFIG:MinSizeRel>:${NGX_LIB_RELEASE}>
    )
else()
    set(NGX_LIB ${NGX_SDK_ROOT}/lib/Linux_x86_64/libnvsdk_ngx.a)
    target_link_libraries(renderer PRIVATE ${NGX_LIB})
endif()