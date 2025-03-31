# MI
A set of CG libraries.
### Extenal Dependencies
* `Vulkan SDK`: https://vulkan.lunarg.com/
### Vcpkg Dependencies
* `glm`
* `imgui[with glfw backend]`
* `spirv-cross`
* `spirv-reflect`
* `spirv-headers`
* `spirv-tools`
* `vulkan-memory-allocator`
* `vulkan-memory-allocator-hpp`
* `vulkan`
* `directx-dxc`
* `stb`
* `gtest`
* `cpptrace`
* `xxhash`
### TODO
* Infra
  * Res : ok
  * Mem : ok
  * Threading : ok
  * Compiler  : ok
* MI
  * rhi
    * vma is missing : ok
    * move sync out of rhi : ok
    * complete rhi worker threads : ok
    * test rhi functionality
  * rdg : wip
  * mesh
  * material
  * scene
  * ml
* sample
  * hello
  * triangle
  * fastmc
* gtest
  * infra : wip