set(SOURCES
        gltf_loader.cpp
        texture_loader.cpp
)

add_library(util ${SOURCES})

add_resources(util shaders "shaders/*.hlsl")

target_include_directories(util PUBLIC include)
target_link_libraries(util PUBLIC infra core rhi rdg renderer)