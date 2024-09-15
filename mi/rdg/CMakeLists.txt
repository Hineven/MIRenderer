set(SOURCE_FILES
        include/rdg/rdg.h
    rdg.cpp
    rdg_resource.cpp
)

add_library(rdg STATIC ${SOURCE_FILES})
target_include_directories(rdg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(rdg PUBLIC core util rhi)