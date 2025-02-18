set(HEADER_FILES
    include/core/base.h
    include/core/common.h
    include/core/infra.h
    include/core/meta.h
    include/core/pixel_format.h
    include/core/platform.h
    include/core/refcounted.h
    include/core/thr.h
    include/core/task.h
    include/core/types.h
        include/core/util/alloc.h
        include/core/util/lockfree.h
)

set(SOURCE_FILES
    blobres.cpp
    task.cpp
    thr.cpp
)

add_library(core STATIC ${HEADER_FILES} ${SOURCE_FILES})
target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)