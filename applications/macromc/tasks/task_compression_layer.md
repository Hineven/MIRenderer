# Task: GigaVoxel 压缩/解压抽象层

**Status**: Planned
**Priority**: P0 (Streaming Pipeline 前置)
**Depends on**: 无
**Estimated Effort**: Small (框架) + Medium (各 codec 实现)

---

## 1. 背景

GigaVoxel streaming pipeline 在多处需要压缩/解压：

| 数据类型 | 压缩方向 | 场景 | 性能要求 |
|----------|---------|------|---------|
| Chunk 体素数据 | Disk ↔ RAM | 加载/保存 chunk | 解压 < 1ms/chunk |
| LFC visible blocks | Disk ↔ RAM | 加载/保存 LFC | 解压 < 0.5ms/chunk |
| Mesh cache (可选) | Disk ↔ RAM | 二次加载跳过 meshing | 解压 < 2ms/chunk |

当前 infra_impl 层已有 FIO 线程做文件 I/O，但没有专门的压缩抽象。需要一个统一的压缩/解压接口，支持多种 backend，方便后续替换优化。

## 2. 需求

### 2.1 核心接口

```
Compressor:
  - Compress(input, output) → compressed_size
  - Decompress(input, output) → decompressed_size
  - MaxCompressedSize(input_size) → upper bound
  - IsAvailable() → bool (某些 codec 可能不可用)

特性：
  - 无状态（同一实例可并发使用）
  - 线程安全
  - 支持 in-place 解压（output buffer == input buffer，节省内存）
```

### 2.2 需要的 Codec

| Codec | 用途 | 特点 |
|-------|------|------|
| **LZ4** | 通用快速解压 | 解压极快，压缩率一般 |
| **Zstd** | 高压缩率 | 压缩率好，解压也快 |
| **PaletteRLE** | LFC visible blocks | 自定义：palette encoding + delta RLE |
| **VoxelCodec** | Chunk 体素数据 | 自定义：chunk palette + 列方向 RLE |
| **None / Passthrough** | Debug / placeholder | 不压缩，直接 memcpy |

### 2.3 Codec 注册与选择

- 通过 codec id 或 name 选择 codec
- 运行时可查询可用的 codec 列表
- 压缩数据头部包含 codec id，解压时自动选择
- 支持 fallback：如果保存时用的 codec 不可用，报错而非静默失败

## 3. 设计方案

### 3.1 接口定义

放在 `mi/core/include/core/compression.h`：

```
CompressedHeader:
  - codec_id (uint8_t)
  - original_size (uint32_t)
  - compressed_size (uint32_t)

ICompressor (interface):
  - Compress(src, src_size, dst, dst_capacity) → result_size
  - Decompress(src, src_size, dst, dst_capacity) → result_size
  - MaxCompressedSize(src_size) → size_t
  - CodecId() → uint8_t
  - Name() → string_view

CompressionRegistry (singleton):
  - Register(compressor)
  - Get(codec_id) → ICompressor*
  - GetAvailable() → list of codec names
  - Compress(codec_id, src, src_size) → vector<uint8_t> (含 header)
  - Decompress(src_with_header, src_size, dst, dst_capacity) → result_size
```

### 3.2 数据格式

压缩后的数据格式：

```
[CompressedHeader: 9 bytes]
  codec_id:       1 byte
  original_size:  4 bytes (little-endian)
  compressed_size: 4 bytes (little-endian)
[Compressed payload: compressed_size bytes]
```

解压时：
1. 读 header → 获取 codec_id 和 original_size
2. 从 registry 查找 codec
3. 调用 codec 的 Decompress

### 3.3 Codec ID 分配

```
0x00: None (passthrough)
0x01: LZ4
0x02: Zstd
0x10: PaletteRLE (LFC)
0x11: VoxelCodec (chunk voxel)
0x20~0xFF: 自定义 / 未来扩展
```

### 3.4 实现优先级

1. **Phase 1 (placeholder)**: None + LZ4
   - None 用于 debug 和测试
   - LZ4 通过 vcpkg 依赖引入，解压极快，适合初期验证

2. **Phase 2 (优化)**: VoxelCodec + PaletteRLE
   - VoxelCodec：chunk palette + 列方向 RLE，针对体素数据优化
   - PaletteRLE：LFC visible block 专用，palette + delta encoding

3. **Phase 3 (可选)**: Zstd
   - 更高压缩率，适合 disk 存储优化
   - 解压速度略慢于 LZ4 但仍然很快

## 4. 与现有系统的集成

### 4.1 FIO 层

- Infra 的 FIO 线程读取压缩文件 → 返回带 header 的 raw bytes
- Streaming pipeline 的 decompress job 调用 CompressionRegistry::Decompress
- 保存时反向：compress → write to disk

### 4.2 Streaming Pipeline

```
加载路径:
  FIO::ReadAsync(file_path) → compressed_bytes (with header)
  → Decompress task (worker thread) → raw_bytes
  → 后续处理（voxel parsing / LFC parsing）

保存路径:
  raw_bytes → Compress(codec_id) → compressed_bytes
  → FIO::WriteAsync(file_path, compressed_bytes)
```

### 4.3 与 Chunk 格式的关系

Chunk 文件 = ChunkHeader + CompressedVoxelData + CompressedLFCDatadata（各部分独立压缩）
- 每部分带独立的 CompressedHeader
- 可以各部分用不同 codec（如 voxel 用 VoxelCodec，LFC 用 PaletteRLE）

## 5. 验收标准

1. 接口定义完整，可编译
2. None codec 实现 + 单元测试（roundtrip: compress → decompress == original）
3. LZ4 codec 实现 + 单元测试
4. CompressionRegistry 注册/查找 + 单元测试
5. 性能基准：LZ4 解压 4KB chunk < 0.1ms
6. 线程安全：多线程并发 compress/decompress 无 data race

## 6. 影响范围

- `mi/core/include/core/compression.h` — 接口定义
- `mi/core/compression/` — 各 codec 实现
- `mi/core/CMakeLists.txt` — 新增源文件 + LZ4 依赖
- `tests/core/test_compression.cpp` — 单元测试
- `applications/macromc/` — streaming pipeline 使用（后续）

## 7. 依赖

- **LZ4**: 通过 vcpkg 安装 (`vcpkg install lz4`)
- **Zstd**: 通过 vcpkg 安装 (`vcpkg install zstd`)（Phase 3）
