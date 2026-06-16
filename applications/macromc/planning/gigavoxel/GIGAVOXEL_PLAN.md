# GigaVoxel 总体设计 Plan

> 本文档是 GigaVoxel 体素渲染系统的顶层计划。详细设计分散在 `planning/` 下的专题文档中，`tasks/` 下维护具体实现任务。

---

## 1. 项目目标

在 MIRenderer 上实现类 Minecraft 的 textured 体素渲染，支持：
- **64km 视距**
- **4096m 世界高度**
- **完整 RTGI（Real-Time Global Illumination）**
- **目标硬件**：NVIDIA RTX，64GB RAM + 24GB VRAM

---

## 2. 系统架构总览

```
┌─────────────────────────────────────────────────────────────────┐
│                        GigaVoxel System                         │
├───────────────┬──────────────────┬──────────────────────────────┤
│   Streaming   │   Culling +      │   Lighting (RTGI)           │
│   Pipeline    │   Rasterization  │   (未来设计)                 │
│               │                  │                              │
│ Disk→RAM→VRAM │ LOD Refinement   │ Primary/Secondary Ray        │
│ Mesh Baking   │ GPU Culling      │ PTLAS Query                  │
│ BLAS Building │ Hybrid Raster    │ GI / Reflection / Shadow     │
│ Job Scheduling│ Visibility Buffer│                              │
└───────────────┴──────────────────┴──────────────────────────────┘
```

三大子系统：
1. **Streaming Pipeline** — 数据生命周期管理（Disk ↔ RAM ↔ VRAM）
2. **Culling + Rasterization** — 可见性确定与 Visibility Buffer 生成
3. **Lighting (RTGI)** — 基于 PTLAS 的光线追踪全局光照（后续设计）

---

## 3. LOD 层级体系

> 详见 `planning/gigavoxel_lod_definition.md`

### 3.1 三级六档

| LOD | 距离 | 体素精度 | 几何 | 材质 | 光栅 |
|-----|------|---------|------|------|------|
| **VC** | 0~128m | 1:1 | Greedy Mesh + UV tiling | Block texture atlas | HW raster |
| **TFC** | 128~512m | 1:1 | Baked mesh | Chunk card (SSBO) | HW raster |
| **LFC0** | 512m+ (fallback) | 1:1 | LOD cubes | Single color | SW raster |
| **LFC1** | 512m~1km | ×2 | LOD cubes (2m) | Single color | SW raster |
| **LFC2** | 1~2km | ×4 | LOD cubes (4m) | Single color | SW raster |
| **LFC3** | 2~8km | ×8h ×4v | LOD cuboids (8×4×8m) | Single color | SW raster |
| **LFC4** | 8~64km | ×16h ×8v | LOD cuboids (16×8×16m) | Single color | SW raster |

### 3.2 关键决策

- **非对称 LOD 合并**：LFC3/LFC4 高度倍率低于水平倍率，保留地形起伏
- **Greedy Meshing + UV Tiling**：VC 使用 greedy meshing，UV 通过 `frac()` 重复 atlas tile
- **Chunk Card (SSBO)**：TFC 纹理用 CPU 烘焙的小纹理卡（top/bottom 16×16, sides 16×32），存储在 SSBO 中
- **LFC 输入为可见 LOD Block 列表**：非 triangle，SW raster 直接投影 cube face

### 3.3 Chunk 与 SubChunk

- **Chunk**：16 × 4096 × 16 blocks，磁盘 I/O 和压缩的基本单位
- **SubChunk**：16 × 16 × 16 blocks（MC 经典），内存组织和 culling 的基本单位
- **Chunk 级 Palette 压缩**：所有 subchunk 共享 chunk palette，压缩后 ~50~200KB/chunk

### 3.4 邻居依赖（所有级别统一）

所有 LOD 级别的 mesh 烘焙都需要邻居数据做边界面剔除。仅在烘焙时依赖，区块内容未变化则无需更新。邻居未就绪时保守保留边界，异步修正。

---

## 4. 半透明材质处理

> 详见 `planning/gigavoxel_transparent_materials.md`

### 4.1 三个分离

- **Mesh 分离**：opaque mesh 和 semi-transparent mesh 独立维护（互相视为空气）
- **BLAS 分离**：VC chunk 有 opaque BLAS（kOpaque flag 优化 tracing）+ semi-transparent BLAS
- **渲染分离**：opaque 走 visibility buffer 路径，semi-transparent 走 sorting + forward 路径

### 4.2 LFC 简化

所有 LFC 级别的 semi-transparent blocks 一律当 opaque 渲染。水面反射通过特殊高反射材质在 RTGI 阶段近似。

### 4.3 排序策略

VC/TFC semi-transparent：per-subchunk 排序 + subchunk 内按面朝向分组。不做 per-pixel 排序。

---

## 5. Culling + Rasterization 管线

> 详见 `planning/gigavoxel_culling_rasterization.md`

### 5.1 五 Phase 管线

```
Phase 0 (CPU): 自顶向下 LOD Refinement（LFC4 → 细分 → VC/TFC）+ Chunk 状态解析 + Fallback
Phase 1 (CPU): 数据准备 + GPU Upload（chunk metadata, subchunk metadata, indirect args buffer）
Phase 2 (GPU Compute): 分层 Culling（chunk frustum → subchunk frustum → Hi-Z occlusion）
Phase 3 (GPU): 混合光栅 (共享 G_depth_, HW 先 SW 后)
  3A: HW Raster VC+TFC (opaque) — graphics
  3B: SW Raster LFC (opaque, 含 semi-transparent 当 opaque) — compute
  3C: HW Raster VC+TFC (semi-transparent, sorted forward) — graphics
Phase 4 (GPU Compute): Visibility Resolve + Composite → G-Buffer
```

### 5.2 关键决策

- **执行顺序**：先 HW 后 SW（VC/TFC/static mesh 近景先写 depth，LFC 远景读 HW depth 做剔除）
- **Visibility Buffer**：R32G32B32A32_UINT 统一格式（RenderableIndex 20bit + RenderableType 12bit + 96bit payload），详见 `gigavoxel_visibility_buffer.md`
- **Fallback 策略**：只向 coarser 方向 fallback，绝不向 finer
- **Hi-Z**：使用上帧 depth 的 conservative mip chain，相机快速运动时可禁用

---

## 6. TLAS 管理

> 详见 `tasks/task_ptlas_support.md`

### 6.1 方案：NVIDIA PTLAS

使用 `VK_NV_partitioned_acceleration_structure` 扩展实现 Partitioned TLAS。

- TLAS 分成多个 partition，每个 partition 可独立 update
- 只有发生变化的 partition 需要 rebuild
- 支持万级 instance 的高频局部更新

### 6.2 BLAS 组织

- **VC chunk**：2 个 BLAS（opaque + semi-transparent）
- **TFC chunk**：1 个 BLAS（opaque only，不含 semi-transparent）
- **LFC chunk**：1 个 BLAS（所有 LOD block 当 opaque）
- TLAS instance = per-chunk（VC 占 2 个 instance，其他 1 个）

### 6.3 实现层级

- **RHI 层**：新增 PTLAS 类型和 build API
- **RDG 层**：PTLAS 资源类型和 pass 集成
- **Renderer 层**：GigaVoxelTLASManager helper

---

## 7. Streaming Pipeline（数据流）

> 详见 `planning/gigavoxel_streaming_and_storage.md`

### 7.1 数据生命周期

```
Disk:     压缩体素数据 → 永久存储
RAM:      解压体素数据 → 与 chunk 可见性绑定（作为缓存保留）
          Mesh staging data → upload 完即释放
VRAM:     Mesh + BLAS + Card + Colors → 活跃期 + grace period（60~120 帧）
```

### 7.2 Budget（24GB VRAM / 64GB RAM）

```
VRAM (~1.1GB / 24GB):
  VC:       ~66MB
  TFC:      ~480MB
  LFC:      ~18MB
  Buffers:  ~500MB (pool overhead)
  → 非常宽裕

RAM (~115MB+ / 64GB):
  体素缓存: ~72MB（3000 chunks 的非空 subchunks）
  Registry: ~5MB
  Staging:  ~20MB
  → 非常宽裕，体素数据可作为缓存常驻
```

### 7.3 流通路径

```
Disk ──load──→ RAM (compressed) ──decompress──→ RAM (voxel cache)
                                                      │
                                                      ├──mesh bake──→ RAM (mesh staging)
                                                      │                      │
                                                      │                      └──upload──→ VRAM
                                                      │
                                                      └──LFC merge──→ RAM (LOD data staging)
                                                                             │
                                                                             └──upload──→ VRAM
```

### 7.4 关键决策

- **体素数据作为 RAM 缓存**：64GB RAM 足够缓存所有活跃 chunk 的解压体素数据
- **邻居依赖不等**：mesh baking 不阻塞等待邻居，先出保守 mesh，邻居就绪后异步修正
- **Eviction grace period**：60~120 帧（VRAM 宽裕，可以大方一些）

---

## 8. 已识别但未深入设计的话题

以下话题已识别但留待后续讨论：

| 话题 | 状态 | 说明 |
|------|------|------|
| **Streaming Pipeline 详细设计** | 核心维度已定，Job DAG 待设计 | 优先级系统、job 调度、瓶颈分析 |
| **GPU Buffer 生命周期管理** | 未设计 | sub-allocator 策略、碎片管理、LOD 转换时的双份数据 |
| **LOD 转换视觉连续性** | 未设计 | crossfade / dither、history 处理 |
| **RTGI 与 LOD 交互** | 高层决策已定 | 射线 hit LFC 当 opaque 处理，VC 完整折射/反射 |
| **Floating Origin** | 未设计 | 64km 精度处理，camera-relative transform |
| **坐标系统** | 未设计 | world ↔ chunk ↔ block ↔ LOD block 转换 |
| **DLSS/TAA 时序整合** | 未设计 | LOD 切换时的 history invalidation |
| **Motion Vector** | 未设计 | 体素场景的 MV 生成 |
| **Chunk Card 流送** | 未设计 | SSBO slot 分配、淘汰 |
| **Worldgen / Gameplay** | 远期 | 世界生成、方块交互、多人 |

---

## 9. 文档索引

### Planning 文档（`planning/`）

| 文档 | 内容 |
|------|------|
| `gigavoxel_lod_definition.md` | LOD 层级定义、SubChunk 设计、对比表、待决问题 |
| `gigavoxel_visibility_buffer.md` | 统一 Visibility Buffer 格式（renderer 层, static mesh + GigaVoxel 共用）、位分配、各类型编码、raster 顺序 |
| `gigavoxel_culling_rasterization.md` | 5-Phase 管线设计、culling 算法、性能估算（visibility 格式引用 visibility_buffer 文档） |
| `gigavoxel_transparent_materials.md` | 半透明材质的三个分离、BLAS 策略、LFC 简化 |
| `gigavoxel_streaming_and_storage.md` | 数据生命周期、LFC 全级别持久化、流通路径、Budget |

### Task 文档（`tasks/`）

| 文档 | 内容 |
|------|------|
| `task_ptlas_support.md` | PTLAS (VK_NV_partitioned_acceleration_structure) RHI/RDG/Renderer 实现 |
| `task_job_cancel.md` | TaskGraph Task 取消支持 |
| `task_job_priority.md` | TaskGraph 动态优先级 + 连续优先级值 |
| `task_compression_layer.md` | 压缩/解压抽象层（ICompressor + Registry + Codec） |

---

## 10. 实现路线建议

```
Phase A — 基础设施（无体素特定逻辑）
  ├─ PTLAS RHI/RDG 支持 (task_ptlas_support)
  ├─ GPU Buffer sub-allocator（大规模 chunk buffer 管理）
  └─ Streaming Pipeline 骨架（ChunkRegistry, Job DAG, FIO 集成）

Phase B — 最小可运行（单 LOD 验证）
  ├─ Chunk 数据加载 + 解压 (VC only)
  ├─ Greedy Meshing + BLAS build
  ├─ 简化版 culling（frustum only，无 Hi-Z）
  ├─ HW raster → Visibility Buffer → G-Buffer
  └─ 基础 RTGI（primary ray only）

Phase C — 多级 LOD
  ├─ TFC mesh baking + chunk card
  ├─ LFC merge + SW raster
  ├─ LOD refinement + fallback
  ├─ Hi-Z occlusion culling
  └─ PTLAS partition 策略

Phase D — 完善
  ├─ Semi-transparent 渲染（水面、树叶）
  ├─ LOD 转换平滑
  ├─ Streaming 优化（优先级、disk cache）
  └─ 完整 RTGI（GI bounce、reflection、shadow）
```
