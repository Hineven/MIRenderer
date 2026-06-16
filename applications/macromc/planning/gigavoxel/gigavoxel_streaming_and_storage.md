# GigaVoxel Streaming & Storage 设计

## 1. 设计目标

GigaVoxel 的数据在三个区域流转：**Disk → RAM → VRAM**。本文档定义每类数据的生命周期、流通规则和预算约束。

目标硬件：64GB RAM + 24GB VRAM。

---

## 2. 数据分类

GigaVoxel 涉及以下数据类型：

| 数据类型 | 来源 | 大小/chunk | 说明 |
|----------|------|-----------|------|
| 压缩体素数据 | Worldgen / Disk | ~50~200KB | chunk 级 palette + 熵编码 |
| 解压体素数据 | 解压 | ~2~4MB (非空 subchunks) | SubChunk palette indices |
| Opaque Mesh | Greedy meshing / Bake | ~50~100KB (VC), ~50KB (TFC) | vertex + index buffer |
| Semi-transparent Mesh | Bake | ~5~10KB | 仅 VC |
| Chunk Card | CPU bake | ~10KB | 仅 TFC |
| LFC Mesh + Colors | Merge + bake | ~4~100KB | 取决于 LOD level |
| LFC Color Hash Table | 从 LFC color 构建 | ~4B/entry | world-space hash, RTGI 查表用 |
| Opaque BLAS | GPU build | ~100~200KB (VC), ~100KB (TFC) | GPU acceleration structure |
| Semi-transparent BLAS | GPU build | ~20KB | 仅 VC |
| Chunk Metadata | Registry | ~100B | state, AABB, LOD info |

---

## 3. Lifespan 规则

### 3.1 Disk 层

```
压缩体素数据:
  - 永久存储（直到区块被修改或世界删除）
  - 以 chunk 为单位的压缩文件

LFC 渲染数据（全量持久化）:
  - LFC0 ~ LFC4: 全量持久化（覆盖整个 64km 视距）
  - 决策理由：见 §4.2。所有 LFC 级别都持久化，避免运行时重新生成
  - 估算：全世界全量 ~32GB（见 §4.1），已探索区域按比例更小（见 §6.3）

Mesh/Card disk cache（可选优化）:
  - 已烘焙的 mesh 和 card 可缓存到 disk
  - 二次加载时跳过 meshing + BLAS build
  - chunk 修改后 cache invalidation
```

### 3.2 RAM 层

```
解压体素数据（分区策略）:
  VC 范围（~200 chunks）:
    - 常驻 RAM，不释放
    - 供 gameplay 系统直接访问（方块放置/破坏、物理、实体交互等）
    - ~200 chunks × 非空 subchunks ~12 × 2KB ≈ 5MB（几乎零开销）

  TFC/LFC 范围:
    - 作为渲染缓存保留（与 chunk 可见性绑定）
    - 离开活跃集后延迟释放（类似 VRAM grace period）
    - 好处：邻居 mesh baking 可直接引用，LOD 转换可快速 rebake
    - ~3000 chunks × 非空 subchunks ~12 × 2KB ≈ 72MB

Mesh/Card staging data:
  - 极短暂（upload 到 VRAM 后立即释放）

ChunkRegistry metadata:
  - 与 chunk 存在性绑定（世界生命周期内持久）

LFC 数据（从 disk 加载）:
  - 所有级别（LFC0~LFC4）均从 disk 直接加载，不需要 merge
  - 在 RAM 中暂存，upload 到 VRAM 后可释放
  - Merge 仅在区块修改后触发：fine level 更新 → 级联更新 coarse level
    (LFC0 变化 → 重新 merge LFC1 → 重新 merge LFC2 → ...)
```

### 3.3 VRAM 层

```
Mesh buffer (vertex + index):
  - 与 chunk 活跃状态绑定
  - 离开活跃集后 grace period（60~120 帧）再释放

BLAS:
  - 与 mesh 同步生命周期
  - 释放需等 GPU 不再引用（RHI 延迟释放队列处理）

Chunk Card (SSBO slot):
  - 与 TFC chunk 活跃状态绑定

LFC Color Hash Table:
  - world-space hash table，映射 (LOD block 世界坐标) → 颜色/材质
  - RTGI 射线 hit LFC BLAS 后，通过 hit 位置查表获取 LOD block 颜色
  - 与 LFC chunk 活跃状态绑定
  - 数据来源：disk 持久化的 LFC color 数据，加载后写入 hash table
  - hash table 常驻 VRAM，采用开放寻址或 cuckoo hashing
  - 容量需支持所有活跃 LFC chunks 的 LOD blocks（可能数十万 entries）

PTLAS instances:
  - 与 chunk 活跃状态绑定
  - partition dirty tracking 驱动 update
```

---

## 4. LFC 全级别持久化

### 4.1 存储量估算（64km 视距）

每 visible block 存储格式：(chunk_inner_id, data) = 8B，加压缩。

| LFC 级别 | chunk 数 | 平均 visible blocks | 压缩后/chunk | 总存储 | 持久化策略 |
|----------|---------|-------------------|-------------|--------|----------|
| LFC4 (×16) | 49K | ~100 | ~0.6KB | **~30MB** | **全量持久** ✅ |
| LFC3 (×8) | 195K | ~300 | ~1.5KB | **~290MB** | **全量持久** ✅ |
| LFC2 (×4) | 780K | ~500 | ~3KB | **~2.3GB** | **全量持久** ✅ |
| LFC1 (×2) | 3.14M | ~400 | ~3.2KB | **~10GB** | **全量持久** ✅ |
| LFC0 (×1) | 12.5M | ~512 | ~1.5KB (palette+delta+RLE) | **~19GB** | **全量持久** ✅ |

**Total LFC 持久化: ~32GB**

### 4.2 为什么全量持久化所有 LFC 级别

**核心原则**：LFC 数据是体素数据的派生产物（face culling + 颜色提取后的可见表面信息），
生成代价高（涉及解压体素 + 面剔除 + 颜色提取），应只做一次并持久化。

- **LFC0**（×1，与体素 1:1）：等价于原始体素数据经面剔除后的可见表面集合。
  Merge 只能从 fine → coarse，LFC0 无法从任何 coarse level 得到，必须从体素生成。
  虽然它信息量与体素数据高度重叠，但存储格式不同（只含可见面 + 颜色），
  读取时无需再走面剔除，所以单独持久化有价值。
- **LFC1 ~ LFC4**：理论上可从 LFC0 级联 merge 得到（见 §4.4），
  但每次启动都重新 merge 上亿个 block 的开销不可接受。
  持久化后，运行时加载 LFC 纯粹是解压，无计算成本。

**统一持久化**还简化了加载路径：所有 LFC 级别走相同的 load → decompress 流程，
无需区分"某级从磁盘读、某级实时生成"的分支逻辑。

### 4.3 LFC0 压缩策略

LFC0 visible blocks 有强空间局部性：
- **Color**: palette encoding（每 chunk 通常 10~30 种颜色 → 4bit index）
- **Position**: delta encoding + RLE（连续 surface blocks 的 id 差值为 1）
- **整体**: zstd/LZ4 二次压缩
- 预估: 原始 4KB/chunk → 压缩后 ~1.5KB/chunk（~60% 压缩率）

### 4.4 级联 Merge

Coarse LFC 可以从 fine LFC merge 而来：

```
LFC0 → LFC1 → LFC2 → LFC3 → LFC4
 (merge)  (merge)  (merge)  (merge)
```

由于所有级别均持久化（§4.2），运行时**不需要**级联 merge。级联 merge 仅用于：
- 区块修改后重新生成受影响级别的 LFC，并级联更新更 coarse 的级别
- 数据损坏时的恢复路径

### 4.5 进入游戏时的加载序列

注意：所有 LFC 级别都在 disk 上全量持久化（§4.2），但加载到 RAM/VRAM 的范围
按 LOD 用途区分——远场级别（LFC3/4）需全世界常驻，近场级别（LFC0~2）只需附近区域。

```
1. 加载 LFC4 + LFC3 全世界（~320MB from disk）
   → 64km 远场地形轮廓立即可渲染
   注：这两级覆盖整个 64km，数据量小，适合全量驻留 RAM

2. 加载附近区域的 LFC2 + LFC1（按需从 disk 懒加载）
   → 中场景细节就绪

3. 加载附近区域的 LFC0（按需从 disk 懒加载）
   → fallback 级别就绪

4. 加载 VC/TFC 体素数据 → bake mesh → build BLAS
   → 最近距离的精确渲染
```

---

## 5. 流通路径与触发条件

### 5.1 流通路径图

```
                    load trigger          decompress
Disk (compressed) ──────────→ RAM (compressed) ──→ RAM (voxel cache)
                                                         │
                         ┌───────────────────────────────┤
                         │              │                │
                    mesh bake       LFC merge       card bake
                         │              │                │
                    RAM (mesh)     RAM (LFC)       RAM (card)
                         │              │                │
                         └──────upload──┴────upload──────┘
                                        │
                                  VRAM (renderable data)
```

### 5.2 触发条件

```
① Disk → RAM（加载触发）
  触发: Phase 0 LOD refinement 请求某 chunk 的某 LOD 数据，该数据不存在
  依据: distance + visibility + lod_gap 综合优先级
  方式: FIO 线程异步读取 → 解压 → 进入 voxel cache

② RAM voxel → RAM mesh/card/LFC（Bake 触发）
  触发: 体素数据就绪 + 该 chunk 需要对应 LOD 的渲染数据
  依据: job system 调度（优先级同 ①）
  方式: Worker thread 执行 mesh baking / card baking / LFC merge

③ RAM staging → VRAM（Upload 触发）
  触发: Bake/build 完成 + chunk 仍在活跃集
  方式: staging buffer → device buffer async copy（RHI 线程）

④ VRAM → 释放（Eviction 触发）
  触发: chunk 离开活跃集连续 N 帧（grace period = 60~120 帧）
  方式: 标记 pending eviction → 等 GPU 不再引用 → 释放 buffer slot

⑤ LFC disk cache miss
  触发: 需要某级 LFC 数据但 disk 无（首次探索该区域）
  方式: 从原始体素生成该级 LFC（面剔除 + 颜色提取）→ 持久化到 disk → 加载
  说明: 所有 LFC 级别均持久化（见 §4.2），cache miss 仅发生在首次探索

⑥ LFC 级联 merge（区块修改后）
  触发: 区块被修改（放置/移除 block），受影响级别的 LFC 需要重新生成
  方式: 从体素重新生成受影响级别 → 级联 merge 更 coarse 的级别 → 更新 disk
```

### 5.3 邻居依赖处理

Mesh baking 需要邻居的体素数据做边界面剔除。处理策略：

```
方案：不等（渐进式）

1. Chunk A 的体素数据就绪，但邻居 B 还没加载
2. 立即 bake A 的 mesh（保守地保留与 B 交界处的面）
3. 将 A 的 mesh upload 到 VRAM，立即可渲染
4. 当 B 加载完成后，触发 A 的 rebake（修正边界，剔除冗余面）
5. 新 mesh upload 替换旧 mesh

好处：不阻塞首次渲染，快速响应
代价：短暂的边界冗余面（几乎不可见），以及一次 rebake 开销
```

### 5.4 LOD 多版本共存

一个原始 chunk 可能同时需要多个 LOD 版本的渲染数据：

```
场景：chunk X 在 VC 距离内
  - 需要 VC 版本（mesh + 2 BLAS）
  - 同时可能被 TFC 距离的渲染作为 fallback 引用
  - 如果 TFC 版本不存在，不需要特意生成（VC 精度更高）

规则：
  - 每个 chunk 在 VRAM 中只保持"当前所需 LOD"的渲染数据
  - LOD 转换时，旧版本在 grace period 后释放
  - Fallback 引用的版本独立维护（如果 LFC1 未就绪，用 LFC2 fallback）
```

---

## 6. Budget 详细估算

### 6.1 VRAM Budget（总分配 ~4~6GB / 24GB）

```
VC (~200 chunks):
  Opaque mesh:       200 × 100KB = 20MB
  Semi-trans mesh:   200 × 10KB  = 2MB
  Opaque BLAS:       200 × 200KB = 40MB
  Semi-trans BLAS:   200 × 20KB  = 4MB
  ── VC subtotal: 66MB

TFC (~3000 chunks):
  Mesh:              3000 × 50KB  = 150MB
  Chunk card:        3000 × 10KB  = 30MB
  BLAS:              3000 × 100KB = 300MB
  ── TFC subtotal: 480MB

LFC:
  LFC0 (~200):       200 × 100KB  = 20MB
  LFC1 (~800):       800 × 20KB   = 16MB
  LFC2 (~2000):      2000 × 8KB   = 16MB
  LFC3 (~3000):      3000 × 5KB   = 15MB
  LFC4 (~2000):      2000 × 2KB   = 4MB
  Color Hash Table:                 = 10MB (数十万 entries × 4B)
  ── LFC subtotal: 81MB

Visibility buffer:   1080p × 4B   = 8MB
Depth buffer:        1080p × 4B   = 8MB
PTLAS:                             = 5MB
Buffer pool overhead:              = 500MB (预分配池，减少碎片)
────────────────────────────────
Total: ~1.1GB / 24GB → 非常宽裕
```

### 6.2 RAM Budget（使用 ~200MB / 64GB）

```
解压体素 - VC 常驻:       ~5MB   (200 chunks × 12 subchunks × 2KB, gameplay 用)
解压体素 - TFC/LFC 缓存:  ~72MB  (3000 chunks × 12 subchunks × 2KB, 渲染缓存)
ChunkRegistry metadata:    ~5MB
Mesh staging buffers:      ~20MB
Streaming I/O buffers:     ~30MB
LFC 暂存 (从 disk):        ~30MB
Gameplay 预留:             ~100MB (实体、物理、AI 等)
Job system overhead:       ~10MB
────────────────────────────────
Total: ~272MB / 64GB → 非常宽裕
```

### 6.3 Disk Budget

两个口径，注意区分：

**已探索区域（10km 半径，~12.5K chunks）**：
```
压缩体素:
  ~12.5K chunks × 100KB = ~1.2GB

LFC 持久化（全级别，见 §4.2）:
  LFC0~4 合计 ≈ ~12.5K × 9.8KB/chunk = ~122MB
  （per-chunk: LFC4 0.6KB + LFC3 1.5KB + LFC2 3KB + LFC1 3.2KB + LFC0 1.5KB，见 §4.1）

Mesh cache（可选）:
  ~12.5K chunks × 50KB = ~625MB（仅已探索区域）

────────────────────────────────
Total explored (10km): ~2GB
```

**全世界（64km 全量，~12.5M LFC0 chunks）**：
```
LFC0~4 合计 ≈ ~32GB（见 §4.1）
注意：64km 全量持久化是理论上限，实际只有已探索区域会落盘。
未探索区域的 LFC 在玩家接近时按 §5.2 ⑤ 首次生成并持久化。
```

---

## 7. Streaming Pipeline 架构（概要）

> 详细的 Job DAG 和优先级系统设计待进一步讨论。

### 7.1 组件

```
StreamingManager:
  ├─ ChunkRegistry      — 全局 chunk 状态表（chunk_id → state + LOD availability）
  ├─ LoadRequestQueue    — 优先级排序的加载请求队列
  ├─ FIOInterface        — 与 FIO 线程交互（异步 disk 读取）
  ├─ JobScheduler        — 将 bake/merge/build 任务分发到 worker threads
  ├─ UploadQueue         — mesh/BLAS upload 请求（提交到 RHI 线程）
  └─ EvictionTracker     — 跟踪 VRAM buffer 的使用状态和 grace period
```

### 7.2 Chunk 状态

```
Empty → Requested → Loading(decompress) → VoxelReady
  → MeshBaking → MeshReady → BLASBuilding → Ready → Evicting → Empty

LFC 路径（从 coarse merge）:
Empty → Requested → Merging → MergeReady → BLASBuilding → Ready
```

### 7.3 优先级

```
Priority = f(distance, screen_area, lod_gap, view_direction)

- distance: 越近优先级越高
- screen_area: 屏幕占比越大优先级越高
- lod_gap: 当前 fallback LOD 与 target LOD 差距越大，优先级越高
- view_direction: 注视方向的前方 chunk 优先于背后
```

---

## 8. 待决问题

### Q1: Streaming Pipeline 的 Job DAG 详细设计

各 job（decompress、mesh bake、card bake、LFC merge、BLAS build）之间的依赖关系和并行度。特别是邻居依赖如何影响 DAG 拓扑。

### Q2: GPU Buffer Pool 策略

如何管理大量 chunk 的 mesh/BLAS buffer 分配和释放？
- 预分配大块池 vs 按需分配？
- 按 LOD 类型分池（VC pool / TFC pool / LFC pool）？
- 碎片管理和 compaction？

### Q3: LOD 转换时的双份数据

chunk 从 TFC 升级为 VC 时，新旧 mesh 在 VRAM 中短暂共存。
峰值 VRAM 使用量是多少？是否需要限制同时转换的 chunk 数量？

### Q4: Mesh Disk Cache 策略

是否启用 mesh/card 的 disk cache？
- 好处：二次加载跳过 meshing + BLAS build
- 代价：额外 disk 空间 + cache invalidation 复杂度
- 建议：作为可选优化，不阻塞核心设计
