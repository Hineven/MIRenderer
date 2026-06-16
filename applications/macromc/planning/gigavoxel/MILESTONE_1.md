# Milestone 1: GigaVoxel Visibility + TLAS/BLAS Working

## 目标

验证 GigaVoxel 核心管线端到端跑通：
- Chunk 数据从 disk 加载 → mesh bake → BLAS build → PTLAS update → visibility buffer 成功渲染
- VC + TFC + LFC 三级 LOD 均可见
- TLAS/BLAS 正确维护，射线可 hit（不要求完整 RTGI，只需 ray query 能返回正确 hit）

## 前置基础设施（先做）

| # | Task | 内容 | 状态 |
|---|------|------|------|
| F1 | [task_job_cancel](../tasks/task_job_cancel.md) | TaskGraph 取消支持 | Planned |
| F2 | [task_compression_layer](../tasks/task_compression_layer.md) | 压缩/解压抽象层 (None + LZ4) | Planned |

## 核心 Task（按依赖顺序）

| # | Task | 内容 | 依赖 |
|---|------|------|------|
| T1 | **Chunk Data + Worldgen** | 扩展 macromc world，支持 4096 高度 + SubChunk + palette 压缩 | 无 |
| T2 | **Greedy Meshing** | VC greedy meshing（含 UV scale）+ TFC baked mesh + LFC cube mesh | T1 |
| T3 | **Mesh Upload + BLAS Build** | CPU mesh → staging → GPU vertex/index buffer → BLAS build | T2, F3 |
| T4 | [task_ptlas_support](../tasks/task_ptlas_support.md) | PTLAS RHI + RDG + GigaVoxelTLASManager | 无 |
| T5 | **Streaming Pipeline 骨架** | ChunkRegistry + 加载请求队列 + job DAG 编排 | F1, F2, F3 |
| T6 | [task_job_priority](../tasks/task_job_priority.md) | 连续优先级 + 动态更新 | Planned |
| T7 | **Culling + Visibility Raster** | LOD refinement + frustum cull + HW raster (VC/TFC) + SW raster (LFC) → visibility buffer | T3, T4, T5 |
| T8 | **Visibility Resolve** | visibility buffer → G-Buffer（仅 opaque，不含 semi-transparent） | T6 |
| T9 | **Ray Query 验证** | 从 G-Buffer 发射简单 ray query → PTLAS → 验证 hit 正确性 | T4, T6 |

## 依赖图

```
F1 ─┐
F2 ─┼→ T5 (Streaming)
F3 ─┘         │
T1 → T2 → T3 ─┤
              ├→ T6 (Culling+Raster) → T7 (Resolve)
T4 (PTLAS) ──┘                        │
                                      └→ T8 (Ray Query 验证)
```

## 不在本 Milestone 范围内

- ❌ Semi-transparent 渲染（水面/树叶当 opaque 处理）
- ❌ Hi-Z occlusion culling（仅 frustum cull）
- ❌ DLSS / Motion Vector
- ❌ Floating Origin（限制视距到 float32 安全范围）
- ❌ LOD 转换平滑
- ❌ Disk cache（mesh/card 缓存）
- ❌ 完整 RTGI（仅验证 ray query 能 hit）

## 完成标准

1. 进入世界，看到 VC/TFC/LFC 渲染的体素地形（visibility buffer 正确生成）
2. 移动相机，chunk 正确加载/卸载（streaming pipeline 工作）
3. LOD 正确切换（近 VC → 中 TFC → 远 LFC）
4. 发射 ray query 命中 BLAS，返回正确的 chunk_id + block_id
