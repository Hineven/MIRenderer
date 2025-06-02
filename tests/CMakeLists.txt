# Add Google Test
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# Enable testing
enable_testing()

# Example
add_executable(example_test test_example.cpp)
target_link_libraries(example_test ${GTEST_LIBRARIES})
add_test(NAME example COMMAND example_test)

# Infra
add_executable(infra_test infra/infra_test.cpp)
target_link_libraries(infra_test infra core ${GTEST_LIBRARIES})
add_test(NAME infra COMMAND infra_test)

# RHI
find_package(Stb REQUIRED)
find_package(cpptrace CONFIG REQUIRED)
add_executable(rhi_test rhi/rhi_test.cpp rhi/rhi_test_bindless.cpp)
file(GLOB RHI_TEST_RESOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/rhi/resources/*.png")
add_custom_target(
        copy_rhi_resources ALL DEPENDS ${RHI_TEST_RESOURCE_FILES}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${RHI_TEST_RESOURCE_FILES}
        ${CMAKE_CURRENT_BINARY_DIR}/rhi/resources/
        COMMENT "Copying RHI resources to build directory"
)
target_sources(copy_rhi_resources PRIVATE ${RHI_TEST_RESOURCE_FILES})
add_dependencies(
        rhi_test
        copy_rhi_resources
)
add_custom_command(
        TARGET rhi_test
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
add_custom_target(
        copy_rdg_resources ALL DEPENDS ${RDG_TEST_SHADER_FILES}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${RDG_TEST_SHADER_FILES}
        ${CMAKE_CURRENT_BINARY_DIR}/rdg/resources/
        COMMENT "Copying RDG resources to build directory"
)
target_sources(copy_rdg_resources PRIVATE ${RDG_TEST_SHADER_FILES})
add_dependencies(
        rdg_test
        copy_rdg_resources
)

# Make sure the destination directory exists
add_custom_command(
        TARGET rdg_test
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
add_executable(util_test util/util_test.cpp)
target_link_libraries(util_test PRIVATE cpptrace::cpptrace infra rhi rdg util core ${GTEST_LIBRARIES})
