# MIRenderer
一个用于研究实时光照的渲染框架和一些功能实现。
![cover](images/cover.png)
## 宇宙免责声明
* 框架还在持续性修修补补之中，可能有bug，不要太信任此框架！
## 安装和编译
### 安装外部依赖
* NVIDIA RTX显卡。AMD显卡理论上支持，但未经过测试。
* `Vulkan SDK`: https://vulkan.lunarg.com/ ，安装最新的Vulkan SDK，请使用1.4.300更高版本，较低版本会出现意外错误。
### 安装Vcpkg
安装vcpkg包管理器，并使用包管理器与`CMake`集成，然后安装以下依赖：
* `glm`
* `imgui[glfw-binding]`：必须安装`glfw-binding`特性！
* `stb`
* `gtest`
* `cpptrace`
* `xxhash`
* `cgltf`
* `happly`
* `nlohmann-json`
* `glfw3`
* `tinyexr`
* `openvdb`
* `tinyexr` <- 似乎最近此包可能因为CmakeBug无法安装，你可以回退到较早版本，或者切换CMake版本（3.30.1可用）。
* `vulkan-memory-allocator`
* `directx-dxc`
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
### 调试
* 在使用`Vulkan Configurator`时，可以开启`Vulkan Validation Layer`与`Break on Validation Error`选项，此时，出现问题时程序会自动中断，你可以用IDE查看栈帧。
  * 开启`Vulkan Validation Layer`后性能下降是正常的。
* 如果要深入调试Shader，请使用`NSight Graphics`进行抓帧，抓帧后可以查看Shader代码、资源绑定、实时检视资源内容等信息。
* 你可以在Shader中使用`printf`函数进行调试输出，此时，请开启`Vulkan Validation Layer`，并开启`Debug Printf`选项，输出会显示在控制台中。
### 线程关系备注
* 线程分四类别：RHI线程、渲染线程、工作线程、主线程
* **只有**RHI线程负责与图形API交互，RHI线程只有一个
* **只有**渲染线程负责与RHI线程交互。一般而言，**只有**渲染线程能访问/**间接或直接持有**/使用RHI资源引用，渲染线程只有一个
* 在一些调试模式下，RHI线程和渲染线程合并成一个线程。
* 工作线程和主线程不能持有，也不能直接使用任何Device相关方法。它们应当仅限于在Host端进行计算和数据处理。
  * 创建、持有Geometry/StaticMesh等没有问题，但不能在线程内使用UpdateOnDevice等方法，也不应该持有DeviceXXXX的引用。