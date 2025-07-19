# MI
A set of CG libraries.
### Extenal Dependencies
* `Vulkan SDK`: https://vulkan.lunarg.com/
### Vcpkg Dependencies
* `glm`
* `imgui[glfw-binding]`
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
* `cgltf`
* `happly`
* `nlohmann-json`
### 线程关系备注
* 线程分四类别：RHI线程、渲染线程、工作线程、主线程
* **只有**RHI线程负责与图形API交互，RHI线程只有一个
* **只有**渲染线程负责与RHI线程交互。一般而言，**只有**渲染线程能访问/**间接或直接持有**/使用RHI资源引用，渲染线程只有一个
* 在一些调试模式下，RHI线程和渲染线程合并成一个线程。
* 工作线程和主线程不能持有，也不能直接使用任何Device相关方法。它们应当仅限于在Host端进行计算和数据处理。
  * 创建、持有Geometry/StaticMesh等没有问题，但不能在线程内使用UpdateOnDevice等方法，也不应该持有DeviceXXXX的引用。