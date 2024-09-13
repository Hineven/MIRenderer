set(SOURCE_FILES
    include/util/lockfree.h
    include/util/alloc.h
    idle.cpp
)

add_library(util STATIC ${SOURCE_FILES})
target_include_directories(util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)


target_link_libraries(util PUBLIC core)