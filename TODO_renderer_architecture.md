# Renderer Architecture TODO

## 背景

最近 `mi/renderer` 的主要问题已经不只是“文件变多”，而是几条职责边界开始同时变得模糊：

- `Renderer::Render()` 既负责构建一帧 RDG，又负责决定默认输出画什么、何时做 path tracing、何时把 overlay 叠到 backbuffer；
- `DeviceBindlessResourceAllocator` 既像 allocator，又像 renderer 的通用 device database，还承担了 feature 扩展入口；
- `application` 想要更多导出能力，但目前导出链条横跨 `Renderer`、`RendererView`、viewer 自己的 export binding、手工 readback 逻辑，存在重复映射和职责交叉。

这份 TODO 不追求一次性大重构，只记录当前最值得优先推进、且适合渐进迁移的几项改动。


## 当前最紧迫的几项改进

### 1. 把“默认输出 / present policy”从 `Renderer::Render()` 主流程中拿出去

#### 现状问题

- `Renderer::Render()` 尾部有一串基于 `CVar_FinalOutputType` 的 `DrawToOutput` 分支；
- 这部分本质上是 application / debug viewer 的展示策略，不是 renderer core 的职责；
- 继续把新需求堆在这里，`Renderer` 会越来越像具体应用，而不是 library。

#### 推荐做法

- 将 renderer 主入口逐步调整为 `BuildFrame(...)` 风格：
  - renderer 负责构建本帧需要的 RDG pass；
  - renderer 负责维护 frame-local products；
  - renderer 不直接决定“最后画哪张图到 FB”。
- 将默认的 final output 选择逻辑搬到 `3d_viewer` 或一个单独的 present helper 中。

#### 第一阶段小改法

- 保留现有 pass 组织不动；
- 先抽出一个类似 `Render_DefaultPresentation(...)` 或 viewer 侧 `PresentViewerFrame(...)` 的函数；
- 目标是先把 `CVar_FinalOutputType` 和尾部的 if-else 链从 `Renderer::Render()` 中移走。


### 2. 正式引入“Frame Products”边界，而不是继续直接暴露内部成员

#### 现状问题

- `RendererView::GetFrameExportResource()` 已经是个不错的雏形；
- 但 viewer 侧又维护了一套自己的 export channel / binding 表；
- 一份数据被 `RendererViewFrameExportResource`、`ViewerFrameExportChannel`、viewer 内部 tonemap/export 分支重复表达。

#### 推荐做法

- 将 `RendererView` 中可对外暴露的帧产物正式收敛为一套稳定的 products API；
- application 通过 products API 选择：
  - 哪些资源要 `SetExport()`；
  - 哪些资源要 present；
  - 哪些资源要 readback / 网络导出；
- renderer 对外暴露“语义化产品”，而不是鼓励 app 直接摸内部字段。

#### 推荐接口方向

- `RendererProductId`
- `RendererProductDesc`
- `RendererFrameProductsView`
- `GetProduct(RendererProductId id)`

#### 第一阶段小改法

- 保留 `RendererView::GetFrameExportResource()`；
- viewer 停止维护重复 enum，改为尽量直接基于 renderer 的 product id / descriptor 工作；
- viewer 自己新增的 tonemapped color / tonemapped path tracing 也应尽量收敛成 renderer 视角下的 product，而不是 viewer 私有分支。


### 3. 将“我要什么输出”交给 application，将“怎样补齐依赖 pass”留在 renderer

#### 现状问题

- viewer 目前不只是请求 export，还在决定：
  - 是否临时补 path tracing；
  - 是否先做 tonemap；
  - 是否叠 overlay；
- 这让 viewer 知道了太多 renderer 的内部 workflow。

#### 推荐做法

- application 只声明“我这帧要哪些 products”；
- renderer 负责判断：
  - 为了得到该 product 是否需要额外 pass；
  - 哪些 product 是 lazy / optional 的；
  - 哪些 product 依赖 path tracing / overlay / post process。

#### 推荐接口方向

- `RendererFrameRequest`
- `EnsureProduct(...)`
- `BuildFrame(view, builder, request)`

#### 关键原则

- app 决定“要什么”；
- renderer 决定“怎么做出来”；
- app 不应自己拼 feature 级 pass 依赖链。


### 4. 给 `DeviceBindlessResourceAllocator` 先做“接口瘦身”，再考虑物理拆分类

#### 现状问题

- 这个类已经同时承担：
  - bindless slot 分配；
  - 大量公共 device buffer / uber buffer 持有；
  - custom feature buffer 注册；
  - delayed destruction owner。
- renderer 各 pass 中对 allocator 的直接 import 和字段依赖非常多，重复且分散。

#### 推荐做法

- 不要先急着把 allocator 大拆八块；
- 第一阶段先引入一个 `RendererSceneBindings` / `SceneGpuResources` 聚合层：
  - 统一 import 常用的 renderable / geometry / material / static mesh / light structure buffers；
  - pass 只吃 bindings，不再各自重复从 allocator 拼参数。
- 第二阶段再视情况拆出：
  - `BindlessSlotAllocator`
  - `SceneGpuDatabase`
  - `FeatureGpuResourceRegistry`
  - `DelayedDestroyOwner`

#### 第一阶段小改法

- 找一两个最重复的 pass 先试点，例如：
  - `r_diffuse_direct_lighting.cpp`
  - `r_diffuse_indirect_lighting.cpp`
  - `r_volume_direct_lighting.cpp`
- 先证明这套 bindings API 确实能明显减参数装配重复度，再继续扩散。


### 5. 收紧 application 对 `GetDeviceAllocator()` 的直接依赖

#### 现状问题

- viewer 里有不少直接通过 `Renderer::GetDeviceAllocator()` 驱动资源上传/更新的代码；
- 这会让 application 长期依赖 renderer 内部 device 组织方式。

#### 推荐做法

- app 侧尽量不直接触碰 allocator；
- 引入更窄的接口来表达“scene 资源需要同步到 device”这件事；
- application 应关注资源语义，而不是 allocator 细节。

#### 第一阶段小改法

- 先把新增代码禁止继续直接依赖 `GetDeviceAllocator()`；
- 老代码暂时保留，但在后续 scene loading / resource upload 重构时逐步替换成更窄的同步入口。


## 推荐执行顺序

为了控制风险，推荐按下面顺序推进：

1. 先把 `Renderer::Render()` 尾部的默认输出选择逻辑搬出 renderer core。
2. 把 `RendererView::GetFrameExportResource()` 提升为正式的 frame products 边界。
3. 让 app 只声明所需 products，renderer 负责补齐额外 pass。
4. 引入 `RendererSceneBindings`，收敛 allocator 的重复 import。
5. 最后再考虑物理拆分 `DeviceBindlessResourceAllocator`。


## 暂时不建议做的事

- 不要立刻大规模重命名或移动 `mi/renderer` 下所有文件。
- 不要一开始就把 `RendererView` 整个变成 application 可随意修改的 public struct。
- 不要把“输出策略上移到 app”误解成“feature 级 RDG 编排也交给 app”。


## 一句话目标

中短期目标不是把 renderer 改成完全抽象的完美库，而是先把边界调整成下面这个形状：

- renderer 负责维护资源、构建 RDG、生产 frame products；
- application 负责选择 products、present/export/readback；
- app 不直接承担 renderer 内部 pass 依赖编排。
