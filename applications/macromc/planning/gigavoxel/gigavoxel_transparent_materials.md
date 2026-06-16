# GigaVoxel 半透明材质处理

## 1. 核心设计原则

**三个分离 + LFC 简化：**
- **Mesh 分离**：每个 chunk 同时维护 opaque mesh 和 semi-transparent mesh，独立生成、独立更新
- **BLAS 分离**：opaque BLAS 和 semi-transparent BLAS 独立，opaque 可开启 kOpaque flag 优化 tracing
- **渲染分离**：VC/TFC 的 opaque 走 visibility buffer 路径，semi-transparent 走 sorting + forward 路径
- **RTGI 分离**：只有 VC 的 BLAS 包含 semi-transparent 几何，TFC/LFC 完全忽略
- **LFC 简化**：所有 semi-transparent 一律当作 opaque 渲染，水面反射通过特殊材质近似

---

## 2. Semi-Transparent 包含哪些 Block

不只是水面，还包括：
- **水面**：半透明，有折射/反射/吸收
- **树叶**：alpha cutoff（镂空纹理），属于 semi-opaque
- **玻璃**：半透明
- **冰**：半透明
- **其他**：任何非完全 opaque 的 block type

这些统一归入 "semi-transparent" 类别，共享同一套 mesh/BLAS/rendering 管线。

---

## 3. 水面/Semi-Transparent Mesh 的 LOD 维护

| LOD | Semi-Transparent Mesh 策略 | 邻居依赖 |
|-----|---------------|----------|
| VC | Greedy meshing（同类只与同类合并），完整几何 | 有（跨 chunk 合并） |
| TFC | Baked mesh，简化几何 | 有 |
| LFC0~4 | **不单独维护**，含水/semi-transparent 的 LOD block 当作普通 opaque 色块渲染 | 无额外 |

**关键点**：
- Opaque mesh 烘焙时 **完全忽略 semi-transparent blocks**（视为空气）
- Semi-transparent mesh 烘焙时 **只处理 semi-transparent blocks**
- LFC 不区分 semi-transparent：水 LOD block 就是一个蓝色的 opaque cube

---

## 5. 光栅渲染

### 5.1 VC / TFC Semi-Transparent 渲染

标准透明渲染流程：

1. **收集**：从 visible chunk list 中筛选出含 semi-transparent mesh 的 chunks
2. **排序**：
   - 第一级：per-subchunk 排序（按 subchunk center 到相机的距离，back-to-front）
   - 第二级：subchunk 内部 per-surface 排序（按 surface center 深度）
3. **渲染**：用独立的 semi-transparent pipeline 做 forward rendering
   - 不写 depth buffer（保留 opaque 的 depth）
   - 做 depth test（被 opaque 遮挡的不渲染）
   - 写入独立的 transparent color target（blend）

> **排序说明**：Per-subchunk 作为第一级粒度，平衡了精度和开销。
> Subchunk 内部的 per-surface 排序可以简化为按面朝向分组（背向相机的先渲染）。
> 对于精确的 per-pixel 排序（如多层水面重叠），可以用 depth peeling 但开销较大，初期可以不做。

### 5.2 LFC Semi-Transparent 渲染——不做特殊处理

**LFC 所有 semi-transparent blocks 一律当作 opaque 渲染。**

- 水面 LOD block = 蓝色 opaque cube，走普通 SW raster
- 树叶 LOD block = 绿色 opaque cube，走普通 SW raster
- 不需要 sorting，不需要 forward pass，不需要 alpha blend

**水面反射的特殊处理**：
- LFC 水面 LOD block 使用特殊材质：高反射率 + 蓝色基色
- 在 RTGI 阶段，射线 hit 水面 LOD block 时返回类似反射的颜色（而不是真实水面折射）
- 视觉效果：远处水面看起来像“闪亮的蓝色平面”，足够近似

> 这个简化的收益巨大：LFC 不需要任何透明渲染基础设施，
> 而水面反射通过材质属性（而非真实折射）近似，开销几乎为零。

---

## 4. BLAS 策略：Opaque + Semi-Transparent 分离

每个 VC chunk 维护两个 BLAS：

| BLAS | 内容 | 光追 Flag | 说明 |
|------|------|----------|------|
| **Opaque BLAS** | 完全不透明的几何 | `kOpaque`（无 any-hit shader） | 硬件优化 tracing，更快 |
| **Semi-Transparent BLAS** | 水面 + 树叶 + 玻璃 + 冰等 | 无 opaque flag（需 any-hit） | 支持 alpha test / 半透明 |

**为什么分离？**
- `kOpaque` flag 让光追硬件跳过 any-hit shader，tracing 性能显著提升
- 如果把 semi-transparent 几何混入 opaque BLAS，整个 BLAS 都无法启用该优化
- 树叶的 alpha cutoff 需要 any-hit shader 做 discard，也会拖慢 tracing

**TFC 的 BLAS**：
- TFC 只有一个 BLAS（不含 semi-transparent 几何）
- TFC 的 semi-transparent blocks 在光栅中作为简化材质处理，不参与 RT

**LFC 的 BLAS**：
- LFC 只有一个 BLAS，所有 LOD block（包括水面）当作 opaque 处理
- 水面 LOD block 就是一个蓝色不透明 cube

**TLAS 中的 Instance 管理**：
- Opaque BLAS instance：mask 标记为 opaque
- Semi-transparent BLAS instance：mask 标记为 semi-transparent，仅在 VC 距离内存在
- 射线可以通过 mask 选择是否 query semi-transparent instances

### 4.2 射线交互

```
VC 范围内射线：
  命中 opaque BLAS → 标准 opaque 反射/漫反射
  命中 semi-transparent BLAS:
    水面 → 反射射线 + 折射射线 + Beer's law 吸收
    树叶 → alpha test，discard 则继续追踪
    玻璃/冰 → 半透明折射

TFC/LFC 范围内射线：
  只命中 opaque BLAS（无 semi-transparent BLAS）
  水面 LOD block 当普通不透明蓝色表面 → 射线返回蓝色
  视觉上近似正确（远处水面就是蓝色反光）
```

---

## 6. 管线集成

### 修改后的管线流程

```
Phase 3A: HW Raster VC+TFC (opaque only) — graphics, 写 visibility + depth
Phase 3B: SW Raster LFC (all opaque, 包括水面/树叶当 opaque) — compute, 读 HW depth
Phase 3C: HW Raster VC+TFC (semi-transparent, sorted forward) — graphics, 独立 color target

Phase 4: Visibility Resolve + Composite
  1. Opaque visibility → G-Buffer
  2. 叠加 semi-transparent color target → 最终 G-Buffer
```

### 执行顺序

```
Phase 3A (VC+TFC opaque)
  ↓
Phase 3B (LFC all opaque, 含 semi-transparent 当 opaque)
  ↓
Phase 3C (VC+TFC semi-transparent, sorted forward)
  ↓
Phase 4 (Resolve + Composite)
```

> 注意：HW 先于 SW（3A→3B），二者共享 G_depth_，SW 用 HW depth 做剔除。
> LFC 没有单独的 transparent pass。所有 semi-transparent 在 Phase 3B 已当 opaque 处理。
> semi-transparent 完全不碰 visibility buffer——visibility 是 opaque-only。
> 详见 `gigavoxel_visibility_buffer.md` §5。

---

## 7. 水面在各 LOD 的视觉表现

| LOD | 水面视觉 | 质量 |
|-----|---------|------|
| VC | 完整水面：波动、折射、反射、半透明 | 最高 |
| TFC | 简化水面：半透明蓝色，简单光照 | 中等 |
| LFC0~4 | 蓝色 opaque cube + 高反射材质 | 最低（但远处足够近似） |

---

## 8. Semi-Transparent Mesh 更新触发条件

| 事件 | 影响 |
|------|------|
| 放置/移除水源、树叶等 block | 触发该 chunk 的 semi-transparent mesh rebake + semi-transparent BLAS rebuild（仅 VC） |
| 水源流动 | 触发 water mesh rebake |
| 邻居 chunk 加载完成 | 触发边界 mesh 修正 |
| LOD 转换 | semi-transparent mesh 随 LOD 转换（TFC 以上不再维护独立 mesh） |

---

## 9. 待决问题

### Q1: Semi-Transparent BLAS 独立于 Opaque BLAS ✅ 已决策

采用独立 BLAS：
- **Opaque BLAS**：开启 `kOpaque` flag，无 any-hit shader，tracing 更快
- **Semi-Transparent BLAS**：包含水面、树叶、玻璃等，需要 any-hit shader
- 水面变化只 rebuild semi-transparent BLAS，不影响 opaque BLAS
- TLAS instance 数增加（含 semi-transparent 的 chunk 多 1 个 instance），通过 mask 区分

### Q2: Semi-Transparent 排序策略 ✅ 已决策

采用分层排序：
- **第一级**：per-subchunk 排序（subchunk center 到相机距离，back-to-front）
- **第二级**：subchunk 内部按面朝向分组（背向相机的先渲染）
- **不做 per-pixel 排序**：初期不用 depth peeling，多层重叠的 artifact 可以接受
