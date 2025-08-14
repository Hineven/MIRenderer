# MI
A set of CG libraries.
## 宇宙免责声明
* 框架还在持续性修修补补之中，可能有bug（或许是大量的bug、离谱的bug！），不要信任这个框架！
* 如果你在阅读代码时发现了非常傻逼的错误或丑陋的代码，请不要惊讶，这是正常的，请喊Hineven去修，或者你也可以自己动手！
* 如果你因为未知的框架bug而怒火中烧，请原谅Hineven【狗头】。
## 安装和编译
### 安装外部依赖
* NVIDIA RTX显卡。AMD显卡理论上支持，但未经过测试。
* `Vulkan SDK`: https://vulkan.lunarg.com/ ，安装最新的Vulkan SDK，请使用1.4.300更高版本。
### 安装Vcpkg
安装vcpkg包管理器，并使用包管理器与`CMake`集成，然后安装以下依赖：
* `glm`
* `imgui[glfw-binding]`：必须安装`glfw-binding`特性！
* `vulkan-memory-allocator`
* `vulkan-memory-allocator-hpp`
* `directx-dxc`
* `stb`
* `gtest`
* `cpptrace`
* `xxhash`
* `cgltf`
* `happly`
* `nlohmann-json`
# 使用
请`fork`此仓库并创建自己的分支。完成开发后，可以创建`pull request`将代码合并到主分支，并在我们之间共享你的实现！
### Target列表
* `3d_viewer`：一个简单的3D模型查看器，目前功能较为匮乏，是主要的测试程序，可以编辑其代码来加载不同模型。
* `core`：核心库。
* `rhi`：RHI抽象层。目前仅支持Vulkan。
* `rdg`：渲染图框架。
* `renderer`：渲染器实现。
* `util`：一些实用工具，比如模型加载。
* `micromc`：小玩具，用这个渲染器渲染几个mc区块。
### Vulkan Validation Layer配置
如果使用Vulkan Validation Layer，请禁用08742号警告（SPIRV字节码扩展能力不支持），此警告不影响程序正确性。
### 线程关系备注
* 线程分四类别：RHI线程、渲染线程、工作线程、主线程
* **只有**RHI线程负责与图形API交互，RHI线程只有一个
* **只有**渲染线程负责与RHI线程交互。一般而言，**只有**渲染线程能访问/**间接或直接持有**/使用RHI资源引用，渲染线程只有一个
* 在一些调试模式下，RHI线程和渲染线程合并成一个线程。
* 工作线程和主线程不能持有，也不能直接使用任何Device相关方法。它们应当仅限于在Host端进行计算和数据处理。
  * 创建、持有Geometry/StaticMesh等没有问题，但不能在线程内使用UpdateOnDevice等方法，也不应该持有DeviceXXXX的引用。