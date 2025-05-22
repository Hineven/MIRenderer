add_executable(hello hello.cpp spinning_triangle.cpp)

# ImGUI
find_package(imgui)
if (imgui_FOUND)
    target_link_libraries(hello PRIVATE imgui::imgui)
else()
    message(FATAL_ERROR "imgui not found")
endif()

add_resources(hello shaders "shaders/*.hlsl")

target_link_libraries(hello PUBLIC infra core rhi rdg glfw)