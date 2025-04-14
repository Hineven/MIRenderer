set(SOURCES
        gltf_loader.cpp
)

add_library(util ${SOURCES})

target_include_directories(util PUBLIC include)
target_link_libraries(util PUBLIC infra core rhi rdg renderer)