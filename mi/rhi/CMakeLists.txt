set(SOURCE_FILES
    rhi_thread.cpp
    rhi_shader.cpp
    rhi_pipeline.cpp
    rhi_bindless.cpp
    rhi_resource.cpp
    rhi_cmd.cpp
    rhi_texture.cpp
    rhi_buffer.cpp
    rhi_param.cpp
    rhi_as.cpp
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

set(HEADER_FILES
        include/rhi/rhi.h
        include/rhi/rhi_thread.h
        include/rhi/rhi_shader.h
        include/rhi/rhi_pipeline.h
        include/rhi/rhi_bindless.h
        include/rhi/rhi_resource.h
        include/rhi/rhi_types.h
        include/rhi/rhi_types_string.h
        include/rhi/rhi_cmd.h
        include/rhi/rhi_texture.h
        include/rhi/rhi_buffer.h
        include/rhi/rhi_param.h
)

add_library(rhi STATIC ${SOURCE_FILES} ${HEADER_FILES})
target_include_directories(rhi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

list(
    APPEND
    EXTERNAL_LIBRARIES
    # Vulkan
    Vulkan::Vulkan
    # Vulkan Alloc
    GPUOpen::VulkanMemoryAllocator
    ${VMA_HPP_TARGET_NAME}
    # Reflection
    spirv-cross-core
    spirv-cross-glsl
    spirv-cross-hlsl
    spirv-cross-cpp
    # SPV processing
    SPIRV-Tools-static
    SPIRV-Tools-opt

    #    SPIRV-Tools-reduce
)

target_link_libraries(rhi PUBLIC core ${EXTERNAL_LIBRARIES})