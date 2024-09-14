set(SOURCE_FILES
    rhi_worker.cpp
    rhi_shader.cpp
    rhi_pipeline.cpp
    rhi_bindless.cpp
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
)

add_library(rhi STATIC ${SOURCE_FILES})
target_include_directories(rhi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

list(
    APPEND
    EXTERNAL_LIBRARIES
    Vulkan::Vulkan
    VulkanMemoryAllocator
)

target_link_libraries(rhi PUBLIC core util ${EXTERNAL_LIBRARIES})