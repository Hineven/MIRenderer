# MIRenderer - AI Agent Guide

## Project Overview

MIRenderer (米渲染器) is a research-oriented real-time global illumination (RTGI) renderer and rendering framework written in modern C++26. It is designed for real-time lighting research and implements various advanced rendering techniques including hardware ray tracing, volumetric rendering, and Gaussian radiance fields.

The project uses Chinese & English as primary documentation languages. All comments within essential directories (mi) should be written in English. Other documentation should be written in Chinese where appropriate.

## Technology Stack

- **Language**: C++26 (requires GCC 14+ or MSVC)
- **Build System**: CMake 3.27+
- **External Dependency**: vcpkg(major) + submodules(assist) 
- **Graphics API**: Vulkan 1.4.300+
- **Shader Language**: HLSL (compiled to SPIR-V via DXC)
- **GPU Requirements**: NVIDIA RTX cards only
- **License**: The Unlicense (public domain)

## Project Structure

```
MIRendererDev/
├── mi/                          # Core renderer libraries
│   ├── core/                    # Core library: types, task system, memory management
│   ├── rhi/                     # Rendering Hardware Interface (Vulkan backend)
│   ├── rdg/                     # Render Graph framework
│   ├── renderer/                # Renderer implementation
│   └── util/                    # Utilities: model loaders, texture loaders
├── infra_impl/                  # Infrastructure abstraction implementation
├── applications/                # Executable applications
│   ├── 3d_viewer/              # Main 3D viewer application
│   ├── micromc/                # Minecraft chunk renderer toy
│   └── min_capture_test/       # Minimal capture test
├── tests/                       # Unit tests (Google Test)
├── external/                    # Git submodules
│   └── VulkanMemoryAllocator-Hpp/
├── cmake/                       # CMake modules
└── cmake-build-*/              # Build directories (Debug, Release, etc.)
```

## Module Descriptions

### 1. Core (`mi/core`)

Foundation layer providing:
- Basic types and platform abstractions (`types.h`, `platform.h`)
- Reference counting system (`refcounted.h`, `TRef<>`)
- Task system for multi-threading (`task.h`, `task.cpp`)
- Lock-free queues and allocators (`util/queue.h`, `util/slot_allocator.h`)
- Debug profiling utilities (`util/debug_prof.h`)

Key classes:
- `RefCounted<>`: Base class for reference-counted objects
- `NonCopyable` / `NonMovable`: Base classes for restricting copy/move semantics
- `TRef<T>`: Smart pointer for reference-counted objects

### 2. RHI (`mi/rhi`) - Rendering Hardware Interface

Abstracts graphics API interactions:
- **Currently only Vulkan backend is supported**
- Thread-safe resource creation
- Command buffer recording and submission
- Bindless resource management
- Hardware ray tracing support (acceleration structures, ray tracing pipelines)

Key concepts:
- **RHI Thread**: Single dedicated thread for all graphics API calls
- **Render Thread**: Single thread that records rendering commands and interacts with RHI
- **Frame Index**: Monotonically increasing counter for frame-based resource management

Key classes:
- `RHI`: Singleton graphics API interface
- `RHITexture` / `RHIBuffer`: GPU resource abstractions
- `RHIShader` / `RHIGraphicsPipeline` / `RHIComputePipeline` / `RHIRayTracingPipeline`: Pipeline objects
- `RHICommandQueueGraphics`: Graphics command submission

### 3. RDG (`mi/rdg`) - Render Dependency Graph

High-level rendering pass organization:
- Automatic resource lifetime management
- Pass dependency tracking and scheduling
- Uniform buffer management
- Profiling timestamp queries

Key classes:
- `RenderGraph`: Encapsulates a frame's rendering passes
- `RenderGraphBuilder`: Builder pattern for constructing render graphs
- `RDGPass`: Base class for render passes
- `RDGTexture` / `RDGBuffer`: Graph-managed resource handles

### 4. Renderer (`mi/renderer`)

The actual rendering implementation featuring:
- Deferred shading for static meshes
- Hardware ray traced shadows, reflections, GI
- Volumetric lighting (volume primitives and OpenVDB grids)
- Gaussian radiance field (3D Gaussian Splatting) rendering
- Temporal anti-aliasing (TAA)
- Various denoising techniques

Key classes:
- `Renderer`: Main renderer singleton
- `RendererView`: Per-view rendering state (camera, viewport)
- `Scene`: Scene management
- `StaticMesh` / `Material` / `Renderable`: Scene objects
- `DeviceBindlessResourceAllocator`: GPU resource allocation

### 5. Util (`mi/util`)

Asset loading utilities:
- `GLTFLoader`: glTF 2.0 model loading
- `TextureLoader`: Various image formats
- `OpenVDBLoader`: Volumetric data loading
- `GaussianRadianceFieldLoader`: 3D Gaussian Splatting data loading

### 6. Infra Impl (`infra_impl`)

Platform abstraction implementation:
- File I/O (dedicated FIO thread for async operations)
- HLSL shader compilation (using DXC)
- Resource path resolution
- Logging and profiling hooks

## Build Instructions

### Prerequisites

1. Install Vulkan SDK 1.4.300+ (https://vulkan.lunarg.com/)
2. Install vcpkg and integrate with CMake
3. Install dependencies via vcpkg:
```bash
vcpkg install glm imgui[glfw-binding] stb gtest cpptrace xxhash cgltf happly nlohmann-json glfw3 openvdb tinyexr vulkan-memory-allocator directx-dxc argparse cppzmq
```

### Build Commands

```bash
# Configure (Debug)
cmake -B cmake-build-debug -S . -DCMAKE_BUILD_TYPE=Debug

# Configure (Release)
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release

# Configure (RelWithDebInfo - recommended for development)
cmake -B cmake-build-relwithdebinfo -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Build
cmake --build cmake-build-relwithdebinfo --target 3d_viewer

# Run tests
ctest --test-dir cmake-build-relwithdebinfo
```

### CMake Options

- `MI_USE_VCPKG_VMA_HPP`: Use VMA-HPP from vcpkg instead of submodule (default: OFF)
- `MI_VOLUME_PRIMITIVES_ENABLE_FOURIER_DEFAULT`: Enable Fourier primitives by default (default: OFF)
- `MI_ENABLE_TIMESTAMP`: Enable GPU timestamp queries (default: ON)
- `MI_ENABLE_SHADER_DEBUGGING`: Enable shader debugging features (default: ON)
- `MI_ENABLE_RHI_OBJECT_NAMING`: Enable human-readable RHI object names (default: ON)
- `MI_ENABLE_COMPLIER_ADDRESS_SANITIZER`: Enable address sanitizer (default: OFF)
- `MI_BYPASS_RHI_THREAD`: Merge RHI and render threads for debugging (default: OFF)

## Code Style Guidelines

### Naming Conventions

- **Classes**: `PascalCase` (e.g., `RenderGraph`, `RHIResource`)
- **Methods**: `PascalCase` (e.g., `CreateBuffer`, `AdvanceFrame`)
- **Variables**: `snake_case` (e.g., `frame_index`, `swapchain_size`)
- **Member variables**: Trailing underscore (e.g., `frame_index_`, `bindless_manager_`)
- **Macros**: `SCREAMING_SNAKE_CASE` with `MI_` prefix (e.g., `MI_NAMESPACE_BEGIN`)
- **Template parameters**: `PascalCase` with prefix `T` (e.g., `TRef`, `TLockFreeQueue`)

### Namespaces

All code must be within the `mi` namespace:

```cpp
#include "core/common.h"
MI_NAMESPACE_BEGIN

// Your code here

MI_NAMESPACE_END
```

### Reference Counting

Use `TRef<T>` for reference-counted pointers:

```cpp
TRef<RHITexture> my_texture;
TRef<RDGBuffer> my_buffer;
```

### Flags

Use the `MAKE_FLAGS` macro to create type-safe flag enums:

```cpp
enum class MyFlagBits : unsigned {
    eOptionA = 1 << 0,
    eOptionB = 1 << 1,
};
MAKE_FLAGS(MyFlag);

// Usage
MyFlags flags = MyFlagBits::eOptionA | MyFlagBits::eOptionB;
```

### File Headers

All source files must include this header comment:

```cpp
/*
 * Created: YYYY/MM/DD
 * Author:  hineven
 * See LICENSE for licensing.
 */
```

### Force Inline

Use `FORCEINLINE` macro for performance-critical inline functions (works across MSVC and GCC).

## Threading Model

The renderer uses a multi-threaded architecture with strict thread roles:

### Thread Categories

1. **RHI Thread** (1 thread): 
   - **ONLY** thread allowed to call graphics APIs
   - Processes command queues submitted by render thread

2. **Render Thread** (1 thread):
   - **ONLY** thread allowed to interact with RHI layer
   - Records rendering commands
   - Manages scene traversal and culling
   - Builds render graphs

3. **FIO Thread** (1 thread):
   - Dedicated for file I/O operations
   - Processes async read/write requests

4. **Worker Threads** (multiple):
   - Host-side computation only
   - **MUST NOT** access RHI resources directly
   - **MUST NOT** call device update methods

5. **Main Thread**:
   - Application logic
   - Window message handling

### Important Rules

- Never access RHI resources from worker/main threads
- Use `TRef<>` to extend resource lifetime when needed
- For debugging, enable `MI_BYPASS_RHI_THREAD` to merge RHI and render threads

## Resource Management

### Resource Hierarchy

1. **RHI Resources**: Direct GPU resources (`RHITexture`, `RHIBuffer`)
   - Managed by RHI layer
   - Automatic delayed release (1+ frames)

2. **Host Resources - RHI SubAllocation**: Share part of an RHI resource
   - Examples: `DeviceUberBufferAllocation`, various `SlotKeeper` classes
   - Released via `DeviceBindlessResourceAllocator`

3. **Host Resources - Pure Host**: CPU-side resources
   - Examples: `Material`, `DeviceGeometry`
   - Can be released immediately

### Ownership

- Use `TRef<>` to keep resources alive
- Unreferenced resources are automatically released at appropriate times
- RHI resources use reference counting with automatic delayed destruction

## Shader Development

### Shader Location

Shaders are in `mi/*/shaders/` directories:
- `mi/renderer/shaders/`: Main renderer shaders
- `mi/rdg/shaders/`: Render graph utilities
- `mi/util/shaders/`: Utility shaders

### Shader Language

- Write shaders in **HLSL**
- Shaders are compiled to SPIR-V at runtime using DXC
- Include shared headers from `shared/` directories

### Debugging Shaders

1. Enable `MI_ENABLE_SHADER_DEBUGGING` CMake option
2. Use `printf()` in shaders for debug output
3. Enable Vulkan Validation Layer with "Debug Printf" option
4. Use NSight Graphics for frame capture and inspection

## Testing

Tests are located in `tests/` using Google Test:

```bash
# Build and run all tests
cmake --build cmake-build-relwithdebinfo --target RUN_TESTS

# Run specific test
./cmake-build-relwithdebinfo/tests/core_task_test
```

Test categories:
- `tests/core/`: Core functionality tests
- `tests/rhi/`: RHI tests (include bindless and ray tracing)
- `tests/rdg/`: Render graph tests
- `tests/util/`: Utility tests
- `tests/infra/`: Infrastructure tests

## Debugging Tips

### Recommended Debug Configuration

Use `RelWithDebInfo` build type for daily debugging - it has optimizations enabled but with debug symbols.

### Troubleshooting

1. **Validation Errors**: Use Vulkan Configurator to enable Validation Layers
2. **Hard Crashes**: 
   - Enable `MI_BYPASS_RHI_THREAD` for single-thread debugging
   - Use "Break on Validation Error" in Vulkan Configurator
3. **Memory Issues**: 
   - Use `heob` on Windows for memory leak detection
   - Or use Visual Studio's built-in memory tools
4. **Performance Analysis**:
   - Use `debug_prof.h` macros for CPU profiling
   - Enable `MI_ENABLE_TIMESTAMP` for GPU profiling
   - Use NSight Graphics for GPU profiling

## Key Targets

- `3d_viewer`: Main 3D model viewer application
- `core`: Core library
- `rhi`: RHI abstraction layer
- `rdg`: Render graph framework
- `renderer`: Renderer implementation
- `util`: Utility library
- `micromc`: Minecraft chunk renderer toy

## External Resources

- Resources are automatically copied to build directory via `add_resources()` CMake function
- Runtime resource path resolution is handled by Infra layer
- In Debug/RelWithDebInfo builds, `MI_PROJECT_ROOT` macro points to source directory

## Agent Collaboration Notes

These notes are intentionally brief and supplementary. They should not override the architectural guidance above.

### Prefer Reading Real Implementations

- Do not rely on this document alone when modifying the renderer. Read the relevant `.cpp` / `.hlsl` implementation files first.
- Treat comments and high-level descriptions as guidance, but use the current code as the source of truth.

### High-Risk Areas

- Changes under `mi/rhi/`, `mi/rdg/`, `mi/renderer/renderer/`, and `mi/renderer/shaders/resources/` are high risk.
- Before editing these areas, inspect the related headers, call sites, and resource bindings instead of making isolated local changes.
- Prefer small, surgical changes over broad refactors in these areas unless explicitly requested.

### Shader Change Checklist

- When modifying shaders, also check shared structs, optional macros, resource declarations, and CPU-side parameter filling code.
- For renderer shaders, verify the corresponding `RDGShader` parameter structs and render pass setup code.
- If a shader uses history buffers, bindless resources, or packed encodings, preserve layout compatibility unless the user explicitly wants a format change.

### Renderer Feature Integration

- New rendering features usually require coordinated updates across `Renderer::Render()`, `RendererView` persistent state, RDG pass setup, shader parameters, and debug / console exposure.
- Prefer tracing the full feature path before editing only one stage.

### Naming And Historical Inconsistencies

- Some names and spellings may reflect historical usage already referenced across the codebase.
- Do not perform unrelated renames, spelling cleanups, or path reshuffles unless explicitly requested.

### Testing Priorities

- For algorithmic or data-structure changes, prefer adding or updating focused tests under `tests/renderer/` or `tests/core/` when possible.
- For RHI / RDG changes, validate with the smallest relevant target first before assuming `3d_viewer` coverage is sufficient.
