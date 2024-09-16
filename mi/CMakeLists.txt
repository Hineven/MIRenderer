
# VMA-HPP from vcpkg
find_package(unofficial-vulkan-memory-allocator-hpp REQUIRED)

find_package(glm REQUIRED)

# core library
add_subdirectory(core)
# utilities
add_subdirectory(util)
# render hardware abstraction
add_subdirectory(rhi)
# render graph
add_subdirectory(rdg)
# top layer renderer logic
add_subdirectory(ml)
