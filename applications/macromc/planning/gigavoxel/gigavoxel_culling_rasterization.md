# GigaVoxel Culling + Rasterization 管线

## 1. 管线总览

```
Frame N
═══════════════════════════════════════════════════════════════

Phase 0 (CPU): LOD 选择 + Chunk 状态解析
  输入: 相机参数, ChunkRegistry
  输出: visible_chunk_list (target_lod + resolved_lod)

Phase 1 (CPU): 数据准备 + GPU Upload
  输入: visible_chunk_list
  输出: GPU chunk metadata buffer

Phase 2 (GPU Compute): 分层 Culling
  Chunk 级 frustum → SubChunk 级 frustum → Hi-Z occlusion
  输出: indirect draw args (VC/TFC) + 可见 LOD Block list (LFC)

Phase 3 (GPU): 混合光栅 → Visibility Buffer (共享 G_depth_)
  3A: HW Raster (VC+TFC, graphics) — 先写近景, 硬件 depth test
  3B: SW Raster (LFC, compute) — 后写远景, 读 HW depth 做 software depth test

Phase 4 (GPU Compute): Visibility Resolve → G-Buffer

═══════════════════════════════════════════════════════════════
```

---

## 2. Phase 0: CPU — LOD 选择 + Chunk 状态解析

### 2.1 自顶向下 LOD Refinement

核心思路：**从最粗糙的 LOD 开始，逐级细分到目标精度**。

1. 将世界中所有活跃的 LFC4 super-chunk 作为起点，对每个做 frustum-AABB 测试
2. 通过粗筛的 LFC4 开始递归细分：
   - 如果该 LFC4 的子 LFC3 仍然足够远（> 2km），保持 LFC3 级别，不再细分
   - 否则继续拆分到 LFC2 → LFC1 → 直到单 chunk 级别
3. 到达单 chunk 级别时，根据距离判定为 VC（< 128m）或 TFC（128~512m）

**距离阈值**：

| 转换 | 阈值 |
|------|------|
| LFC4 → LFC3 | < 8km |
| LFC3 → LFC2 | < 2km |
| LFC2 → LFC1 | < 1km |
| LFC1 → TFC/VC | < 512m |
| TFC → VC | < 128m |

> 这些阈值后续可以用屏幕投影面积替代，实现 view-dependent LOD。

### 2.2 Chunk 状态解析 + Fallback

对每个被选中的 chunk，查询 ChunkRegistry 确认目标 LOD 数据是否就绪：

1. **已就绪**：直接使用目标 LOD 渲染
2. **未就绪**：向 coarser 方向查找最接近且已就绪的 LOD 作为 fallback
   - 例如目标 VC 未就绪 → fallback 到 TFC → fallback 到 LFC0 → fallback 到 LFC1...
   - **绝不向 finer 方向 fallback**
3. **完全无数据**：跳过该 chunk（不渲染）
4. 同时向 StreamingManager 异步请求目标 LOD 数据（未来帧就绪）

**Fallback 的视觉效果**：远处的 chunk 显示为较粗糙的色块，随着数据逐步加载，会“升级”到更高精度。这个过程是渐进的，不会突然出现。

**LFC Fallback 裁剪**：

当 LFC(N) chunk 未就绪，fallback 到 coarser 的 LFC(N+1) chunk 时，LFC(N+1) 的 mesh 覆盖了 4 个 LFC(N) 的面积。但不能直接渲染整个 LFC(N+1)，因为：
- LFC(N+1) 的 LOD block 更大，表面高度可能与 LFC(N) 不一致
- Coarse block 可能“凸出”到 fine block 上方，depth test 无法保证正确遮挡
- 会干扰同一 LFC(N+1) 内其他已就绪的 LFC(N) 兄弟 chunk

**解决方案**：fallback 渲染时，只渲染 LFC(N+1) 中属于该 fallback chunk 区域内的 LOD blocks。
- 传入 fallback chunk 的 world-space bounding box
- 在 LFC Block Cull（Dispatch 3）中，只保留 LOD block center 落在 fallback bbox 内的 blocks
- 横跨边界的 LOD block 按 center 位置判定归属（保守策略：横跨边界的 block 始终渲染）

---

## 3. Phase 1: CPU — 数据准备 + GPU Upload

将 Phase 0 产出的 visible_chunk_list 整理为 GPU 可用的格式：

**Chunk Instance Buffer**（每 chunk 一条记录）：
- chunk_id、resolved_lod、target_lod、flags
- world transform（用于 vertex shader）
- mesh buffer 内的 offset 和 vertex count
- 资源引用 offset（chunk card buffer offset / color array offset）
- chunk AABB（world space，用于 GPU 侧 culling）

**SubChunk Metadata Buffer**（仅 VC/TFC 需要）：
- 每个非空 subchunk 的 AABB、mesh offset、vertex count、is_empty flag
- 稀疏存储：空 subchunk 不上传

**Indirect Draw Args Buffer**（Phase 2 的输出目标）：
- 预分配固定大小的 buffer
- Phase 2 的 compute shader 会往里写 draw commands + 更新 draw count

Upload 方式：staging buffer → device buffer 的 async copy，与上帧 GPU 工作并行。

---

## 4. Phase 2: GPU Compute — 分层 Culling

### 4.1 三层 Culling 级联

**Dispatch 1 — Chunk 级 Frustum Cull**：
- 每个 thread 处理一个 visible chunk
- 将 chunk AABB 与 6 个 frustum plane 做 intersect test
- 通过的 chunk 在 visible_mask bit array 中标记为 true
- 用 atomic counter 统计 visible chunk 数量

**Dispatch 2 — SubChunk 级 Cull**（仅 VC/TFC）：
- 每个 thread 处理一个 (chunk, subchunk) pair
- 只处理 Dispatch 1 标记为 visible 的 chunks
- SubChunk AABB vs frustum test
- SubChunk AABB vs Hi-Z buffer test（可选，用 conservative depth 做遮挡剔除）
- 通过的 subchunk 写入对应的 indirect draw args

**Dispatch 3 — LFC Block Cull**：
- 每个 thread 处理一个 LFC chunk
- 对该 chunk 的可见 LOD Block（已在 CPU 上烘焙好的外层 block）做 frustum + Hi-Z test
- **Fallback 裁剪**：如果该 LFC chunk 是作为更细级别 chunk 的 fallback 被渲染的，额外检查每个 LOD block 的 center 是否落在 fallback region 的 bbox 内。不在内的 blocks 直接跳过。
- 通过的 LOD Blocks 写入“可见 LOD Block Buffer”（供 Phase 3B 使用）
- 每条记录包含：block 的 world-space 位置/尺寸、packed color、chunk_id、block_id、面朝向信息

### 4.2 Hi-Z Occlusion Culling

Hi-Z buffer 来自**上一帧** depth buffer 的 conservative mip chain（每级取 MAX depth = 最近深度）：

- 将 subchunk / triangle 的 AABB 投影到 screen space 得到 2D rect
- 根据 rect 大小选择合适的 Hi-Z mip level
- 如果目标的最近深度 > Hi-Z 采样值 → 被遮挡，剔除

注意事项：
- 使用上帧数据，快速运动时可能有 1 帧 artifact
- 只用于剔除（保守测试），不会误保留不该渲染的东西
- 可在相机速度过高时临时禁用

### 4.3 Culling 输出

Phase 2 产出以下 GPU buffer，供 Phase 3 消费：

- `vc_indirect_args[]` + `vc_draw_count`：VC 的 indirect draw commands
- `tfc_indirect_args[]` + `tfc_draw_count`：TFC 的 indirect draw commands
- `lfc_visible_blocks[]` + `lfc_block_count`：cull 后的可见 LOD Block 列表（每条含 position/size、color、chunk_id、block_id）

---

## 5. Phase 3: 混合光栅 → Visibility Buffer

### 5.1 Visibility Buffer 格式

Visibility buffer 是 **renderer 层统一格式**，由 static mesh 和 GigaVoxel 共用。
完整规范见 **`gigavoxel_visibility_buffer.md`**，此处仅列 GigaVoxel 相关摘要。

**格式**: `R32G32B32A32_UINT` (128bit/pixel)

```
x[19:0]  RenderableIndex (20bit)  — 指向 GigaVoxelRenderable (单槽)
x[31:20] RenderableType  (12bit)  — kGigaVoxel
y, z, w  Payload (96bit)          — 按 VC/LFC 自由编码
```

**GigaVoxel VC/TFC 编码**（有纹理）：
| 字段 | 位数 | 含义 |
|------|------|------|
| LocalBlockIndex (y[23:0]) | 24 | VC 范围内全局 block 坐标, 直接定位 block |
| 额外 (y[31:24]) | 8 | 备用: face index / subchunk flag |
| UV.x (z) | 32 (f32) | block 纹理坐标 |
| UV.y (w) | 32 (f32) | block 纹理坐标 |

**GigaVoxel LFC 编码**（远场, 无纹理追求）：
| 字段 | 位数 | 含义 |
|------|------|------|
| BlockColor (y) | 32 | 直接塞 packed color, decode 零计算 |
| 备用 (z) | 32 | 未定义, 留作远场法线/AO 等 |
| NaN sentinel (w) | 32 | 指数位全 1 → NaN, 标记此像素为 LFC |

> **VC/LFC 区分**：decode 时检查 `w` 是否为 NaN——是 NaN 走 LFC 分支（y 直解颜色），非 NaN 走 VC 分支（y 解 LocalBlockIndex + z/w 解 UV）。零 header 查询。

**配套 Depth Buffer**: `D32_FLOAT` reversed-z，HW 与 SW 共用 `G_depth_`。
判空统一用 `Depth == 0`。

### 5.2 Phase 3A: HW Raster (VC + TFC)

使用标准 Vulkan graphics pipeline，通过 indirect draw 渲染：

**VC Pipeline**：
- Vertex Shader 从 mesh buffer 读取顶点（position, uv_base, uv_scale, normal）
- Fragment Shader 写入 Visibility Buffer: RenderableIndex + RenderableType=kGigaVoxel + LocalBlockIndex + UV
- 绑定 block texture atlas (SRV) 用于 alpha test（如果有的话）

**TFC Pipeline**：
- Vertex Shader 与 VC 类似
- Fragment Shader 同样写入 visibility (LocalBlockIndex + UV)
- TFC 不需要 atlas 采样（材质由 chunk card 表达，但 card lookup 推迟到 Phase 4 resolve）
- 可选绑定 chunk card buffer (SSBO)

**Draw 方式**：`vkCmdDrawIndexedIndirectCount`，draw count 来自 Phase 2 的 atomic counter。

### 5.3 Phase 3B: SW Raster (LFC, Compute Shader)

所有可见 LFC 的 culled LOD Blocks 在单个 compute dispatch 中批量光栅化。

**输入**：可见 LOD Block Buffer（不是 triangle，而是 cube/cuboid 列表）。

**每个 thread 处理一个 LOD Block**：
1. 根据 block 的 world-space 位置和尺寸，确定其 6 个面
2. 根据相机方向判断哪些面朝向相机（通常 1~3 个面可见）
3. 将每个可见面投影到 screen space，得到一个 screen-space quad
4. 遍历 quad 内的像素，读取 HW 已写的 G_depth_ 做 software depth test
5. 通过后写入 visibility (BlockColor 直存 y) + 更新 depth，w 写 NaN sentinel 标记 LFC

**LFC Visible Block** 每条记录包含：
- world-space 位置 + 尺寸（3 个分量，支持非正方体 LOD block）
- packed RGBA color（直接写进 visibility.y，不存 block_id）

> 注意：LOD block 的面朝向检测可以在 shader 内完成（比较面法线与相机方向），
> 无需在 CPU 侧预计算可见面。每个 block 最多光栅化 3 个面。

### 5.4 执行顺序：先 HW 后 SW

**选择先 Phase 3A (HW/VC+TFC) 后 Phase 3B (SW/LFC)**：

1. VC/TFC 和 static mesh 通常在近景，HW raster 先写 visibility + G_depth_
2. LFC cube 在远景，SW raster 读 HW 已写的 G_depth_ 做 software depth test
3. 被近场几何遮挡的 LFC 像素直接跳过，不覆盖 visibility——省 SW 工作量
4. 优势：SW 只需普通 read-modify-write depth，无需 atomic 竞争（HW 已串行在前）

> ⚠️ 更正：早期版本曾写"先 SW 后 HW"，那是写反了。正确顺序是 **HW 先，SW 后**。
> 详见 `gigavoxel_visibility_buffer.md` §5。

Phase 3A 与 Phase 3B 之间需要 RDG barrier：`G_depth_` 从 depth-attachment-write 转 shader-read|shader-write。

---

## 6. Phase 4: Visibility Resolve → G-Buffer

一个 full-screen compute dispatch，将 Visibility Buffer 转换为 G-Buffer。
decode 按 `RenderableType` 分支，GigaVoxel 内部再按 NaN sentinel 区分 VC/LFC。
完整 decode 逻辑见 `gigavoxel_visibility_buffer.md` §6。

**每个像素的处理**：
1. 读 G_depth_，若 `Depth == 0`（reversed-z, sky/无数据）→ 写 sky 默认值，跳过
2. 读 G_visibility_，解包 RenderableIndex(20) + RenderableType(12)
3. 按 RenderableType 分支：
   - **kStaticMeshInstance**：现有 static mesh decode 路径（DescriptorIndex + PrimitiveIndex + Bary）
   - **kGigaVoxel**：
     - 检查 `w` 是否 NaN
     - **非 NaN → VC/TFC**：y 解 LocalBlockIndex → 查 GigaVoxelRenderable block 表 + UV 采样 → 完整材质
     - **是 NaN → LFC**：y 直解 packed color → G_albedo，法线取几何近似，emission=0
4. 写入 G-Buffer（albedo、normal、material_id 等）

**G-Buffer 输出**（与现有 renderer 兼容）：
- Albedo (RGBA8)
- Normal (RG16, octahedral encoded)
- Material ID / flags
- Motion Vector (RG16F) [未来帧]

---

## 7. 管线时序与依赖

```
Frame N-2:                    Frame N-1:                    Frame N:
┌────────────────┐           ┌────────────────┐           ┌────────────────┐
│Streaming Jobs  │           │Streaming Jobs  │           │Streaming Jobs  │
│(decompress,    │──────────→│(results from   │──────────→│(new requests   │
│ mesh baking,   │           │ N-2 ready,     │           │ from Phase 0)  │
│ BLAS build)    │           │ new requests)  │           │                │
└────────────────┘           └────────────────┘           └────────────────┘
                                                                       │
Frame N CPU:                                                           │
┌──────────────────────────────────────────────────────┐               │
│ Phase 0: LOD Select + Chunk State (CPU)              │←──────────────┘
│ Phase 1: Upload metadata (CPU → GPU staging)         │
└──────────────────────────────────────────────────────┘
                              │
Frame N GPU:                  ▼
┌──────────────────────────────────────────────────────┐
│ Phase 2: GPU Culling (compute dispatches)            │
│ Phase 3B: SW Raster LFC (compute)                    │
│ Phase 3A: HW Raster VC+TFC (graphics pipeline)       │
│ Phase 4: Visibility Resolve (compute)                │
│ ... RTGI / DLSS / Tone Map (existing passes) ...     │
└──────────────────────────────────────────────────────┘
```

**关键时序**：
- Phase 0/1 在 CPU 上执行，与上帧 GPU 工作并行
- Streaming jobs（decompress、mesh baking、BLAS build）在 worker threads 上持续运行，跨帧完成
- 新 request 的 chunk 数据最早在 Frame N+1 或 N+2 可用，期间用 fallback LOD 填充

---

## 8. 性能估算

| 阶段 | 工作负载 | 预估耗时 |
|------|---------|---------|
| Phase 0 (CPU) | ~5000 LFC4 粗筛 + ~2000 细分 | < 1ms |
| Phase 1 (Upload) | ~50KB metadata | < 0.1ms |
| Phase 2 (GPU) | ~5000 chunk cull + ~20K subchunk cull | ~0.5ms |
| Phase 3B (SW Raster) | ~200K visible LOD blocks | ~1ms |
| Phase 3A (HW Raster) | ~200 VC + ~3000 TFC indirect draws | ~2ms |
| Phase 4 (Resolve) | 1080p = 2M pixels | ~0.5ms |
| **Total GPU** | | **~4ms** |

---

## 9. 待决问题

### Q1: HW Raster 的 Draw Call 组织

VC 有完整材质（不同 block type 用不同 atlas tile）。两种组织方式：

- **方案 A**: 单个 mega-pipeline，所有 block types 通过 atlas index 在 shader 内区分。简单，一次 indirect draw。
- **方案 B**: 按材质/材质组 分多个 pipeline。复杂，但支持不同 shader 逻辑（如透明 vs 不透明）。

推荐方案 A（atlas index 区分），除非有明确的材质 shader 差异需求。

### Q2: SW Raster 的 Thread 负载均衡

LOD Blocks 的 screen-space 面积差异大（远处可能几个像素，近处可能几十像素）。
简单的“1 thread = 1 LOD Block”方案会导致 thread divergence。

可选优化方向：
- 先按 screen area 分桶（小 block 一组，大 block 一组），分别 dispatch
- 或者使用 tile-based 方案：先 bin blocks to screen tiles，再 per-tile 处理

初期可以先用简单方案，后续根据 profiling 优化。

### Q3: Visibility Buffer 的 Clear 策略

每帧开始前需要 clear visibility buffer：
- **标准 clear**：vkCmdClearAttachments，简单直接
- **Frame ID 方案**：不 clear，在 visibility buffer 中嵌入 frame_id，resolve 时判断是否过期。省一次 full-screen write，但需要额外的 bits。

### Q4: Hi-Z 精度与时序

Hi-Z 使用上帧 depth 的 conservative mip chain。快速相机运动时可能不准确。

缓解策略：
- 降低 Hi-Z 测试的保守程度（只剔除明确被遮挡的，留余量）
- 相机速度超过阈值时临时禁用 Hi-Z culling
- 未来可结合 temporal reprojection 提升 Hi-Z 精度
