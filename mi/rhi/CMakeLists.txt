set(SOURCE_FILES
        rhi_shader.cpp
        rhi_pipeline.cpp
        rhi_resource.cpp
        rhi_texture.cpp
        rhi_buffer.cpp
        rhi.cpp
)

add_library(rhi STATIC ${SOURCE_FILES})
target_include_directories(rhi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MI_BUILD_RHI_VULKAN)
    target_compile_definitions(rhi PUBLIC MI_RHI_VULKAN)
    # Our findvulkan fix
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
    # VMA-HPP from vcpkg
    find_package(VulkanMemoryAllocator REQUIRED GLOBAL)
    find_package(unofficial-vulkan-memory-allocator-hpp REQUIRED GLOBAL)
    find_package(Vulkan REQUIRED COMPONENTS dxc GLOBAL)
    if(NOT Vulkan_dxc_lib_FOUND OR NOT Vulkan_dxc_DEBUG_LIBRARY)
        # Fallback to standalone DXC
        message(STATUS "No integrated dxc found within Vulkan SDK, using standalone DXC")
        if(NOT ${Vulkan_dxc_DEBUG_LIBRARY})
            message(STATUS "Reason: dxc debug library not found")
        endif()
        # Standalone DXC
        find_package(directx-dxc CONFIG REQUIRED GLOBAL)
        set(dxc_library_target Microsoft::DirectXShaderCompiler)
    else()
        message(STATUS "Using integrated dxc within Vulkan SDK")
        set(dxc_library_target Vulkan::dxc_lib)
    endif()

    # Vulkan implementation
    list(APPEND SOURCE_FILES
        vk/vk_pipeline.cpp
        vk/vk_device.cpp
        vk/vk_cmd_exec.cpp
        vk/vk_texture.cpp
        vk/vk_resource.cpp
        vk/vk_buffer.cpp
        vk/vk_rhi.cpp
        vk/vk_shader.cpp
        vk/vma_impl.cpp
        vk/vk_dispatcher.cpp
    )

    # Link Vulkan libraries
    list(APPEND EXTERNAL_LIBRARIES
            ${dxc_library_target}
            Vulkan::Vulkan
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
            # SPV processing
            SPIRV-Tools-static
            SPIRV-Tools-opt
    )
endif()

if(MI_BUILD_RHI_D3D12)
    target_compile_definitions(rhi PUBLIC MI_RHI_D3D12)
    # DX12
    find_package(directx-headers CONFIG REQUIRED GLOBAL)
    find_package(directx12-agility CONFIG REQUIRED GLOBAL)
    if(TARGET Microsoft::DirectX12-Agility)
        file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/D3D12")
        add_custom_command(TARGET rhi POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PROPERTY:Microsoft::DirectX12-Core,IMPORTED_LOCATION_RELEASE> $<TARGET_FILE_DIR:rhi>/D3D12
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PROPERTY:Microsoft::DirectX12-Layers,IMPORTED_LOCATION_DEBUG> $<TARGET_FILE_DIR:rhi>/D3D12
                COMMAND_EXPAND_LISTS
        )
    endif()
        # Vulkan implementation
    list(APPEND SOURCE_FILES
            d3d12/d3d12_rhi_cmd.cpp
            d3d12/d3d12_rhi.cpp
            d3d12/d3d12_rhi_buffer.cpp
            d3d12/d3d12_rhi_intenral.cpp
    )
    list(APPEND EXTERNAL_LIBRARIES
        ${dxc_library_target}
        # D3D12
        Microsoft::DirectX-Headers
        Microsoft::DirectX-Guids
        Microsoft::DirectX12-Agility
        # Reflection
        spirv-cross-core
        spirv-cross-glsl
        spirv-cross-hlsl
        spirv-cross-cpp
        # SPV processing
        SPIRV-Tools-static
        SPIRV-Tools-opt
    )
endif()

target_link_libraries(rhi PUBLIC core ${EXTERNAL_LIBRARIES})