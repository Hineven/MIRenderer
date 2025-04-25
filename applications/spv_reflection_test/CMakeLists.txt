add_executable(spv_reflection_test spv_reflection_test.cpp)

# 使用自定义目标来复制着色器文件
file(GLOB SHADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/*.hlsl")
set(SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/resources)

# 创建输出目录
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# 为每个着色器文件创建自定义命令和输出文件
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

# 创建自定义目标来复制着色器
add_custom_target(spv_reflection_test_shaders DEPENDS ${SHADER_OUTPUTS})
add_dependencies(spv_reflection_test spv_reflection_test_shaders)

# Add the shader files to the target sources
target_sources(spv_reflection_test PRIVATE ${SHADER_FILES})

target_link_libraries(spv_reflection_test PUBLIC infra core
        spirv-cross-core
        spirv-cross-glsl
        spirv-cross-hlsl
        spirv-cross-cpp
        # SPV processing
        SPIRV-Tools-static
        SPIRV-Tools-opt
)