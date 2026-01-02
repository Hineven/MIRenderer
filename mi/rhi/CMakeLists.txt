# Function to create SPIRV library targets
function(create_spirv_targets)
    # Get Vulkan library directory
    get_filename_component(VULKAN_LIB_DIR ${Vulkan_LIBRARY} DIRECTORY)

    # List of SPIRV libraries we want to link
    set(SPIRV_LIBS
        spirv-cross-core
        spirv-cross-glsl
        spirv-cross-hlsl
        spirv-cross-cpp
        SPIRV-Tools-opt
    )

    # Create imported targets for each SPIRV library
    foreach(SPIRV_LIB ${SPIRV_LIBS})
        # Determine library filename based on build type
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(LIB_FILENAME "${SPIRV_LIB}d.lib")
            if(NOT EXISTS "${VULKAN_LIB_DIR}/${LIB_FILENAME}")
                set (LIB_FILENAME "${SPIRV_LIB}.lib")
            endif()
        else()
            set(LIB_FILENAME "${SPIRV_LIB}.lib")
        endif()

        set(LIB_PATH "${VULKAN_LIB_DIR}/${LIB_FILENAME}")

        # Check if library exists
        if(EXISTS ${LIB_PATH})
            # Create imported target
            add_library(Vulkan::${SPIRV_LIB} STATIC IMPORTED)
            set_target_properties(Vulkan::${SPIRV_LIB} PROPERTIES
                IMPORTED_LOCATION ${LIB_PATH}
            )
            message(STATUS "Found SPIRV library: ${LIB_PATH}")
        else()
            message(WARNING "SPIRV library not found: ${LIB_PATH}")
        endif()
    endforeach()
endfunction()

# Create SPIRV targets
create_spirv_targets()

set(SOURCE_FILES
    rhi_thread.cpp
    rhi_shader.cpp
    rhi_pipeline.cpp
    rhi_bindless.cpp
    rhi_resource.cpp
    rhi_cmd.cpp
    rhi_cmd_stats.cpp
    rhi_texture.cpp
    rhi_buffer.cpp
    rhi_param.cpp
    rhi_as.cpp
    rhi.cpp
        vk/vk_pipeline.cpp
        vk/vk_cmd_exec.cpp
        vk/vk_cmd_exec_common.cpp
        vk/vk_cmd_exec_rt.cpp
        vk/vk_texture.cpp
        vk/vk_resource.cpp
        vk/vk_buffer.cpp
        vk/vk_as.cpp
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
    Vulkan::spirv-cross-core
    Vulkan::spirv-cross-glsl
    Vulkan::spirv-cross-hlsl
    Vulkan::spirv-cross-cpp
    # SPV processing
    Vulkan::SPIRV-Tools
    Vulkan::SPIRV-Tools-opt

)

target_link_libraries(rhi PUBLIC core ${EXTERNAL_LIBRARIES})