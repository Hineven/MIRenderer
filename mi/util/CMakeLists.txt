set(SOURCES
        gltf_loader.cpp
        texture_loader.cpp
        openvdb_loader.cpp
        renderable_node.cpp
)

add_library(util ${SOURCES})

add_resources(util shaders "shaders/*.hlsl")

# tinyexr for exr loading
find_package(tinyexr CONFIG REQUIRED)
# openvdb for loading openvdb volume data
find_package(OpenVDB CONFIG REQUIRED)

target_include_directories(util PUBLIC include)
target_link_libraries(util PUBLIC infra core rhi rdg renderer unofficial::tinyexr::tinyexr OpenVDB::openvdb)