set(SOURCES
        gltf_loader.cpp
        texture_loader.cpp
        volprims_loader.cpp
)

add_library(util ${SOURCES})

add_resources(util shaders "shaders/*.hlsl")

# Happly for loading .ply point clouds
find_path(HAPPLY_INCLUDE_DIRS "happly.h")

target_include_directories(util PUBLIC include PRIVATE ${HAPPLY_INCLUDE_DIRS})
target_link_libraries(util PUBLIC infra core rhi rdg renderer)