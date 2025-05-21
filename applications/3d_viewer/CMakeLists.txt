set(SOURCES
    3d_viewer.cpp
    render_frame.cpp
)

add_executable(3d_viewer ${SOURCES})

# ImGUI
find_package(imgui)
if (imgui_FOUND)
    target_link_libraries(3d_viewer PRIVATE imgui::imgui)
else()
    message(FATAL_ERROR "imgui not found")
endif()

add_shaders(3d_viewer)
add_assets(3d_viewer)

target_link_libraries(3d_viewer PUBLIC infra core rhi rdg renderer util glfw)