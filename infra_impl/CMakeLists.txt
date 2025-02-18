set(SOURCE_FILES
        include/infra_impl/infra.h
        infra_res.cpp
        infra_compiler.cpp
        infra.cpp
)

# Shader compiler
find_package(directx-dxc CONFIG REQUIRED GLOBAL)
set(dxc_library_target Microsoft::DirectXShaderCompiler)

add_library(infra STATIC ${SOURCE_FILES})
target_include_directories(infra PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(infra PUBLIC core ${dxc_library_target})