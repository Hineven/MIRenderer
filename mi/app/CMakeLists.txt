set(SOURCE_FILES
        include/ml/ml.h
        include/ml/ml_desc.h
        include/ml/ml_fwd.h
        ml_start.cpp
)

add_library(ml STATIC ${SOURCE_FILES})
target_include_directories(ml PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(ml PUBLIC core rhi glfw)