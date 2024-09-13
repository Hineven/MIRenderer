set(SOURCE_FILES
        include/infra_impl/infra.h
        include/infra_impl/infra_fwd.h
        infra_res.cpp
        infra_compiler.cpp
        infra.cpp
)

add_library(infra STATIC ${SOURCE_FILES})
target_include_directories(infra PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(infra PUBLIC core)