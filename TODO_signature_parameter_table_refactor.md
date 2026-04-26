# Signature Parameter Table 重构

## 问题

当前 RDG 层每次 dispatch 都要：
1. 调用 `SetupShaderParams` 遍历整个参数结构体，构建 `RHIBindPipelineParametersDesc`
2. 传入 RHI 层 `BindPipelineParameters` 命令
3. RHI 线程中 `ParameterTable::Merge` 逐字段比较脏标记
4. `FlushBindPointState` 中用 Zobrist hash 做缓存查找

即使同一份 params 在同一帧内被多个 shader 使用（如 DiffuseIndirectLighting 的 ~25 个 compute shader 共享 CommonViewParams 等），每次都要重复走上述全流程。

RHI 层的 hash/缓存代码复杂且缺少 RDG 层的上下文信息。

## 目标

1. 将 RDGShaderParam 中 **vertex input / render target**（与 root signature / descriptor set 无关的部分）分离出来
2. 替换 RHI 层 `BindPipelineParameters` 为两个命令：`CreateSignatureParameterTable` + `BindSignatureParameterTable`
3. RDG 层可基于 `(shader, params_ptr)` 做帧内去重，避免重复 `SetupShaderParams`
4. 将 binding remapping 从 per-pipeline 收归 per-signature

## 设计决策（已确认）

破坏性重构rhi层关于bindPipelineParameters的接口，更符合目前signature - pipeline抽象结构和上层调用需求。

- **table_id**：由调用方（RDG 层）分配的 uint32_t，帧局部有效，作为命令参数传入 RHI 层
- **slot_table_**：RHI 层帧局部哈希表，`table_id → vk::DescriptorSet`，帧结束随 descriptor pool reset 清空
- **Push constants**：预留接口暂不实现（当前未使用 push constant）
- **Partial update**：不再支持（当前代码中也没用到 `Merge` 的增量语义）
- **去重**：RDG 层不做隐式去重，由上层调用方通过复用 `table_id` 自行控制
- **Remapping**：从 per-pipeline（`VulkanPipelineBindingRemappings`）改为 per-signature，同一 signature 下所有 pipeline 共享

---

## Step 0: 分离 RDGShaderParam 中的非 signature 部分

**目标**：将 `RDGShaderParamStructAndSizeInfo` 中与 root signature 无关的字段分离到独立结构体。

**现状**（`rdg_param.h:106-120`）：
```cpp
struct RDGShaderParamStructAndSizeInfo: public RDGShaderParamStructInfo {
    // Root signature 相关（保留）
    std::span<RDGShaderParameterLocation> storage_buffers_;
    std::span<RDGShaderParameterLocation> uniform_buffers_;
    std::span<RDGShaderParameterLocation> uavs_;
    std::span<RDGShaderParameterLocation> srvs_;
    std::span<RDGShaderParameterLocation> samplers_;
    std::span<RDGShaderParameterLocation> acceleration_structures_;
    // 非 root signature 相关（需分离）
    std::span<RDGShaderParameterLocation> vertex_buffers_;
    std::span<RDGShaderParameterLocation> vertex_attributes_;
    RDGShaderParameterLocation index_buffer_;
    std::span<RDGShaderParameterLocation> render_targets_;
    RDGShaderParameterLocation dispatch_command_; // 这个东西目前根本没用，是legacy代码，可以直接移除
};
```

**改动**：

引入 `RDGShaderRenderPassInfo` 承载 vertex input / render target 信息：

```cpp
// Root signature 相关参数（descriptor set 绑定用）
struct RDGShaderSignatureParamInfo : public RDGShaderParamStructInfo {
    std::span<RDGShaderParameterLocation> storage_buffers_;
    std::span<RDGShaderParameterLocation> uniform_buffers_;
    std::span<RDGShaderParameterLocation> uavs_;
    std::span<RDGShaderParameterLocation> srvs_;
    std::span<RDGShaderParameterLocation> samplers_;
    std::span<RDGShaderParameterLocation> acceleration_structures_;
};

// 非 root signature 部分（vertex input / render target / dispatch command）
struct RDGShaderRenderPassInfo {
    std::span<RDGShaderParameterLocation> vertex_buffers_;
    std::span<RDGShaderParameterLocation> vertex_attributes_;
    RDGShaderParameterLocation index_buffer_;
    std::span<RDGShaderParameterLocation> render_targets_;
    RDGShaderParameterLocation dispatch_command_;
};

// 向后兼容：继承两者
struct RDGShaderParamStructAndSizeInfo : public RDGShaderSignatureParamInfo {
    RDGShaderRenderPassInfo render_pass_info_;
};
```

**涉及文件**：
- `mi/rdg/include/rdg/rdg_param.h` — 新增结构体，调整 `RDGShaderParamStructAndSizeInfo`
- `mi/rdg/rdg_param.cpp` — `zzFinalizeTopLevelParamsStructInfo` 中分离填充逻辑

---

## Step 1: RHI 层 — 替换 BindPipelineParameters 为 Create/Bind 命令对

**目标**：替换 `RHICommandBindPipelineParameters` 为两个新命令。

### 1a. 新 RHI 命令定义

替换（`rhi_cmd.h`）：

```cpp
// 旧：
class RHICommandBindPipelineParameters : public TRHICommand<RHICommandBindPipelineParameters> { ... };

// 新：
class RHICommandCreateSignatureParameterTable : public TRHICommand<RHICommandCreateSignatureParameterTable> {
public:
    RHICommandCreateSignatureParameterTable(
        uint32_t table_id,
        RHIPipelineRootSignature * root_signature,
        RHIBindPipelineParametersDesc desc)
        : table_id_(table_id), root_signature_(root_signature), desc_(desc) {}
    void Execute(RHICommandQueueBase & cmd) override;
    uint32_t table_id_;
    RHIPipelineRootSignature * root_signature_;
    RHIBindPipelineParametersDesc desc_;
};

class RHICommandBindSignatureParameterTable : public TRHICommand<RHICommandBindSignatureParameterTable> {
public:
    RHICommandBindSignatureParameterTable(
        uint32_t table_id,
        RHIBindPointType point,
        std::span<const std::byte> constants = {})
        : table_id_(table_id), point_(point), constants_(constants) {}
    void Execute(RHICommandQueueBase & cmd) override;
    uint32_t table_id_;
    RHIBindPointType point_;
    std::span<const std::byte> constants_;
};
```

### 1b. RHICommandQueueGraphics 接口变更

```cpp
// 旧：
void BindPipelineParameters(RHIBindPointType point, RHIBindPipelineParametersDesc table);

// 新：
void CreateSignatureParameterTable(
    uint32_t table_id,
    RHIPipelineRootSignature * root_signature,
    RHIBindPipelineParametersDesc desc);
void BindSignatureParameterTable(
    uint32_t table_id,
    RHIBindPointType point,
    std::span<const std::byte> constants = {});
```

### 1c. VulkanCommandExecutor 新的执行逻辑

**CreateSignatureParameterTable**（在 `vk_cmd_exec.cpp`）：
```
1. 用 root_signature 的 descriptor_set_layout_ 分配一个 descriptor set
2. 用 root_signature 的 remappings 做 binding 映射
3. 调用 vkUpdateDescriptorSets 写入所有 binding
4. 存入 slot_table_[table_id] = descriptor_set
```

**BindSignatureParameterTable**：
```
1. 从 slot_table_[table_id] 取出 descriptor set
2. 如果 bound_pipeline_dirty，先 bind pipeline
3. bindDescriptorSets(slot_table_[table_id] + bindless_set)
4. 如果有 constants，pushConstants
```

**slot_table_** 在 `CommandQueueState` 中：
```cpp
std::unordered_map<uint32_t, vk::DescriptorSet> slot_table_;
```
帧结束在 `Clear()` 中清空。

### 1d. 删除的代码

- `RHICommandBindPipelineParameters` 命令类（`rhi_cmd.h`）
- `RHIBindPipelineParameters` 队列方法（`rhi_cmd.h`）
- `RHIBindPipelineParameters` 执行器（`vk_cmd_exec.cpp`）
- `ParameterTable` 及其 `Merge` 方法（`vk_cmd_exec.h:137-148`, `vk_cmd_exec.cpp:729-775`）
- `CompileShaderDescriptorWrites`（`vk_cmd_exec.cpp:777-940`）— 逻辑移入 CreateSignatureParameterTable
- `FlushBindPointState` 中的 Zobrist hash 计算（`vk_cmd_exec.cpp:998-1025`）
- `descriptor_cache_` 及其相关类型（`vk_cmd_exec.h:150-164`）
- `bound_descriptor_dirty` 脏标记
- `RHIClearBoundState` 命令（如确认不再需要）

**涉及文件**：
- `mi/rhi/include/rhi/rhi_cmd.h` — 删除旧命令，新增两个命令，修改队列接口
- `mi/rhi/vk/vk_cmd_exec.h` — 删除 ParameterTable / DescriptorSetCacheKey 等，新增 slot_table_
- `mi/rhi/vk/vk_cmd_exec.cpp` — 重写 FlushBindPointState，新增 Create/Bind 执行逻辑，删除 Merge / CompileShaderDescriptorWrites / Zobrist hash
- `mi/rhi/rhi_cmd_exec.h` — 更新纯虚接口

---

## Step 2: 将 Remapping 收归 Signature

**目标**：`VulkanPipelineBindingRemappings` 从 per-pipeline 改为 per-signature（per-`VulkanRootSignature`）。

**现状**：
- `VulkanPipelineBindingRemappings` 存储在每个 pipeline 子类中（`vk_pipeline.h:67,108,164`）
- 同一 signature 下各 pipeline 各自独立计算 remapping，但实际上 remapping 交集就是 signature 的 remapping

**改动**：

在 `VulkanRootSignature` 中持有 remappings：

```cpp
// vk_root_signature.h
class VulkanRootSignature : public RHIPipelineRootSignature {
    // ...
    VulkanPipelineBindingRemappings remappings_;
public:
    FORCEINLINE const VulkanPipelineBindingRemappings & GetRemappings() const { return remappings_; }
};
```

Pipeline 编译时（`CompileRHI`）：
1. 不再自行计算 remappings
2. 从 `root_signature->GetRemappings()` 获取
3. Pipeline 删除 `remappings_` 成员和 `GetRemappings()` 方法

`RHIPipelineRootSignature` 基类也可暴露通用的 `GetBinding()` helper（已有 `binding_base_` 数组），使 remapping 信息在 RHI 抽象层就可用。

**涉及文件**：
- `mi/rhi/vk/vk_root_signature.h` / `.cpp` — 添加 `remappings_`，在构造时计算
- `mi/rhi/vk/vk_pipeline.h` / `.cpp` — 三种 pipeline 删除 `remappings_`，改用 root_signature 的
- `mi/rhi/include/rhi/rhi_root_signature.h` — 可选：在基类添加 remapping 查询接口

---

## Step 3: RDG 层 — 适配新接口

### 3a. SetupShaderParams 拆分

`SetupShaderParams`（`rdg_cmd.cpp:24-175`）拆为两部分：
- **Signature 部分**：构建 `RHIBindPipelineParametersDesc`（UB/SSBO/UAV/SRV/Sampler/AS）— 用于 `CreateSignatureParameterTable`
- **RenderPass 部分**：处理 vertex buffer / render target / draw state — 作为独立参数传入 graphics bind/draw

### 3b. RDGCommandHelper 接口变更

核心变更：将 implicit 的 params → table_id 映射改为 explicit 的三段式调用。
RDG 层不再负责帧内去重，去重由上层调用方自行控制。

**新接口**：

```cpp
// 显式创建 parameter table，返回 table_id
uint32_t CreateParameterTable(
    RHICommandQueueGraphics & queue,
    RDGPass & pass,
    RDGShader & shader,
    const RDGShaderSignatureParamInfo & info,
    const void * params);

// Compute dispatch：直接传 table_id
void Dispatch(
    RHICommandQueueGraphics & queue,
    RDGPass & pass,
    RDGShader & shader,
    uint32_t table_id,
    uint32_t x, uint32_t y, uint32_t z);

// Graphics draw：传 table_id + render_pass_info（从 params 中分离出的部分）
void Draw(
    RHICommandQueueGraphics & queue,
    RDGPass & pass,
    RDGShader & shader,
    uint32_t table_id,
    const RDGShaderRenderPassInfo & render_pass_info,
    uint32_t vertex_count, uint32_t instance_count,
    uint32_t first_vertex, uint32_t first_instance);

void DrawIndexed(
    RHICommandQueueGraphics & queue,
    RDGPass & pass,
    RDGShader & shader,
    uint32_t table_id,
    const RDGShaderRenderPassInfo & render_pass_info,
    uint32_t index_count, uint32_t instance_count,
    uint32_t first_index, int32_t vertex_offset,
    uint32_t first_instance);

// Ray tracing：传 table_id
void DispatchRays(
    RHICommandQueueGraphics & queue,
    RDGPass & pass,
    RDGShader & shader,
    uint32_t table_id,
    uint32_t width, uint32_t height, uint32_t depth);
```

**调用流程对比**：

```cpp
// 旧：
RDGCommandHelper::Dispatch(queue, pass, shader, params, x, y, z)
  → BindComputeShader(queue, pass, shader, info, params)
    → SetupShaderParams(...)
    → queue.BindPipeline(...)
    → queue.BindPipelineParameters(point, desc)
  → queue.Dispatch(x, y, z)

// 新（三段式，上层自行决定去重策略）：
auto table_id = CreateParameterTable(queue, pass, shader, info, params);
  → SetupShaderParams(...)          // 仅 signature 部分
  → queue.CreateSignatureParameterTable(id, sig, desc)
  → return id

Dispatch(queue, pass, shader, table_id, x, y, z);
  → queue.BindPipeline(...)
  → queue.BindSignatureParameterTable(table_id, point)
  → queue.Dispatch(x, y, z)

// 上层去重示例（由调用方自行控制）：
// 同一 params 复用于多个 shader 时：
auto table_id = CreateParameterTable(queue, pass, shader_a, info_a, shared_params);
Dispatch(queue, pass, shader_a, table_id, ...);
Dispatch(queue, pass, shader_b, table_id, ...);  // 复用同一 table_id（如 signature 兼容）
```

**涉及文件**：
- `mi/rdg/rdg_cmd.cpp` — 重写所有 helper 实现
- `mi/rdg/include/rdg/rdg_cmd.h` — 更新接口

---

## Step 4: 更新所有调用方

由于 Step 3 将 helper 接口改为显式 Create/Dispatch 两步调用，所有调用方需要适配新接口。

**改动模式**：

```cpp
// 旧调用：
helper.Dispatch(queue, pass, shader, &my_params, x, y, z);

// 新调用：
auto table_id = helper.CreateParameterTable(queue, pass, shader, info, &my_params);
helper.Dispatch(queue, pass, shader, table_id, x, y, z);

// Draw 类调用额外传入 render_pass_info：
auto table_id = helper.CreateParameterTable(queue, pass, shader, info, &my_params);
helper.DrawIndexed(queue, pass, shader, table_id, render_pass_info, ...);
```

上层可在此模式下自由实现去重：
```cpp
// 同一 params 多次复用：
auto tid = helper.CreateParameterTable(queue, pass, shader, info, &params);
helper.Dispatch(queue, pass, shader_a, tid, ...);
helper.Dispatch(queue, pass, shader_b, tid, ...); // 复用
```

**需修改的调用方**：
- `mi/renderer/` 下所有 pass lambda 中的 dispatch/draw 调用 — 从单步改为 Create + Dispatch/Draw 两步
- `mi/rdg/rdg_helper.cpp` — SpawnDrawIndirectCommand 等，适配新接口
- `tests/rdg/rdg_test.cpp` — 适配新接口

---

## Step 5: 清理与验证

- [ ] 删除 `ParameterTable::Merge`、`CompileShaderDescriptorWrites`、`DescriptorSetCacheKey` 等废弃代码
- [ ] 删除 `RHICommandClearBoundState`（如确认不再需要）
- [ ] 确认 `FlushBindPointState` 简化后的正确性
- [ ] 编译通过
- [ ] DiffuseIndirectLighting 阶段 ~25 个共享 signature 的 compute shader 验证 descriptor set 复用
- [ ] NSight Graphics 确认 descriptor set 分配数量下降
