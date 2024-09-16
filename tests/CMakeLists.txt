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
target_link_libraries(infra_test infra core ${GTEST_LIBRARIES} ${dxc_library_target})
add_test(NAME infra COMMAND infra_test)

# RHI
add_executable(rhi_test rhi/rhi_test.cpp)
target_link_libraries(rhi_test infra rhi core ${GTEST_LIBRARIES})
add_test(NAME rhi COMMAND rhi_test)