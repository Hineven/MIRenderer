# GigaVoxel 统一 Visibility Buffer 设计

> **范围说明**：本文档定义 MIRenderer 的 **renderer 层 visibility buffer 格式**，
> 它同时影响 `mi/renderer` 的现有 static mesh 路径和 `applications/macromc` 的 GigaVoxel 接入。
> 虽然放在 macromc planning 下（因为由 GigaVoxel 需求驱动），但它是 renderer 层的通用格式规范。

## 1. 背景与动机

### 1.1 现状

MIRenderer 的 static mesh 走 **visibility buffer + deferred decode** 路径：

```
DrawDeferredStaticMeshes (HW raster)
  PS 写 G_visibility_ (R32G32B32A32_UINT) + G_depth_ (D32, reversed-z)
       ↓
DecodeVisibility (compute, 16x16 tiles)
  读 G_visibility_ + G_depth_ → 解码出 RenderableIndex / PrimitiveIndex / Barycentrics
  → 查 RenderableHeader / Material / Geometry 表 → 写出 G_albedo / G_normal / ... G_motion_vector
```

当前 visibility 位分配（`mi/renderer/shaders/DrawStaticMeshes.hlsl`）：

```
uint4 Visibility:
  x = DescriptorIndex(8) << 24 | RenderableIndex(24)
  y = PrimitiveIndex (32bit)
  z = Barycentrics.y (f32)
  w = Barycentrics.z (f32)
```

- `RenderableIndex` 占 24bit（实际仅用了高位打包），但 ray tracing 的 `InstanceCustomIndex` 只用 20bit（`RENDERABLE_INDEX_NUM_BITS = 20`，见 `SharedRenderable.hlsl`），两套编码不对齐。
- `DescriptorIndex` 硬编码在 x 高 8 位，限制了 `kMaxNumGeometries = 256`。
- 整个 payload 语义绑定在 static mesh 的"三角形 + bary"模型上，无法容纳 GigaVoxel 这类非三角形几何。

### 1.2 目标

引入 GigaVoxel（VC/TFC/LFC）后，需要一种 visibility 编码，使得：

1. **static mesh 行为零变化**——现有场景渲染结果不变。
2. **GigaVoxel 三种 LOD（VC/TFC/LFC）共用同一 target、同一 decode pass**——下游光照/后处理不感知几何类型。
3. **LFC（远场）decode 极轻量**——不追求从 visibility 完美恢复所有 block 属性。
4. **visibility 与 ray tracing 共享 renderable index 编码**——收敛散乱的两套 index 体系。
5. **payload 按类型自由分配**——不强求"形状统一"，每种 renderable 用自己最优的编码。

### 1.3 主流做法对比

| 路线 | 代表 | 特点 | 本方案取舍 |
|------|------|------|-----------|
| 类型化 payload + 分发 | UE5 Nanite / Lumen | visibility 存 renderable id + 类型化 payload，decode 按 type switch | **采用** |
| 每类独立 target + 独立 decode | 早期 voxel 引擎 | 各类型 visibility 自由设计，但多 target / 多 pass / depth 合成复杂 | 不采用（违反"单 target"目标） |
| 最小 payload + 外移表 | Nanite 现行 | visibility 只存 cluster instance id + triangle id，属性全查表 | 部分采用（LFC 颜色直接进 payload 是反例，但 VC 复用查表） |

本方案是"类型化 payload + 分发"为主，结合"最小 payload"的局部优化（LFC）。

---

## 2. 位分配规范

### 2.1 通道总览

```
uint4 Visibility (128bit):
  x[19:0]   RenderableIndex   (20bit)  ← 路由键: 指向 Renderable (StaticMesh instance 或 GigaVoxelRenderable)
  x[31:20]  RenderableType    (12bit)  ← 第一时间决定 decode 分支, 现仅用低 3bit
  y         Payload0          (32bit)  ← 按 Type 自由编码
  z         Payload1          (32bit)
  w         Payload2          (32bit)
```

- **RenderableIndex 20bit**：容量 1M，与 ray tracing 的 `INSTANCE_CUSTOM_INDEX_INDEX_MASK`（`SharedRenderable.hlsl`）对齐。这是 visibility 与 ray tracing 共享 renderable index 编码的收敛点。
- **RenderableType 12bit**：现 `RenderableType` 枚举仅 5 个值（3bit），12bit 留富余。decode 第一条指令即可 `switch(type)` 分支，无需先查 `RenderableHeaderBuffer`——对全屏 decode pass（1080p ≈ 2M 像素）省掉 2M 次 header 表随机读，cache 友好。
- **Payload y/z/w 共 96bit**：语义由 `RenderableType` 决定，decode 时按类型解释。不追求跨类型形状统一。

### 2.2 判空约定

所有路径统一用 **`Depth == 0`** 判定空像素（reversed-z，0 = 远裁面 = 未写过）。
HW raster 和 SW raster 都写同一个 `G_depth_`，decode pass 读 depth 判空后读 visibility。

---

## 3. 各 RenderableType 的 payload 编码

### 3.1 StaticMesh (`kStaticMeshInstance`)

保持现有 static mesh 语义，仅把 `DescriptorIndex` 从 x 高位挪到 y 低位（释放 x 高位给 RenderableType）。

```
y[7:0]    DescriptorIndex   (8bit,  ≤256 geometries/mesh, 对应 kMaxNumGeometries)
y[31:8]   PrimitiveIndex    (24bit, ≤16M triangles/geometry)
z         Barycentrics.y    (f32)
w         Barycentrics.z    (f32,  .x = 1 - .y - .z, 由 decode 推导)
```

**decode**：`RenderableIndex → RenderableHeader → StaticMeshIndex → StaticMeshHeader.DescriptionOffset`，加 `DescriptorIndex` 得全局描述符索引 → 查 `StaticMeshDescriptionBuffer` 得 (geometry, material) 对 → 用 `PrimitiveIndex + Barycentrics` 在 geometry 的 index/vertex buffer 内插值出 Albedo/Normal/UV/Emission/MetallicRoughness → 写 G-buffer。

**与现状的改动**：仅三处机械改动——
- VS: `Output.DescriptorRenderableIndex = DescriptorIndex << 24 | RenderableIndex` → 改为新打包（x 分 RenderableIndex+Type，y 装 DescriptorIndex+PrimitiveIndex）
- PS: `Output.Visibility = uint4(Input.DescriptorRenderableIndex, PrimitiveIndex, Bary.y, Bary.z)` → 重新排列
- Decode: `RenderableIndex = Visibility.x & 0xFFFFFF; DescriptorRank = (Visibility.x >> 24) & 0xFF;` → 改为新解包

static mesh 渲染行为完全不变，纯格式迁移。

### 3.2 GigaVoxel VC/TFC（有纹理，需 UV）

VC 渲染范围内的总方块数有限：256×256×4096 = 256M < 2²⁴，因此 **24bit 足以编码 VC 范围内的任意 block**。payload 装下 LocalBlockIndex + 额外信息位 + UV。

```
y[23:0]   LocalBlockIndex   (24bit, VC 范围内的全局 block 坐标, 直接定位到具体 block)
y[31:24]  额外信息位         (8bit, 备用: subchunk 标记 / face index / 杂项 flag)
z         UV.x              (f32)
w         UV.y              (f32)
```

- **无需单独编码 chunk id**：LocalBlockIndex 是 VC 范围内的全局坐标，GigaVoxelRenderable 内部用它直接定位 block，不依赖 chunk 划分。
- **UV 直接存进 z/w**：decode 时省去"由 block 属性反查 UV"的查表步骤。
- **额外 8bit 备用**：当前未严格定义，可用于 face index（greedy mesh 合并的面方向）或 subchunk 级 flag。

**decode**：`RenderableIndex → GigaVoxelRenderable`，用 `LocalBlockIndex` 查它内部的 block/mesh 表 → 取 base color + material flags → 配合 UV 采样 block 纹理 → 写 G-buffer。

> VC/TFC 有三角形（greedy mesh / card），但 decode 不依赖三角形 bary 重建——LocalBlockIndex 已直接给出 block 归属，UV 已存。这是比 static mesh 更直接的 decode 路径。

### 3.3 GigaVoxel LFC（远场，无纹理追求，64bit 随机应变）

LFC 不追求从 visibility 完美恢复一切 block 属性。**颜色直接写进 payload**，decode 时 y → G_albedo，连 block lookup 都省了。

```
y         BlockColor        (32bit, 直接塞 packed color: RGBA8 或 R11G11B10 等)
z         (32bit, 备用: normal / AO / emissive 等轻量属性, 当前可未定义)
w         NaN sentinel      (f32, 指数位全 1 + 尾数非零 → 表示 NaN)
```

- **y 是颜色而非 block index**：远场不需要逐 block 材质查询，颜色在 SW raster 阶段就烘焙进 payload，decode 零计算。
- **z 备用**：当前 LFC 不需要法线等（远场光照简化），保留给未来按需扩展（如远场法线、shadow term）。
- **w 是 NaN sentinel**：见 §4.1 的 VC/LFC 区分机制。

**decode**：`RenderableIndex → GigaVoxelRenderable`，检测到 w 为 NaN → LFC 分支 → `G_albedo = unpack(Visibility.y)`，其余 G-buffer 通道按远场简化策略填充（法线取几何法线近似、emission=0 等）。

---

## 4. GigaVoxel 特有的设计点

### 4.1 VC/LFC 的区分：NaN sentinel

GigaVoxel 只占**一个** `RenderableType::kGigaVoxel` 枚举值（不拆分 VC/LFC），decode 时需进一步区分像素走 VC 分支还是 LFC 分支。两种机制：

1. **w 的 NaN sentinel（首选）**：IEEE 754 中，指数位全 1 且尾数非零即 NaN。LFC 的 w 不存有意义数据，写入 NaN 作为 sentinel；VC 的 w 是正常 UV float（非 NaN）。decode 第一步检查 `isnan(Visibility.w)`：
   - 是 NaN → LFC 分支
   - 非 NaN → VC/TFC 分支
   - 零额外位成本，零 header 查询。
2. **LocalBlockIndex 范围推断（备用）**：VC 的 LocalBlockIndex 有有效范围（24bit 内），LFC 的 y 是颜色（可能落在任意 32bit 值）。此机制不如 NaN 可靠，仅作辅助/调试。

**推荐 NaN sentinel 作为唯一机制**，范围推断不写入实现，避免歧义。

### 4.2 GigaVoxel 作为单 Renderable 槽

GigaVoxel 在 Scene 中注册为**一个** `GigaVoxelRenderable`（类似 `GigaVoxelInstance`），整个场景中最多几百个（一个巨大地形 + 几百个活跃可动 shell），占用**一个** RenderableIndex 槽。

**含义**：
- visibility 的 `RenderableIndex` 对 GigaVoxel 而言只指向"那个 GigaVoxelRenderable"，真正的 chunk/LOD 定位全在 payload（LocalBlockIndex / BlockColor）里。
- GigaVoxel 内部的 chunk 流送（mesh/BLAS/LFC block 的加载/卸载/LOD 转换）**不走 Scene 的 per-instance transform/header/hash 表**，由 GigaVoxelRenderable 自己的特化高性能流送管线管控。
- 这彻底规避了"数千 chunk 注册进 Scene renderable 表"导致的 dirty tracking / TLAS rebuild 风暴——Scene 只看到少数几个大型 GigaVoxelRenderable。

**对 Scene 表的影响**：RenderableHeader / Transform / Hash 槽位开销与 static mesh 同量级，无膨胀。TLAS instance 数也仅增加"每个 GigaVoxelRenderable 一个"（内部 BLAS 管理另议，见 PTLAS task）。

### 4.3 废弃类型说明

`RenderableType` 现有 `kGaussianRadianceFieldInstance` 和 `kVolumePrimitivesInstance` / `kVolumeGridInstance` 为**废弃内容**，保留枚举位但不再发展。新增 `kGigaVoxel` 作为未来主要的场景 renderable 类型，与 `kStaticMeshInstance` 并列。

---

## 5. Raster 管线顺序与 Depth 共享

### 5.1 执行顺序

单 visibility target 意味着所有 opaque 几何路径串行写同一个 `G_visibility_` + `G_depth_`。顺序：

```
Phase 3A: HW Raster (graphics pass)
  ├─ Static mesh opaque (deferred, 写 visibility + depth)
  └─ GigaVoxel VC/TFC opaque (greedy mesh / card, 写 visibility + depth)
       ↓ (RDG barrier: depth + visibility 转为可读)
Phase 3B: SW Raster LFC (compute pass)
  └─ GigaVoxel LFC (软件投影 LOD block cube)
       ↓
Phase 3C: HW Raster semi-transparent (graphics pass, 独立 color target, 不碰 visibility)
       ↓
Phase 4: DecodeVisibility (compute, 16x16 tiles) → G-buffer
```

**HW 先于 SW 的理由**：static mesh 等传统几何通常比 SW raster 的远场 LFC cube 更近。HW 先写 depth，SW 阶段读 HW 已写的 `G_depth_` 做 software depth test——被近场几何遮挡的 LFC 像素直接跳过，不覆盖 visibility。这比 SW 先写再被 HW 覆盖更省 SW 工作量。

> ⚠️ **更正说明**：早期 GigaVoxel culling 文档（`gigavoxel_culling_rasterization.md`）曾写"Phase 3B (LFC) 先于 Phase 3A (VC/TFC)"，那是写反了。正确顺序是 **HW (3A) 先，SW (3B) 后**。culling 文档需同步更正。

### 5.2 Depth 策略

- **共用 `G_depth_`**：HW 和 SW 写同一个 D32 reversed-z depth buffer。
- **HW raster**：硬件 depth test（`kGreater`，reversed-z），自然处理遮挡。
- **SW raster (LFC)**：compute shader 内做 software depth test——读 `G_depth_` 当前像素值，与投影出的 LOD block 深度比较，通过才写 visibility + depth。
- **barrier**：Phase 3A 结束 → 3B 开始前，`G_depth_` 需从 `kDepthAttachmentWrite` 转 `kShaderRead | kShaderWrite`（SW compute 读写）。Phase 3B 结束 → Phase 4 前，`G_visibility_` + `G_depth_` 转 `kShaderRead`。

### 5.3 Semi-Transparent 不碰 visibility

visibility target 是 **opaque-only**。semi-transparent 几何（水面、树叶、玻璃等）完全不走 visibility：
- VC/TFC semi-transparent：走独立 forward pass（Phase 3C），写入独立 transparent color target，做排序 + alpha blend。
- LFC：所有 semi-transparent 一律当 opaque 处理（蓝色 cube），走 visibility。

详见 `gigavoxel_transparent_materials.md`。decode pass 不感知 semi-transparent——它在 Phase 3C 之后由独立的 composite 步骤叠加到最终 G-buffer / color。

---

## 6. Decode Pass 行为

`DecodeVisibility`（compute, 16×16 tiles）按像素读 `G_visibility_` + `G_depth_`，按 `RenderableType` 分支：

```
DecodeVisibility(pixel):
  depth = G_depth_[pixel]
  if depth == 0: return                        // 空像素
  
  vis = G_visibility_[pixel]
  renderable_index = vis.x & 0xFFFFF           // 20bit
  type = (vis.x >> 20) & 0xFFF                 // 12bit
  
  switch (type):
    case kStaticMeshInstance:
      descriptor = vis.y & 0xFF
      primitive = vis.y >> 8
      bary = float3(1 - vis.z - vis.w, vis.z, vis.w)
      → EvaluateStaticMeshIntersection(renderable_index, descriptor, primitive, bary)
      → 写 G_albedo / G_normal / G_emission / G_metallic_roughness / G_motion_vector
    
    case kGigaVoxel:
      if isnan(vis.w):                          // LFC sentinel
        color = unpack_color(vis.y)
        → G_albedo = color
        → 法线取几何近似 / emission=0 / motion=0 (远场简化)
      else:                                     // VC/TFC
        local_block_index = vis.y & 0xFFFFFF
        extra = (vis.y >> 24) & 0xFF
        uv = float2(vis.z, vis.w)
        → EvaluateGigaVoxelVCBlock(renderable_index, local_block_index, uv)
        → 写 G-buffer (带纹理、法线等)
    
    default:
      // 废弃类型 / 未识别, 跳过
```

**性能要点**：
- LFC 分支极轻量（unpack 颜色 + 直写），无任何表查询。符合远场定位。
- VC 分支一次 `LocalBlockIndex → block 表` 查询，比 static mesh 的多层间接（Description → Geometry/Material pair）更直接。
- static mesh 分支与现状逻辑等价，仅解包路径变化。

---

## 7. 落地范围与改动清单

### 7.1 Static Mesh 格式迁移（地基，纯重构）

**目标**：把现有 8+24 打包挪到新格式，static mesh 渲染行为零变化，可独立验证。

改动文件：
- `mi/renderer/shaders/DrawStaticMeshes.hlsl`：VS 打包、PS 写入、Decode 解包三处。
- 无 C++ 侧格式变更（`kMaxNumGeometries` 不变，只是 DescriptorRank 换通道）。

**验证**：3d_viewer 跑现有 static mesh 场景，确认渲染结果与迁移前一致（G-buffer 截图对比）。

### 7.2 RenderableType 扩展

- `mi/renderer/include/renderer/mi_renderer_types.h`：新增 `kGigaVoxel` 枚举值。
- `mi/renderer/shaders/shared/SharedRenderable.hlsl`：`MI_RENDERABLE_TYPE_*` macro 映射（由 RDGShader 注入）需补 `GigaVoxel`。

### 7.3 GigaVoxelRenderable 接入

- 新增 `GigaVoxelRenderable`（继承 `Renderable`），占单 RenderableIndex 槽。
- VC/TFC 几何进 GigaVoxelRenderable 内部 bindless 表（不走 Scene 的 geometry/material 表）。
- VC/TFC HW raster pass 写 visibility（新 graphics pass 或复用 static mesh pass 框架）。
- LFC SW raster pass 写 visibility + software depth test（新 compute pass）。

### 7.4 依赖与前置

- **PTLAS（`task_ptlas_support.md`）**：GigaVoxelRenderable 内部 chunk BLAS 数量大（数千），传统 TLAS 的单 instance 更新瓶颈在此场景显著。PTLAS 的 partitioned update 是 GigaVoxel 光追路径的前置。**但 raster/visibility 路径不依赖 PTLAS**——可在 PTLAS 落地前先打通 raster 渲染。
- **Culling 管线**：LFC 与 VC/TFC 统一进同一 culling 管线（见 `gigavoxel_culling_rasterization.md`），不独立。

### 7.5 落地顺序建议

1. **Static Mesh 格式迁移**（§7.1）——纯重构，可独立验证，是地基。
2. **RenderableType 扩展**（§7.2）——加枚举，无行为变化。
3. **GigaVoxelRenderable 骨架**——空壳 renderable，注册进 Scene，确认 RenderableIndex 分配。
4. **VC/TFC raster + decode 接入**——复用 static mesh decode 框架，验证带纹理的近场渲染。
5. **LFC SW raster + decode 接入**——新 compute pass + NaN sentinel 机制，验证远场简化渲染。
6. **PTLAS 集成**（与 `task_ptlas_support.md` 协同）——打通光追路径。

---

## 8. 未定义 / 未来扩展

| 项 | 现状 | 说明 |
|----|------|------|
| LFC `z` (Payload1) | 未定义 | 备用：远场法线 / AO / emissive / shadow term，按需启用 |
| LFC `w` 除 NaN 外的位 | 仅用 NaN sentinel 标记 | 若未来需要，NaN 之外可编码少量数据（但需保证不破坏 sentinel 检测） |
| VC/TFC `y[31:24]` 额外 8bit | 未严格定义 | 备用：face index / subchunk flag |
| RenderableType 高 9bit | 仅用低 3bit | 富余，未来 renderable 类型扩展空间 |
| RenderableIndex 容量监控 | 20bit = 1M | 当前场景（static mesh + 少量 GigaVoxelRenderable）远未触顶，无需监控 |

---

## 9. 决策记录

| 决策 | 结论 | 理由 |
|------|------|------|
| 单 visibility target vs 多 target | 单 target | 下游光照统一，pass 数少 |
| RenderableIndex 位数 | 20bit | 与 ray tracing 对齐；GigaVoxel 单槽使容量充足 |
| RenderableType 进 visibility | 是（12bit） | decode 第一时间分支，省 header 查表 |
| Payload 形状是否跨类型统一 | 否 | 各类型按最优编码，LFC 直存颜色最务实 |
| VC/LFC 区分机制 | NaN sentinel (w) | 零位成本，零 header 查询 |
| GigaVoxel 注册粒度 | 单 Renderable 槽 | 避免数千 chunk 冲击 Scene 表 |
| Raster 顺序 | HW 先，SW 后 | 近场几何先写 depth, SW 用其剔除远场 |
| Semi-transparent 与 visibility | 分离 | visibility opaque-only，semi-transparent 走独立 forward pass |

---

## 附录 A：与现有代码的对照

| 现有位置 | 现状 | 新方案 |
|----------|------|--------|
| `mi/renderer/include/renderer/mi_static_mesh.h:149-150` | `kMaxNumGeometries = 256`, 注释提"visibility buffer reserved 8 bits" | 8bit DescriptorIndex 保留，但从 x 挪到 y[7:0] |
| `mi/renderer/shaders/DrawStaticMeshes.hlsl:50` | `DescriptorIndex << 24 \| RenderableIndex` | `x = RenderableIndex(20) \| Type(12)<<20`; `y = DescriptorIndex(8) \| PrimitiveIndex(24)<<8` |
| `mi/renderer/shaders/DrawStaticMeshes.hlsl:131-132` | `RenderableIndex = Visibility.x & 0xFFFFFF; DescriptorRank = (Visibility.x>>24)&0xFF` | `RenderableIndex = Visibility.x & 0xFFFFF; Type = (Visibility.x>>20)&0xFFF; Descriptor = Visibility.y & 0xFF; Primitive = Visibility.y >> 8` |
| `mi/renderer/include/renderer/mi_renderable.h:81` | `kRenderableIndexNumBits = 20` (ray tracing) | visibility 对齐此值 |
| `mi/renderer/shaders/shared/SharedRenderable.hlsl:50` | `RENDERABLE_INDEX_NUM_BITS 20` (ray tracing) | 共享定义 |
