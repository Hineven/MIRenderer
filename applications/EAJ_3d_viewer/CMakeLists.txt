set(SOURCES
    EAJ_3d_viewer.cpp
    render_frame.cpp
)

add_executable(EAJ_3d_viewer ${SOURCES})

# ImGUI
find_package(imgui)
if (imgui_FOUND)
    target_link_libraries(EAJ_3d_viewer PRIVATE imgui::imgui)
else()
    message(FATAL_ERROR "imgui not found")
endif()

add_resources(EAJ_3d_viewer shaders "shaders/*.hlsl")
add_resources(EAJ_3d_viewer assets "assets/*")

target_link_libraries(EAJ_3d_viewer PUBLIC infra core rhi rdg renderer util glfw)