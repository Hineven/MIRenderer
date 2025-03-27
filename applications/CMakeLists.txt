add_executable(hello hello/main.cpp hello/spinning_triangle.cpp)

target_link_libraries(hello PUBLIC infra core rhi rdg glfw)