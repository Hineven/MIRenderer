# GigaVoxel LOD 层级定义

## 1. 总览

GigaVoxel 将体素世界按视距分层为三种 Chunk 类型：VC、TFC、LFC。
它们的核心区别在于 **体素分辨率**、**几何表达方式**、**材质表达方式** 和 **渲染方式**。

```
相机
 │
 ├─ VC (Vanilla Chunk)           0 ~ 128m     精确体素 + 精确材质 (atlas)
 ├─ TFC (Textured Far Chunk)   128 ~ 512m     精确体素 + 烘焙纹理卡片 (chunk card)
 └─ LFC (LOD Far Chunk)        512m ~ 64km    单色 LOD blocks
     ├─ LFC0 (×1)              512m ~ 1km     精确体素 + 单色（不合并，基线级）
     ├─ LFC1 (×2)              512m ~ 1km     2:1 合并
     ├─ LFC2 (×4)              1 ~ 2km        4:1 合并
     ├─ LFC3 (×8h×4v)          2 ~ 8km        非对称合并
     └─ LFC4 (×16h×8v)         8 ~ 64km       非对称合并
```

> 距离边界为初始设计值，实际可按 culling 中的屏幕投影面积动态调整。
> LFC0 与 LFC1 可共存于同一距离带，LFC0 常用作区块刚进入 LFC 范围时的 fallback 级。

---

## 2. 基础单位：Chunk 与 Block

| 概念 | 定义 |
|------|------|
| **Block** | 最小体素单元，1m × 1m × 1m |
| **Chunk** | 16 × 4096 × 16 blocks（水平 16m × 高度 4096m × 纵深 16m），共 1,048,576 blocks/chunk |
| **SubChunk** | Chunk 按高度切分的子区域，用于 culling。建议每 64 blocks 一个 subchunk → 每 chunk 64 个 subchunks |
| **LOD Block** | LFC 中合并后的体素单元，边长 = 1m × LOD 倍率 |

---

## 3. 各级别详细定义

### 3.1 VC — Vanilla Chunk

| 属性 | 值 |
|------|-----|
| **距离范围** | 0 ~ 128m（约 8 chunk 半径） |
| **体素分辨率** | 1:1（原始 16×4096×16 blocks） |
| **几何** | Greedy Meshing 生成的精确三角面片（每 chunk 约 2000~8000 triangles，取决于地形复杂度） |
| | **Greedy Meshing 说明**：只合并相同 block type 的连续共面面片，UV 通过 frac() tiling 重复 atlas tile。 |
| | 交替 pattern（石-草-石-草）下退化为 per-block quad，但在自然地形中平均压缩率 75~90%。 |
| **材质** | 完整 block material（base color、normal、roughness、emissive 等） |
| **纹理** | 直接引用全局 block texture atlas，标准 UV |
| **BLAS** | 每 chunk 独立 BLAS，`kPreferFastTrace` |
| **TLAS Instance** | 每 chunk 1 个 instance |
| **光栅渲染** | 硬件光栅（标准 draw call） |
| **光追渲染** | 完整 RT（hit shader 访问完整材质） |
| **邻居依赖** | Greedy meshing 需邻居 chunk 边界数据做面剔除 |
| **CPU 开销** | 高（greedy meshing 需遍历 1M blocks） |
| **GPU 内存/chunk** | ~100KB mesh (含 normal/uv) + ~200KB BLAS ≈ 300KB |

**VC 是最"诚实"的表达**——几何和材质与原始体素数据完全一致。

---

### 3.2 TFC — Textured Far Chunk

| 属性 | 值 |
|------|-----|
| **距离范围** | 128 ~ 512m（约 8~32 chunk 半径） |
| **体素分辨率** | 1:1（几何基于原始 16×4096×16 blocks） |
| **几何** | CPU 烘焙的简化 mesh（可做 more aggressive 的 greedy meshing / face merging） |
| **材质** | Chunk Card 系统——每面一张小纹理卡 |
| **纹理（Chunk Card）** | top/bottom: 16×16，sides: 16×32（更高的高度分辨率），CPU 端烘焙，SSBO 存储 |
| **BLAS** | 每 chunk 独立 BLAS，`kPreferFastTrace` |
| **TLAS Instance** | 每 chunk 1 个 instance |
| **光栅渲染** | 硬件光栅，fragment shader 从 chunk card buffer 查色 |
| **光追渲染** | RT hit shader 从 chunk card buffer 查色（简化材质） |
| **邻居依赖** | 烘焙 mesh 时需邻居数据做面剔除 |
| **CPU 开销** | 中（mesh 烘焙 + card 烘焙，遍历 1M blocks） |
| **GPU 内存/chunk** | ~50KB mesh + ~8KB card + ~100KB BLAS ≈ 158KB |

**TFC 的核心思想**：几何仍然是精确的（1:1 体素），但材质从"逐 block 引用纹理"退化为"逐面一张烘焙纹理卡"。

**Chunk Card 详解**：
- 每个 face 的 card 是该面上所有可见 block 纹理的投影拼合
- **Top/Bottom face**: 16×16 texels（水平 16m × 16m，1:1 映射）
- **Side faces (N/S/E/W)**: 16×32 texels（水平 16m × 垂直 4096m，垂直方向 128:1 压缩）
  - 侧面的垂直分辨率很低，但在 TFC 距离上几乎看不到侧面细节，可以接受
  - 如需提升，可改为 16×64 或 32×64，开销仍然可控
- 每 chunk card 总大小: 2×(16×16) + 4×(16×32) = 512 + 2048 = **2560 texels × 4 bytes ≈ 10KB**
- Card 在 CPU 端烘焙：遍历该面可见 block → 采样其纹理 → 写入 card 对应像素
- Card 以 `StructuredBuffer<uint>` (或 `uint4`) 形式存储在 GPU SSBO 中
- Shader 访问：`instance_custom_index` → card buffer offset → texel lookup
- 流送：staging buffer → device buffer async copy

---

### 3.3 LFC — LOD Far Chunk

LFC 是最复杂的一级，因为它涉及 **体素合并** + **区块合并**。

#### 3.3.1 LOD 倍率与合并规则

> 4096m 高度下，采用 **非对称合并**：水平倍率 N_h，高度倍率 N_v（N_v ≤ N_h）。
> 目的：保留地形起伏轮廓，远看山脉不会变成扁平色块。

| LOD 级别 | 水平倍率 N_h | 高度倍率 N_v | Chunk 合并(水平) | 单 LFC Chunk 覆盖 | LOD Block 尺寸 | LOD Block 数量 |
|----------|-------------|-------------|------------------|-------------------|---------------|----------------|
| LFC0 | ×1 | ×1 | 无合并 | 16m × 4096m × 16m | 1×1×1 m | 16×4096×16 = 1M |
| LFC1 | ×2 | ×2 | 2×2 chunks → 1 | 32m × 4096m × 32m | 2×2×2 m | 16×2048×16 = 524K |
| LFC2 | ×4 | ×4 | 4×4 chunks → 1 | 64m × 4096m × 64m | 4×4×4 m | 16×1024×16 = 262K |
| LFC3 | ×8 | ×4 | 8×8 chunks → 1 | 128m × 4096m × 128m | 8×4×8 m | 16×1024×16 = 262K |
| LFC4 | ×16 | ×8 | 16×16 chunks → 1 | 256m × 4096m × 256m | 16×8×16 m | 16×512×16 = 131K |

> **非对称合并说明**：
> - LFC1/LFC2：水平和高度等比合并，LOD block 为正方体
> - LFC3(×8)：水平×8 但高度仅×4，LOD block 为 8×4×8（扁长方体），保留更多高度细节
> - LFC4(×16)：水平×16 但高度仅×8，LOD block 为 16×8×16，仍有 512 层高度细节

#### 3.3.2 LFC 通用属性

| 属性 | 值 |
|------|-----|
| **距离范围** | 512m ~ 64km |
| **体素分辨率** | N:1 合并（N = LOD 倍率） |
| **几何** | LOD blocks 的简化 mesh（每个 LOD block 一个 cube，做面剔除后剩余外表面） |
| **材质** | **每个 LOD block 一个单色**（从原始体素统计/平均得出） |
| **颜色存储** | World-space hash table 或 per-LFC-chunk color array |
| **BLAS** | 每 LFC chunk 独立 BLAS（含该 LFC chunk 内所有 LOD block 面片） |
| **TLAS Instance** | 每 LFC chunk 1 个 instance |
| **光栅渲染** | Compute Shader 软光栅（批量处理所有可见 LFC） |
| **光追渲染** | RT hit shader 查 LOD block 单色 |
| **邻居依赖** | 有。确定“最外层 LOD Block”需查询相邻 LFC chunk 的边界数据做面剔除。与 VC/TFC 类似，只在烘焙时依赖；区块内容未变化则无需更新 |
| **CPU 开销** | 低（简单 cube mesh + 颜色统计） |

**LFC 邻居依赖详解**：

LFC 的 mesh 只包含“最外层 LOD Block”的面片。判断一个 LOD block 是否“最外层”，需要查询相邻 LFC chunk 边界处的 LOD block 是否为 solid：

```
Chunk A (LFC2)    Chunk B (LFC2)
┌───────────────┐ ┌───────────────┐
│  ███ ███      │ │      ███ ███ │
│  ███ ███      │ │      ███ ███ │
│         ███   │ │ ███           │ ← 边界处：两个 solid LOD block
│         ███   │ │ ███           │    相邻，公共面应剔除
└───────────────┘ └───────────────┘
```

- **烘焙时机**：CPU 端烘焙 LFC mesh 时，需邻居 LFC chunk 的 LOD block 数据已就绪
- **与 VC/TFC 一致**：邻居依赖只在 mesh 烘焙时存在；区块内容未变化则无需重新烘焙
- **邻居未就绪时**：保守地保留边界面上（不剔除），等邻居就绪后异步修正
- **LFC 邻居是同级 LFC**：LFC2 的邻居也是 LFC2（同 LOD level），不跨 level

#### 3.3.3 各级 LFC 特有考量

**LFC0 (×1, 基线级)**
- 单 LFC chunk = 1 原始 chunk = 16×4096×16 blocks → 1M LOD blocks (1m cube)
- 不合并体素，但使用单色材质模型（与 LFC1-4 相同的 cube mesh + color pipeline）
- 用途：区块刚进入 LFC 范围时的 fallback（LFC1 mesh 尚未就绪时先用 LFC0 显示）
- 颜色直接来自原始 block data 的 base color，无需统计/平均
- GPU 内存/LFC chunk：~50KB mesh + ~128KB colors + ~50KB BLAS ≈ 228KB
- 注意：因为 LOD block 数量巨大（1M），LFC0 仅作为临时 fallback，不长期驻留

**LFC1 (×2)**
- 单 LFC chunk = 2×2 原始 chunks = 32×4096×32 blocks → 16×2048×16 LOD blocks (2m cube)
- 几何仍有一定精度，地形起伏清晰
- 颜色从 2³ = 8 个原始 blocks 平均，信息损失较小
- GPU 内存/LFC chunk：~20KB mesh + ~64KB colors + ~20KB BLAS ≈ 104KB

**LFC2 (×4)**
- 单 LFC chunk = 4×4 原始 chunks = 64×4096×64 blocks → 16×1024×16 LOD blocks (4m cube)
- LOD block 4m 边长，开始有明显的像素化
- 颜色从 4³ = 64 个原始 blocks 统计
- GPU 内存/LFC chunk：~8KB mesh + ~32KB colors + ~8KB BLAS ≈ 48KB

**LFC3 (×8h ×4v)**
- 单 LFC chunk = 8×8 原始 chunks = 128×4096×128 blocks → 16×1024×16 LOD blocks (8×4×8m)
- LOD block 扁长方体，保留高度轮廓
- 颜色从 8×4×8 = 256 个原始 blocks 统计
- GPU 内存/LFC chunk：~8KB mesh + ~32KB colors + ~8KB BLAS ≈ 48KB

**LFC4 (×16h ×8v)**
- 单 LFC chunk = 16×16 原始 chunks = 256×4096×256 blocks → 16×512×16 LOD blocks (16×8×16m)
- LOD block 极大扁长方体，但仍有 512 层高度 → 山脉轮廓仍然可辨
- 颜色从 16×8×16 = 2048 个原始 blocks 统计
- GPU 内存/LFC chunk：~4KB mesh + ~16KB colors + ~4KB BLAS ≈ 24KB

---

## 4. 对比总结

| 维度 | VC | TFC | LFC0(×1) | LFC1(×2) | LFC2(×4) | LFC3(×8h×4v) | LFC4(×16h×8v) |
|------|-----|------|----------|----------|----------|--------------|---------------|
| 距离 | 0-128m | 128-512m | 512m+(fallback) | 512m-1km | 1-2km | 2-8km | 8-64km |
| 体素精度 | 1:1 | 1:1 | 1:1 | 2:1 | 4:1 | 8:1h 4:1v | 16:1h 8:1v |
| Chunk 范围 | 1 chunk | 1 chunk | 1 chunk | 2×2 chunks | 4×4 chunks | 8×8 chunks | 16×16 chunks |
| LOD Block 尺寸 | 1m cube | 1m cube | 1m cube | 2m cube | 4m cube | 8×4×8 m | 16×8×16 m |
| LOD Block 数量 | 1M | 1M | 1M | 524K | 262K | 262K | 131K |
| 几何来源 | Greedy mesh | Baked mesh | LOD cubes | LOD cubes | LOD cubes | LOD cuboids | LOD cuboids |
| 材质 | Atlas+UV | Chunk card | Single color | Single color | Single color | Single color | Single color |
| 光栅方式 | HW raster | HW raster | SW raster | SW raster | SW raster | SW raster | SW raster |
| BLAS | Per-chunk | Per-chunk | Per-chunk | Per-LFC-chunk | Per-LFC-chunk | Per-LFC-chunk | Per-LFC-chunk |
| ~GPU 内存 | ~300KB | ~158KB | ~228KB | ~104KB | ~48KB | ~48KB | ~24KB |
| **邻居依赖** | 强(meshing) | 强(meshing) | 有(边界) | 有(边界) | 有(边界) | 有(边界) | 有(边界) |
| **CPU 处理** | 重 | 中 | 轻 | 轻 | 轻 | 轻 | 轻 |

---

## 5. 待决设计问题

### Q1: LFC 高度合并策略 ✅ 已决策

采用 **非对称合并**：高度倍率 N_v ≤ 水平倍率 N_h。
- LFC1/LFC2：等比合并（LOD block 为正方体）
- LFC3：水平×8 高度×4（LOD block 8×4×8 扁长方体）
- LFC4：水平×16 高度×8（LOD block 16×8×16，仍有 512 层高度细节）

好处：远看山脉仍然有起伏轮廓，不会变成扁平色块。

### Q2: LFC LOD Block 颜色来源

当 LFC 的 LOD block 合并时，颜色如何确定？
- **方案 A**：体积平均——所有非空 blocks 的 base color 平均
- **方案 B**：表面加权——只统计表面 blocks（从 6 个方向看最外层的 blocks）
- **方案 C**：主色提取——取出现频率最高的 block type 的颜色

方案 B 更准确（内部石头不应影响表面颜色），但计算更复杂。

### Q3: LFC Chunk 边界的 LOD Block 面剔除 ✅ 已决策

所有 LFC 级别都做边界 LOD block 面剔除。LFC mesh 只包含“最外层 LOD Block”的面片，而确定“最外层”必须查询相邻 LFC chunk 的边界数据。

这与 VC/TFC 的邻居依赖一致：只在 mesh 烘焙时依赖，区块内容未变化则无需更新。

### Q4: LOD 过渡区域的裂缝处理

当相邻 chunk 处于不同 LOD 级别时（如一个 TFC 旁边一个 LFC1），
它们共享的边界处几何不连续，会出现 **LOD crack**。

- **方案 A**：Transition mesh——在边界处生成过渡三角形连接两边
- **方案 B**：Morphing——在 GPU 里将高精度侧的边界顶点向低精度侧 lerp
- **方案 C**：Skirt——在低精度侧边界生成垂直裙边遮挡裂缝

方案 C 最简单但视觉上有 artifact；方案 A 最干净但实现复杂。

### Q5: SubChunk 设计

SubChunk 的详细设计见下方 §7 独立章节。

---

## 7. SubChunk 设计详解

### 7.1 SubChunk 尺寸选择

SubChunk 是 chunk 在 Y 轴上的切分单元，同时是 **存储、加载、meshing、culling** 的基本单位。

| 方案 | SubChunk 尺寸 | 每 Chunk 数量 | 优势 | 劣势 |
|------|------------------|----------------|------|------|
| A | 16×16×16 | 256 | MC 经典尺寸，久经验证；粒度精细，空跳过率高 | 每个 chunk 256 个 section，元数据开销大；meshing 边界多 |
| B | 16×32×16 | 128 | 折中方案 | 非二次幂，不优雅 |
| C | 16×64×16 | 64 | 元数据开销小；单个 mesh 更大，边界少 | 粒度粗，空跳过率低；空 subchunk 可能仍含 20% 实心块 |
| D | 16×128×16 | 32 | 元数据最少 | 128m 高的 subchunk 太大，culling 几乎无效 |

**建议：16×16×16（方案 A）**。理由：
- 4096m 高度意味着每 chunk 上方可能有 100+ 个纯空气 subchunk，细粒度跳过很有价值
- 16³ = 4096 blocks / subchunk 数据量适中，压缩后 1~4KB
- 与 MC 生态兼容，未来可用现有工具链
- Palette 压缩在小 volume 上更高效（块类型多样性更低）

### 7.2 SubChunk 数据结构

```
Chunk (存储/压缩单元) {
  ChunkCoord coord;
  Palette    chunk_palette;     // chunk 级调色板，所有 subchunk 共享
  SubChunk   subchunks[256];    // 内存组织单元
}

SubChunk (内存组织单元) {
  ChunkCoord chunk_coord;      // 所属 chunk
  uint8_t    section_y;        // 在 chunk 内的 Y index (0~255)
  SubChunkState state;         // Unloaded/Loading/Ready/Meshing/...

  // 体素数据（解压后，引用 chunk_palette）
  uint8_t[]  indices;          // 4096 个 palette index (4bit/index → 2KB)
                               // 仅在 is_empty == false 时分配

  // 元数据
  bool       is_empty;         // 全空快速跳过
  bool       is_uniform;       // 全同一种 block（不需要 indices）
  AABB       mesh_aabb;        // mesh 的包围盒（用于 culling）
  uint32_t   triangle_count;   // mesh 三角形数

  // Mesh 数据（CPU 端生成，上传 GPU）
  MeshData   mesh;             // greedy meshing 结果
}
```

**Chunk 级 Palette 压缩**：
- 整个 chunk 的 block types 共享一个 palette，比 subchunk 级 palette 去重更高效
- 典型 chunk 有 30~100 种 block type → 7-bit index 足够
- 磁盘存储: chunk_palette (~100 bytes) + indices (1M × 7bit ≈ 900KB) + 熵编码 → **~50~200KB**
- 内存解压: 每非空 subchunk 独立分配 4-bit indices (2KB)，引用 chunk_palette

### 7.3 SubChunk 与 Greedy Meshing 的关系

核心问题：**meshing 边界是否跨过 subchunk？**

| 策略 | 描述 | 优势 | 劣势 |
|------|------|------|------|
| **独立 meshing** | 每个 subchunk 只在自己的 16×16×16 内 mesh | 完全独立，可单独加载/更新 | subchunk 边界处有冗余面（相邻都 solid 时未剔除） |
| **跨边界 meshing** | meshing 时访问上下邻居 subchunk 的边界层 | 最优三角面数，无冗余面 | 加载一个 subchunk 需邻居数据就绪；更新一个 subchunk 需重 mesh 邻居 |
| **混合** | 默认独立 meshing，当邻居就绪后异步修正边界 | 渐进式优化，不阻塞加载 | 实现复杂 |

**建议：独立 meshing + 边界层优化**。具体做法：

```
Greedy mesh 一个 subchunk 时：
1. 在 subchunk 内部做标准 greedy meshing
2. 对于 subchunk 的 top/bottom 边界（Y=0 和 Y=15）：
   - 如果邻居 subchunk 已加载 → 查邻居边界层，剔除公共面
   - 如果邻居未加载 → 保守地保留这些面（当邻居加载后再修正）
3. 这样每个 subchunk 可以独立加载和渲染，但邻居就绪后会变得最优
```

冗余面的开销：每个 subchunk 上下边界最多 16×16×2 = 512 个面，占总面数的 ~10%，可以接受。

### 7.4 SubChunk 在 Culling 中的作用

```
渲染时的 culling 层级：

Level 1: Chunk Frustum Cull (粗筛)
  └─ 整个 chunk AABB vs frustum

Level 2: SubChunk Frustum Cull (细筛)
  └─ 每个非空 subchunk AABB vs frustum
  └─ 4096m 高度下，一个 128m 距离的 chunk 可能只有 30~40% subchunks 在视锥内

Level 3: SubChunk Hi-Z / Occlusion Cull
  └─ 每个 subchunk 用 mesh_aabb 做 conservative depth test
  └─ 被前方地形遮挡的地下 subchunk 直接跳过

Level 4: SubChunk Empty Skip
  └─ is_empty == true → 直接跳过，不参与任何渲染
```

**4096m 高度下的空 subchunk 分布**：

```
典型地形（地表高度 ~64m）：
  Y = 0~64:    实心/混合 subchunks（~4 个非空）
  Y = 64~4096: 全空 subchunks（~252 个空！）

典型高山（山巅 ~200m）：
  Y = 0~200:   实心/混合（~12 个非空）
  Y = 200~4096: 全空（~244 个空）

→ 在 4096m 世界中，每 chunk 平均 90%+ subchunks 是空的
→ SubChunk empty skip 是最重要的优化之一
```

### 7.5 SubChunk 与流送/存储

**I/O 与压缩以 Chunk 为单位，内存组织以 SubChunk 为单位。**

```
磁盘存储：
  - 一个 chunk 作为一个压缩单元存储/读取
  - 压缩方案：chunk 级 palette + 熵编码（如 zstd / LZ4）
  - chunk 级 palette 比 subchunk 级更大，跨 subchunk 的 block type 去重更高效
  - 典型 chunk 压缩后大小：~50~200KB（取决于非空 subchunk 数量）
  - 单次 I/O 加载整个 chunk，避免大量小请求的调度开销

内存解压后组织：
  - 解压后按 subchunk 拆分到各自的 SubChunk 结构
  - 空 subchunk 直接标记 is_empty，不分配 indices 内存
  - 非空 subchunk 分配 palette + indices，用于 meshing/culling

流送策略：
  - 以 chunk 为粒度 request 加载
  - 加载完成后，仅解压可见/非空 subchunks 到内存
  - 空 subchunks 只记录 is_empty flag，几乎零开销
```

### 7.6 SubChunk 与 LOD 的关系

SubChunk 只对 VC 和 TFC 有意义（1:1 体素精度）。LFC 已经做了空间合并，不需要 subchunk。

| LOD | SubChunk | 说明 |
|-----|----------|------|
| VC | 16×16×16 | 完整 subchunk，每个都有独立 mesh + 完整材质 |
| TFC | 16×16×16 | 完整 subchunk，但 mesh 是烘焙简化版 + chunk card |
| LFC0 | N/A | 1:1 体素但使用 cube mesh + 单色，不按 subchunk 切分 |
| LFC1~4 | N/A | 已空间合并，LOD block 本身就是 culling 粒度 |

### 7.7 SubChunk 与 BLAS

每个 subchunk 有自己的 mesh，BLAS 的组织方式有两种选择：

| 方案 | 描述 | 优势 | 劣势 |
|------|------|------|------|
| **Per-subchunk BLAS** | 每个 subchunk 一个独立 BLAS | 更新一个 subchunk 只需 rebuild 该 BLAS | 每 chunk 256 个 BLAS，TLAS instance 数爆炸 |
| **Per-chunk BLAS** | chunk 内所有 subchunk mesh 合并成一个 BLAS | TLAS instance 少 | 更新一个 subchunk 需 rebuild 整个 chunk BLAS |

**建议：Per-chunk BLAS，但 mesh 按 subchunk 分区存储**。
- Mesh buffer 内按 subchunk 分段（每段有独立的 vertex/index range）
- BLAS 构建时一次性包含所有 subchunk mesh
- 更新时 rebuild 整个 chunk BLAS（VC chunk 更新频率低，可接受）
- TLAS instance 仍然是 per-chunk，保持数量可控

---

## 6. 数据流总览

```
                    ┌───────────────────────────────────────┐
                    │         Chunk Source Data             │
                    │  (Disk compressed / Worldgen output)  │
                    └───────────┬───────────────────────────┘
                                │
                    ┌───────────▼───────────────────────────┐
                    │      CPU: Chunk Decompression         │
                    │      + Block Data Ready               │
                    └──┬────────────┬──────────────┬────────┘
                       │            │              │
                ┌──────▼──┐  ┌─────▼─────┐  ┌────▼─────┐
                │VC Path  │  │TFC Path   │  │LFC Path  │
                │         │  │           │  │          │
                │Greedy   │  │Bake Mesh  │  │Merge     │
                │Meshing  │  │+ Bake     │  │Blocks →  │
                │(+邻居)  │  │Chunk Card │  │LOD Mesh  │
                │         │  │(+邻居)    │  │+ Color   │
                └────┬────┘  └─────┬─────┘  └────┬─────┘
                     │             │              │
                ┌────▼────┐  ┌────▼─────┐  ┌────▼─────┐
                │Build    │  │Build     │  │Build     │
                │BLAS     │  │BLAS      │  │BLAS      │
                └────┬────┘  └────┬─────┘  └────┬─────┘
                     │            │              │
                     └────────────┼──────────────┘
                                  │
                    ┌─────────────▼─────────────────────────┐
                    │     TLAS Instance Pool Update         │
                    │  (per-chunk instance, lod mask)       │
                    └─────────────┬─────────────────────────┘
                                  │
                    ┌─────────────▼─────────────────────────┐
                    │     Render: Culling → Raster/RT       │
                    └───────────────────────────────────────┘
```
