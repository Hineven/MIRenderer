set(SOURCE_FILES
        rhi_thread.cpp
    rhi_shader.cpp
    rhi_pipeline.cpp
    rhi_bindless.cpp
    rhi_resource.cpp
    rhi_cmd.cpp
    rhi_texture.cpp
    rhi_buffer.cpp
    rhi.cpp
        vk/vk_pipeline.cpp
        vk/vk_device.cpp
        vk/vk_cmd_exec.cpp
        vk/vk_texture.cpp
        vk/vk_resource.cpp
        vk/vk_buffer.cpp
        vk/vk_bindless.cpp
        vk/vk_rhi.cpp
        vk/vk_shader.cpp
        vk/vma_impl.cpp
        vk/vk_dispatcher.cpp
)

add_library(rhi STATIC ${SOURCE_FILES})
target_include_directories(rhi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

list(
    APPEND
    EXTERNAL_LIBRARIES
    # Vulkan
    Vulkan::Vulkan
    # Vulkan Alloc
    GPUOpen::VulkanMemoryAllocator
    unofficial::VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
    # Reflection
    spirv-cross-core
    spirv-cross-glsl
    spirv-cross-hlsl
    spirv-cross-cpp
)

target_link_libraries(rhi PUBLIC core ${EXTERNAL_LIBRARIES})