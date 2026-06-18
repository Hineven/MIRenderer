/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "viewer_gigavoxel.h"

#include <algorithm>
#include <array>
#include <vector>

#include "renderer/mi_giga_voxel.h"
#include "renderer/mi_renderer.h"
#include "renderer/mi_texture.h"
#include "rhi/rhi.h"

#include "registry/block_registry.h"
#include "world/types.h"
#include "world/world_data.h"
#include "worldgen/simple_terrain_worldgen.h"
#include "meshing/chunk_meshing_context.h"
#include "meshing/types.h"

#include <core/pixel_format.h>
#include <core/task.h>

MI_NAMESPACE_BEGIN

namespace {

// ---- Config for the bring-up terrain --------------------------------------
// A small NxN chunk grid around the origin. Kept tiny so worldgen + meshing
// finish instantly in-process. The Phase-1 GigaVoxel merges all of this into
// one BLAS, so do not crank this up.
constexpr int kBringUpChunkRadius = 1; // (2R+1)^2 chunks => 3x3 with R=1

// Convert one macromc VoxelVertex into the renderer-layer GigaVoxelVertex.
// Field-for-field; the two structs mirror each other by design (see comments in
// SharedGigaVoxel.hlsl and meshing/types.h).
GigaVoxelVertex ToGigaVoxelVertex(const MACROMC_MESHING_NAMESPACE::VoxelVertex & v) {
    GigaVoxelVertex out;
    out.position = v.position;
    out.normal = v.normal;
    out.uv_base = v.uv_base;
    out.uv_scale = v.uv_scale;
    out.texture_index = v.texture_index;
    out._pad = 0;
    return out;
}

// Flatten a whole chunk's VoxelMeshJobResult (256 subchunk meshes) into a single
// vertex/index pair with indices re-based into the merged vertex range.
struct FlattenedChunk {
    std::vector<GigaVoxelVertex> vertices;
    std::vector<uint32_t> indices;
};

FlattenedChunk FlattenChunkMesh(const std::shared_ptr<MACROMC_MESHING_NAMESPACE::VoxelMeshJobResult> & result) {
    FlattenedChunk out;
    if (!result) return out;
    for (const auto & sub : result->subchunk_meshes) {
        if (sub.IsEmpty()) continue;
        uint32_t base = static_cast<uint32_t>(out.vertices.size());
        for (const auto & v : sub.vertices) {
            out.vertices.push_back(ToGigaVoxelVertex(v));
        }
        for (auto idx : sub.indices) {
            out.indices.push_back(idx + base);
        }
    }
    return out;
}

// Build a placeholder 4096^2 block atlas: one solid color per builtin block
// tile (stone/dirt/grass/bedrock/sand/water), 16^2 texels each. This is enough
// to exercise the atlas bindless index path; real atlas baking is out of scope.
TRef<Texture> BuildPlaceholderAtlas() {
    // RGBA8 colors per atlas tile index. Tiles 0-5 are the original single-
    // texture builtins; tiles 6/7 are grass top/side (grass now uses a
    // top/side/bottom split: top=6, side=7, bottom=1=dirt).
    struct Color { uint8_t r, g, b, a; };
    const std::array<Color, 8> tile_colors = {{
        {128, 128, 128, 255}, // 0: stone     (grey)
        {120,  80,  50, 255}, // 1: dirt      (brown)  -- also grass bottom
        { 90, 160,  70, 255}, // 2: (legacy grass, unused now)
        { 40,  40,  40, 255}, // 3: bedrock   (dark grey)
        {220, 200, 140, 255}, // 4: sand      (sand)
        { 60, 110, 200, 255}, // 5: water     (blue)
        {110, 180,  80, 255}, // 6: grass top (brighter green)
        {130, 100,  60, 255}, // 7: grass side(brown w/ green tint band)
    }};

    auto atlas = Texture::Create(PixelFormatType::kR8G8B8A8_UNORM,
                                 MACROMC_MESHING_NAMESPACE::kAtlasTilesPerRow * MACROMC_MESHING_NAMESPACE::kAtlasTileTexelSize,
                                 MACROMC_MESHING_NAMESPACE::kAtlasTilesPerCol * MACROMC_MESHING_NAMESPACE::kAtlasTileTexelSize,
                                 1, 1);
    atlas->SetName("GigaVoxelAtlasPlaceholder");

    auto & data = const_cast<std::vector<uint8_t> &>(atlas->GetBinary());
    const uint32_t atlas_w = atlas->GetWidth();
    const uint32_t tile = MACROMC_MESHING_NAMESPACE::kAtlasTileTexelSize;
    const uint32_t tiles_per_row = MACROMC_MESHING_NAMESPACE::kAtlasTilesPerRow;

    // Default-fill the whole atlas with stone grey so unassigned tiles are sane.
    Color fill {128, 128, 128, 255};
    for (uint32_t y = 0; y < atlas->GetHeight(); ++y) {
        for (uint32_t x = 0; x < atlas_w; ++x) {
            size_t off = (y * atlas_w + x) * 4;
            data[off + 0] = fill.r;
            data[off + 1] = fill.g;
            data[off + 2] = fill.b;
            data[off + 3] = fill.a;
        }
    }
    // Paint the known tiles with their colors.
    for (uint32_t t = 0; t < tile_colors.size(); ++t) {
        uint32_t tx = t % tiles_per_row;
        uint32_t ty = t / tiles_per_row;
        const Color & c = tile_colors[t];
        for (uint32_t dy = 0; dy < tile; ++dy) {
            for (uint32_t dx = 0; dx < tile; ++dx) {
                uint32_t x = tx * tile + dx;
                uint32_t y = ty * tile + dy;
                size_t off = (y * atlas_w + x) * 4;
                data[off + 0] = c.r;
                data[off + 1] = c.g;
                data[off + 2] = c.b;
                data[off + 3] = c.a;
            }
        }
    }

    atlas->UpdateOnDevice();
    atlas->ConvertToBindless();
    return atlas;
}

} // namespace

TRef<GigaVoxelInstance> CreateGigaVoxelBringUp(
    Scene * scene,
    DeviceBindlessResourceAllocator * allocator,
    TRef<GigaVoxel> * asset_out
) {
    if (!scene || !allocator) return {};

    // macromc modules all live in the single macromc:: namespace now.
    using namespace macromc;

    // 1. Ensure builtin blocks are registered (idempotent enough for bring-up).
    MACROMC_REGISTRY_NAMESPACE::GetGlobalBlockRegistry().RegisterBuiltinBlocks();

    // 2. Build a small voxel world with the simple terrain worldgen.
    WorldData world;
    auto shell = world.CreateShell(ShellCategory::kTerrain);
    world.SetPrimaryShell(shell->GetId());

    SimpleTerrainWorldgen worldgen(1337u);
    ChunkCoord camera_chunk {0, 0};
    for (int cz = -kBringUpChunkRadius; cz <= kBringUpChunkRadius; ++cz) {
        for (int cx = -kBringUpChunkRadius; cx <= kBringUpChunkRadius; ++cx) {
            ChunkCoord coord {cx, cz};
            auto chunk = mi::Create<ChunkData>(coord);
            worldgen.GenerateChunk(shell.Raw(), coord, chunk.Raw());
            shell->SetChunk(coord, chunk);
        }
    }

    // 3. Mesh every registered chunk through the neighbor-aware context.
    //    The viewer's TaskGraph runs with 0 worker threads, so RequestMesh
    //    executes synchronously on the current thread; WaitForTask is a no-op
    //    but keeps the pattern explicit.
    auto ctx = ChunkMeshingContext::Create(MACROMC_REGISTRY_NAMESPACE::GetGlobalBlockRegistry());
    for (const auto & [coord, chunk] : shell->GetAllChunks()) {
        ctx->RegisterChunk(coord, chunk);
    }
    for (const auto & [coord, chunk] : shell->GetAllChunks()) {
        auto task = ctx->RequestMesh(coord, camera_chunk);
        if (task) mi::TaskGraph::Get().WaitForTask(task);
    }

    // 4. Build the placeholder atlas and the GigaVoxel asset.
    //    The atlas is a global config shared by all GigaVoxel assets; set it
    //    once here (app init would do this in the real macromc app).
    auto atlas = BuildPlaceholderAtlas();
    GigaVoxel::SetGlobalAtlas(atlas);
    auto gv = GigaVoxel::Create();

    // 5. Flatten each chunk's mesh and upload it to the asset as a heap-backed
    //    chunk (O(1) alloc + incremental GPU upload). Chunk id = packed coord.
    for (const auto & [coord, chunk] : shell->GetAllChunks()) {
        auto result = ctx->GetResult(coord);
        if (!result) continue;
        auto flat = FlattenChunkMesh(result);
        if (flat.vertices.empty() || flat.indices.empty()) continue;
        auto chunk_id = static_cast<mi::GigaVoxelChunkId>(
            (uint64_t(uint32_t(coord.x)) << 21) | uint64_t(uint32_t(coord.z)));
        gv->UploadChunk(chunk_id, std::move(flat.vertices), std::move(flat.indices));
    }

    if (gv->IsEmpty()) {
        MI_WARN("GigaVoxel bring-up produced no geometry; skipping instance creation.");
        return {};
    }

    // 6. Build per-chunk BLAS + refresh header (sync wrapper for bring-up).
    gv->UpdateOnDevice(allocator);

    // 7. Attach to the scene (asset owns the instance; self-registers).
    gv->AttachToScene(scene, Transform{});
    auto inst = gv->GetInstance();
    if (!inst) {
        MI_WARN("GigaVoxelInstance creation failed during bring-up.");
        return {};
    }

    if (asset_out) *asset_out = gv;
    const int grid_extent = 2 * kBringUpChunkRadius + 1;
    const uint32_t vtx_count = gv->GetGeometryHeap() ? static_cast<uint32_t>(gv->GetGeometryHeap()->GetVertexHighWatermark()) : 0u;
    const uint32_t idx_count = gv->GetGeometryHeap() ? static_cast<uint32_t>(gv->GetGeometryHeap()->GetIndexHighWatermark()) : 0u;
    MI_INFO("GigaVoxel bring-up: created terrain over {}x{} chunks, {} vertices / {} indices.",
            grid_extent, grid_extent, vtx_count, idx_count);
    return inst;
}

MI_NAMESPACE_END
