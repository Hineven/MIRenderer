# Add Google Test
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# Enable testing
enable_testing()

# Example
add_executable(example_test test_example.cpp)
target_link_libraries(example_test ${GTEST_LIBRARIES})
add_test(NAME example COMMAND example_test)

# Core - Task System Test
add_executable(core_task_test core/test_task_system.cpp)
target_link_libraries(core_task_test core infra ${GTEST_LIBRARIES})
add_test(NAME core_task COMMAND core_task_test)

# Core - Command Line Test
add_executable(core_command_line_test core/test_command_line.cpp)
target_link_libraries(core_command_line_test core infra ${GTEST_LIBRARIES})
add_test(NAME core_command_line COMMAND core_command_line_test)

# Core - SizeClassAllocator Test
add_executable(core_size_class_allocator_test core/test_size_class_allocator.cpp)
target_link_libraries(core_size_class_allocator_test core ${GTEST_LIBRARIES})
add_test(NAME core_size_class_allocator COMMAND core_size_class_allocator_test)

# Infra
add_executable(infra_test infra/infra_test.cpp)
target_link_libraries(infra_test infra core ${GTEST_LIBRARIES})
add_test(NAME infra COMMAND infra_test)

# RHI
find_package(Stb REQUIRED)
find_package(cpptrace CONFIG REQUIRED)
add_executable(rhi_test rhi/rhi_test.cpp rhi/rhi_test_bindless.cpp rhi/rhi_test_rt.cpp)
file(GLOB RHI_TEST_RESOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/rhi/resources/*")
set(RHI_DEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/rhi/resources)
add_custom_target(
        copy_rhi_resources ALL
        # 1. 先创建目录
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RHI_DEST_DIR}
        # 2. 再复制文件
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${RHI_TEST_RESOURCE_FILES}
        ${RHI_DEST_DIR}
        DEPENDS ${RHI_TEST_RESOURCE_FILES}
        COMMENT "Copying RHI resources to build directory"
)
add_dependencies(
        rhi_test
        copy_rhi_resources
)
# Make sure the destination directory exists
add_custom_command(
        TARGET copy_rhi_resources
        PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
        ${CMAKE_CURRENT_BINARY_DIR}/rhi/resources/
)
target_include_directories(rhi_test PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(rhi_test PRIVATE cpptrace::cpptrace infra rhi core ${GTEST_LIBRARIES})
add_test(NAME rhi COMMAND rhi_test)

# RDG
add_executable(rdg_test rdg/rdg_test.cpp)
# Create custom commands to copy the shader file at build time
file(GLOB RDG_TEST_SHADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/rdg/resources/*.hlsl")
set(RDG_DEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/rdg/resources)
add_custom_target(
        copy_rdg_resources ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RDG_DEST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${RDG_TEST_SHADER_FILES}
        ${RDG_DEST_DIR}
        DEPENDS ${RDG_TEST_SHADER_FILES}
        COMMENT "Copying RDG resources to build directory"
)
add_dependencies(
        rdg_test
        copy_rdg_resources
)

# Make sure the destination directory exists
add_custom_command(
        TARGET copy_rdg_resources
        PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
        ${CMAKE_CURRENT_BINARY_DIR}/rdg/resources/
)
# Add resource file to the test target
target_sources(rdg_test PRIVATE ${RDG_TEST_SHADER_FILES})
target_include_directories(rdg_test PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(rdg_test PRIVATE cpptrace::cpptrace infra rhi rdg core ${GTEST_LIBRARIES})
add_test(NAME rdg COMMAND rdg_test)

# Util
add_executable(util_test util/util_test.cpp util/test_fixed_allocator.cpp)
target_link_libraries(util_test PRIVATE cpptrace::cpptrace infra rhi rdg util core ${GTEST_LIBRARIES})
add_test(NAME util COMMAND util_test)

# Renderer - Light Cluster Hierarchy
add_executable(
        renderer_light_cluster_test
        renderer/test_light_cluster_hierarchy.cpp
        renderer/test_light_cluster_hierarchy_gltf_template.cpp
)

file(GLOB RENDERER_TEST_RESOURCE_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/renderer/resources/*.gltf"
        "${CMAKE_CURRENT_SOURCE_DIR}/renderer/resources/*.glb"
        "${CMAKE_CURRENT_SOURCE_DIR}/renderer/resources/*.bin"
        "${CMAKE_CURRENT_SOURCE_DIR}/renderer/resources/*.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/renderer/resources/*.jpg"
        "${CMAKE_CURRENT_SOURCE_DIR}/renderer/resources/*.jpeg"
)
set(RENDERER_DEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/renderer/resources)
if(RENDERER_TEST_RESOURCE_FILES)
    add_custom_target(
            copy_renderer_resources ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RENDERER_DEST_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${RENDERER_TEST_RESOURCE_FILES}
            ${RENDERER_DEST_DIR}
            DEPENDS ${RENDERER_TEST_RESOURCE_FILES}
            COMMENT "Copying renderer test resources to build directory"
    )
else()
    add_custom_target(
            copy_renderer_resources ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory ${RENDERER_DEST_DIR}
            COMMENT "No renderer test resources found yet; created destination directory"
    )
endif()
add_dependencies(renderer_light_cluster_test copy_renderer_resources)

target_link_libraries(renderer_light_cluster_test PRIVATE renderer util ${GTEST_LIBRARIES})
add_test(NAME renderer_light_cluster COMMAND renderer_light_cluster_test)

# Renderer - KD-Tree
add_executable(renderer_kd_tree_test renderer/test_kd_tree.cpp)
target_link_libraries(renderer_kd_tree_test PRIVATE renderer util ${GTEST_LIBRARIES})
add_test(NAME renderer_kd_tree COMMAND renderer_kd_tree_test)

# MacroMC - World (chunk / palette / subchunk / worldgen)
add_executable(macromc_world_test macromc/world_test.cpp)
target_link_libraries(macromc_world_test PRIVATE macromc_world macromc_worldgen macromc_registry core ${GTEST_LIBRARIES})
add_test(NAME macromc_world COMMAND macromc_world_test)

# MacroMC - Registry
add_executable(macromc_registry_test macromc/registry_test.cpp)
target_link_libraries(macromc_registry_test PRIVATE macromc_registry core ${GTEST_LIBRARIES})
add_test(NAME macromc_registry COMMAND macromc_registry_test)

# MacroMC - Greedy Meshing (CPU algorithm + multi-threaded scheduler)
add_executable(macromc_greedy_meshing_test macromc/greedy_meshing_test.cpp)
target_link_libraries(macromc_greedy_meshing_test PRIVATE
    macromc_meshing macromc_world macromc_registry core infra ${GTEST_LIBRARIES})
mi_apply_app_manifest(macromc_greedy_meshing_test)
add_test(NAME macromc_greedy_meshing COMMAND macromc_greedy_meshing_test)

# MacroMC - Streaming (envelope driver + registry + meshing, headless stress)
add_executable(macromc_streaming_test macromc/streaming_test.cpp)
target_link_libraries(macromc_streaming_test PRIVATE
    macromc_meshing macromc_world macromc_worldgen macromc_registry core infra ${GTEST_LIBRARIES})
mi_apply_app_manifest(macromc_streaming_test)
add_test(NAME macromc_streaming COMMAND macromc_streaming_test)

# MacroMC - Profiler (timers + counters + gauges, thread-local merge)
add_executable(macromc_profiler_test macromc/profiler_test.cpp)
target_link_libraries(macromc_profiler_test PRIVATE
    macromc_prof core infra ${GTEST_LIBRARIES})
mi_apply_app_manifest(macromc_profiler_test)
add_test(NAME macromc_profiler COMMAND macromc_profiler_test)
