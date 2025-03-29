add_executable(hello main.cpp spinning_triangle.cpp)

# ImGUI
find_package(imgui)
if (imgui_FOUND)
    target_link_libraries(hello PRIVATE imgui::imgui)
else()
    message(FATAL_ERROR "imgui not found")
endif()


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
add_custom_target(hello_shaders DEPENDS ${SHADER_OUTPUTS})
add_dependencies(hello hello_shaders)

# Add the shader files to the target sources
target_sources(hello PRIVATE ${SHADER_FILES})

target_link_libraries(hello PUBLIC infra core rhi rdg glfw)