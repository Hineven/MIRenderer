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
add_executable(rhi_test rhi/rhi_test.cpp)
target_include_directories(rhi_test PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(rhi_test PRIVATE cpptrace::cpptrace infra rhi core ${GTEST_LIBRARIES})
add_test(NAME rhi COMMAND rhi_test)

# RDG
add_executable(rdg_test rdg/rdg_test.cpp)
target_link_libraries(rdg_test PRIVATE infra rhi rdg core ${GTEST_LIBRARIES})
add_test(NAME rdg COMMAND rdg_test)