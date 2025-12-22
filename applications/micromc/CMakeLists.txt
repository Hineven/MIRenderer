add_executable(micromc micromc.cpp micromc_render.cpp micromc_world.cpp micromc_world_types.cpp)

# ImGUI
find_package(imgui)
if (imgui_FOUND)
    target_link_libraries(micromc PRIVATE imgui::imgui)
else()
    message(FATAL_ERROR "imgui not found")
endif()

add_resources(micromc shaders "shaders/*.hlsl")

find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(micromc
        PUBLIC infra core rhi rdg glfw renderer util
        PRIVATE nlohmann_json::nlohmann_json
)
