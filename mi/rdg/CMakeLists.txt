set(SOURCE_FILES
    include/rdg/rdg.h
    rdg.cpp
    rdg_resource.cpp
    rdg_base.cpp
    rdg_pass.cpp
    rdg_param.cpp
    rdg_shader.cpp
    rdg_pool.cpp
    rdg_builder.cpp
    rdg_cmd.cpp
    rdg_helper.cpp
    rdg_global_memory_collector.cpp
)

add_library(rdg STATIC ${SOURCE_FILES})
target_include_directories(rdg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(rdg PUBLIC core rhi xxHash::xxhash)

add_resources(rdg shaders "shaders/*.hlsl")