# Mesh Light Sampling TODO

## 背景

在直接光照路径中，`MeshLightHierarchy -> LightGrid -> RIS` 已经完成了从 `triangle -> grid -> RIS` 到 `cluster -> grid -> RIS` 的第一轮迁移，并且已经修复了一批明显问题，包括：

- `active grid flag` / `grid pressure` 的 persistent state 维护与 reset；
- `ClipActiveGrids` 的接线；
- `MeshLightInstanceClusterHeader::WeightedNormal` 的错误表示方式；
- cluster 注入估计中 `WeightedNormalVariance` 未归一化、近场方向项过强等问题；
- cluster 下采样时遗漏 tree traversal pdf 的问题。

这些修复后，结果已经明显改善，但我们在调试一个由大量三角形组成的单个 emissive icosphere 时，仍然观察到少量稳定的 grid seam：相邻 grid 的边界处会出现轻微但可见的亮度跳变。


## 这次讨论得出的关键结论

### 1. 现有流程本质上包含三个阶段

为了便于后续讨论，统一使用下面的记号：

- 过程 A：对候选 light elements 做 Bernoulli thinning；
- 过程 B：从剩余 elements 中继续构造 retained subset；
- 过程 C：在 retained subset 上运行当前的 light-level RIS，并最终生成真实光采样。

也就是说，当前 mesh light 的局部采样技术并不是一个单层 proposal，而是 `A + B + C` 组合出来的结果。


### 2. 仅用 `P(L in grid)` 不能恢复最终 proposal

这次讨论的一个核心结论是：

- 虽然可以定义“某个光源 `L` 落入当前 grid retained subset 的概率”；
- 但这个量只是 A/B 阶段的边际包含概率；
- 它并不等于最终采样过程选到该光源样本的概率。

原因在于：

- 过程 C 的选取概率依赖整个 retained subset，而不是只依赖单个光源；
- 不同的 A/B 结果之间在最终样本空间上存在重叠支持；
- 同一个最终 mesh light sample，可能由多个不同的 retained subset 路径生成。

因此，不能简单把 `P(L in grid)` 直接当成一个“大 proposal”的 pdf 组成部分。  
这也是为什么当前局部技术如果想单独写出严格的最终 proposal，会变得非常复杂。


### 3. 真正合理的改进方向是引入完整 support 的 global proposal

我们已经基本确认，未来更合理的长期方案不是继续硬推 `P(L in grid)`，而是：

- 保留 light grid 这一条局部采样技术；
- 再引入一条拥有完整 support 的 global mesh-light proposal；
- 在更高层将两条技术统一起来，恢复无偏性。

讨论过程中，最自然的 global 方案有两个方向：

- 基于 mesh light root 的全局 proposal；
- 基于 BSDF 的全局 proposal。

最终的判断是：

- BSDF 方案虽然生成成本低，但 `q_grid(y)` 的 cross pdf 很难高效计算，不适合作为当前阶段的主要方向；
- 基于 mesh light root 的 global 方案更贴近现有数据结构，也更容易落地。


### 4. 如果继续保留过程 A，那么最终应把问题看成“多技术候选合并”

后续更成熟的方案，不应在“两个最终 sample”之间做 MIS，而应在 candidate 层合并两条技术：

- light grid 提供局部 candidates；
- global proposal 提供完整 support 的 candidates；
- 两者共同进入统一的 reservoir / RIS；
- 最终只保留一个样本，并只发一次真正的阴影测试。

这个思路有几个好处：

- 不需要显式构造 “final RIS output distribution” 的 pdf；
- 不需要对两个最终 sample 做 pairwise MIS；
- 不会自然退化成“两条 shadow ray”；
- 能更好地复用现有 `SampleOneLightSample_RIS` 的结构。


## 当前阶段的决定

### 暂时删去过程 A（Bernoulli thinning）

这是当前最务实的决定。

原因不是过程 A 的想法本身不好，而是：

- 一旦保留过程 A，整个局部技术就不再是一个容易写出 pdf 的 proposal；
- 再叠加 tree subdivision 后，global 技术与 subset 技术之间的 cross pdf 会进一步复杂化；
- 尤其当 global 样本落在一个当前 subset cluster 的父节点 / 祖先节点对应的子树上时，两条技术不在同一层级，cross pdf 很难写得清楚且高效。

换句话说，过程 A 不是“错误”，但它会显著抬高当前这一版算法的理论和实现复杂度。  
在全局 proposal 还没有成型之前，先去掉过程 A，更有利于把 mesh light 采样重新整理到一个可验证、可继续演进的状态。


## 未来推荐的改进路线

下面是下次继续推进时，比较推荐的顺序。

### 阶段 1：先把当前局部技术整理干净

目标：

- 去掉过程 A 后，重新检查 retained subset 与现有 RIS 的一致性；
- 确保局部技术本身足够稳定，不再引入明显的系统性 bias；
- 继续观察 grid seam 是否进一步减弱。

建议重点检查：

- `SampleOneLightSample_RIS` 中 light-level proposal 的定义是否与实际 `Sample.Pdf` 一致；
- 去掉过程 A 后，过程 B 的 retained subset correction 是否还能简化；
- tree subdivision 与 retained subset 的层级选择是否仍然会导致明显的 per-grid proposal 差异。


### 阶段 2：实现一个简单但完整的 global mesh-light proposal

第一版 global proposal 不必追求最优，只要满足：

- 完整 support；
- pdf 清晰；
- 便于和现有 RIS 结构整合。

推荐的第一版做法：

- 在 CPU 侧基于 active mesh light instance 构建 root-level 的离散分布；
- 优先使用 root intensity 的 prefix CDF；
- shader 侧通过二分选出一个 global MLI root；
- 再沿现有 hierarchy 继续向下采样到 triangle，并复用现有 area-light sampling 逻辑。

如果需要更稳一点，可以使用一个简单混合分布：

`p_root(i) = alpha / N + (1 - alpha) * w_i / SumW`

这样既保留 uniform floor，又保留 intensity guidance。


### 阶段 3：在 candidate 层融合 local 与 global 技术

这是长期正确方向。

目标不是：

- 让 local sampler 和 global sampler 各自产出一个最终 sample，再做 pairwise MIS；

而是：

- 让 local 技术和 global 技术都产出 light-level candidates；
- 将这些 candidates 放进同一个 reservoir / RIS；
- 最终只保留一个样本。

这样做可以避免：

- 计算最终 RIS 输出分布的 pdf；
- 为两个最终 sample 分别付出可见性成本；
- 在最终 sample 层面处理极其复杂的 cross pdf。


## 实现时需要特别注意的点

### 1. 不要把“global sample”放得太晚

如果 global 技术直接输出最终 area-light point sample，那么它与当前 local RIS 的 sample space 会很难对齐。  
更合理的做法，是在 `SampleOneLightSample_RIS` 的第一层，也就是 light-level proposal 层面引入 global candidates。


### 2. tree subdivision 会让 cross pdf 变得比看上去更复杂

一个 global root 样本，对应的是真正的 root cluster；
而当前 grid subset 里的 retained element，可能只是该 root 子树中的某个中间 cluster，甚至多个 cluster 的并列前沿。

因此：

- “global 样本是否属于当前 subset”
- “subset 技术生成该样本的概率是多少”

都不是一个常数时间即可写清楚的问题。  
这是本次决定暂时移除过程 A 的一个重要原因。


### 3. 先把算法讲清楚，再追求复杂优化

这次调试已经证明，mesh light 采样一旦进入“多阶段 subset + hierarchy + RIS”的组合，任何一个看似小的 pdf 漏项，都会在图像上表现为稳定的结构性错误。  
因此后续推进时，优先级应当是：

1. proposal 的定义是否清楚；
2. sample 的生成路径是否唯一或是否已正确按 mixture 处理；
3. pdf 是否与实际 sample 过程一致；
4. 最后才是 aggressive culling / thinning / 更复杂的 guiding。


## 简短结语

这次讨论虽然没有直接把最终方案一次做完，但已经把问题的结构想清楚了：

- 当前残留问题，不只是一个局部公式错误；
- 更深层的原因是 mesh light local proposal 已经演化成了一个多阶段采样过程；
- 如果继续扩展，就必须把它放到更统一的采样框架下思考。

当前先删去过程 A，是一个合理且清醒的收缩动作。  
等局部方案重新稳定后，再按“global root proposal + candidate-level unified RIS”的方向继续推进，会更稳，也更容易做对。
