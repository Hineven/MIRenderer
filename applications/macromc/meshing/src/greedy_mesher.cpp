/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "meshing/greedy_mesher.h"

#include <array>
#include <cstring>

MACROMC_MESHING_NAMESPACE_BEGIN

// ============================================================================
// SingleChunkVoxelSource
// ============================================================================

SingleChunkVoxelSource::SingleChunkVoxelSource(const ChunkData& chunk, const ChunkCoord& chunk_coord)
    : chunk_(chunk), chunk_coord_(chunk_coord),
      origin_(ChunkCoordToBlockOrigin(chunk_coord)) {}

BlockId SingleChunkVoxelSource::GetBlockIdAtWorld(int32_t bx, int32_t by, int32_t bz) const {
    // Outside this chunk => air (conservative).
    int32_t lx = bx - origin_.x;
    int32_t lz = bz - origin_.z;
    if (lx < 0 || lx >= static_cast<int32_t>(kChunkSizeX)) return kAirBlockId;
    if (lz < 0 || lz >= static_cast<int32_t>(kChunkSizeZ)) return kAirBlockId;
    if (by < 0 || by >= static_cast<int32_t>(kChunkSizeY)) return kAirBlockId;
    return chunk_.GetBlockId(static_cast<uint32_t>(lx), static_cast<uint32_t>(by), static_cast<uint32_t>(lz));
}

// ============================================================================
// GreedyMesher
// ============================================================================

GreedyMesher::GreedyMesher(const BlockRegistry& registry) : registry_(registry) {}

uint32_t GreedyMesher::GetTextureIndex(BlockId block_id, BlockFace face) const {
    const BlockDefinition* def = registry_.GetDefinition(block_id);
    if (!def) return 0;
    return def->GetFaceTexture(face);
}

bool GreedyMesher::IsFaceSolid(BlockId block_id, BlockFace face) const {
    const BlockDefinition* def = registry_.GetDefinition(block_id);
    if (!def) return false; // unknown => not solid => keep neighbor's face (conservative)
    return def->IsFaceSolid(face);
}

bool GreedyMesher::IsRenderable(BlockId block_id) const {
    if (block_id == kAirBlockId) return false;
    const BlockDefinition* def = registry_.GetDefinition(block_id);
    if (!def) return false;
    return def->render;
}

void GreedyMesher::MeshSubChunk(const ChunkData& chunk,
                                const ChunkCoord& chunk_coord,
                                uint8_t subchunk_y,
                                const VoxelSource& source,
                                glm::ivec3 block_origin,
                                VoxelMesh& out_mesh) const {
    (void)chunk_coord; // source handles neighbor resolution; kept for API clarity.
    (void)subchunk_y;  // subchunk addressed via block_origin.y by MeshFaceDirection.

    // Mesh the 6 face directions. For each axis d (0=X,1=Y,2=Z) we sweep the
    // positive face (+) and negative face (-).
    //
    // For each direction we operate on a 16x16 transverse plane (u,v) and a
    // single layer of voxels at depth `d_layer` along the sweep axis.
    //
    // A face at sweep-layer L (0..16) is the boundary between voxel layer L-1
    // and voxel layer L. Visibility:
    //   - The block at the back side (the solid voxel that "owns" the face)
    //     must be renderable.
    //   - The block at the front side (across the face) must NOT be face-solid
    //     on the opposite face (so it does not occlude).
    //
    // We emit faces for sweep layers 0..kSubChunkSize (inclusive), where layer
    // 0 is the subchunk's negative boundary and layer kSubChunkSize is the
    // positive boundary. This lets boundary faces against neighbors be culled
    // or kept naturally via `source`.

    // Helper: axis orderings for (sweep d, transverse u, transverse v).
    struct Axis { int d, u, v; };
    constexpr std::array<Axis, 3> kAxes = {{
        {0, 1, 2}, // sweep X, transverse (Y,Z)
        {1, 0, 2}, // sweep Y, transverse (X,Z)
        {2, 0, 1}, // sweep Z, transverse (X,Y)
    }};

    for (const auto& ax : kAxes) {
        // Positive face (+d): owner voxel is at layer L-1, front neighbor at layer L.
        MeshFaceDirection(chunk, source, block_origin, ax.d, ax.u, ax.v, +1, out_mesh);
        // Negative face (-d): owner voxel is at layer L, front neighbor at layer L-1.
        MeshFaceDirection(chunk, source, block_origin, ax.d, ax.u, ax.v, -1, out_mesh);
    }
}

// Per-direction mask values: which block type owns a visible face (or 0 = none).
// We reuse BlockId directly; kAirBlockId (0) sentinel = no face.
namespace {
struct MaskEntry {
    BlockId owner = kAirBlockId;   // the renderable block owning the face (kAirBlockId = no face)
};
}

void GreedyMesher::MeshFaceDirection(const ChunkData& chunk,
                                     const VoxelSource& source,
                                     glm::ivec3 block_origin,
                                     int d, int u, int v,
                                     int8_t normal_sign,
                                     VoxelMesh& out_mesh) const {
    // We evaluate one sweep layer at a time over the 16x16 transverse (u,v) plane.
    // For each voxel at local depth L (0..15) along axis d, it "owns" two faces:
    //   - its +d face (normal_sign = +1), fronted by the voxel at depth L+1
    //   - its -d face (normal_sign = -1), fronted by the voxel at depth L-1
    // A face is visible iff the owner is renderable + renders that face, and
    // the front neighbor is NOT solid on the opposing face. Boundary front
    // neighbors (depth outside [0,16)) are resolved via `source`.

    constexpr int N = static_cast<int>(kSubChunkSize); // 16
    std::array<MaskEntry, N * N> mask{};

    // BlockFace the face looks toward (front), and the owner face we see (back).
    // BlockFace order: kPosX=0,kNegX=1,kPosY=2,kNegY=3,kPosZ=4,kNegZ=5 (Pos=even, Neg=odd).
    using F = BlockFace;
    F front_face;
    if (d == 0) {
        front_face = (normal_sign > 0) ? F::kPosX : F::kNegX;
    } else if (d == 1) {
        front_face = (normal_sign > 0) ? F::kPosY : F::kNegY;
    } else {
        front_face = (normal_sign > 0) ? F::kPosZ : F::kNegZ;
    }
    F back_face = OppositeFace(front_face);

    // Chunks span full height, so block_origin.y is already the absolute local y
    // of the subchunk's (0,0,0) corner within the chunk.
    const int chunk_local_y_base = block_origin.y;

    for (int L = 0; L < N; ++L) {
        // Build the visibility mask for this sweep layer.
        for (int iu = 0; iu < N; ++iu) {
            for (int iv = 0; iv < N; ++iv) {
                glm::ivec3 lp(0);
                lp[d] = L;
                lp[u] = iu;
                lp[v] = iv;

                BlockId owner = chunk.GetBlockId(
                    static_cast<uint32_t>(lp.x),
                    static_cast<uint32_t>(chunk_local_y_base + lp.y),
                    static_cast<uint32_t>(lp.z));

                MaskEntry& me = mask[iu * N + iv];
                me.owner = kAirBlockId;

                if (!IsRenderable(owner)) continue;
                // Does the block actually render this face?
                const BlockDefinition* odef = registry_.GetDefinition(owner);
                if (!odef || !odef->IsFaceRendered(back_face)) continue;

                // Front neighbor: voxel at local depth L + normal_sign.
                glm::ivec3 nlp = lp;
                nlp[d] = L + normal_sign;

                BlockId front_id;
                if (nlp[d] >= 0 && nlp[d] < N) {
                    // Inside this subchunk -> query chunk directly.
                    front_id = chunk.GetBlockId(
                        static_cast<uint32_t>(nlp.x),
                        static_cast<uint32_t>(chunk_local_y_base + nlp.y),
                        static_cast<uint32_t>(nlp.z));
                } else {
                    // Boundary -> query neighbor via source (world coords).
                    glm::ivec3 wpos = block_origin + lp;
                    // step across the face by one block along d
                    wpos[d] += normal_sign;
                    front_id = source.GetBlockIdAtWorld(wpos.x, wpos.y, wpos.z);
                }

                // Face is visible iff front neighbor is NOT solid on its opposing face.
                if (IsFaceSolid(front_id, front_face)) {
                    continue; // culled
                }
                me.owner = owner;
            }
        }

        // Greedy merge of the mask: merge contiguous same-owner cells into quads.
        // Standard 2D greedy rectangle merge over the (u,v) plane.
        std::array<bool, N * N> merged{};
        for (int iu = 0; iu < N; ++iu) {
            for (int iv = 0; iv < N; ++iv) {
                if (merged[iu * N + iv]) continue;
                BlockId owner = mask[iu * N + iv].owner;
                if (owner == kAirBlockId) continue;

                // Compute width (along u) and height (along v) of this quad.
                int w = 1;
                while (iv + w < N && !merged[iu * N + iv + w]
                       && mask[iu * N + iv + w].owner == owner) {
                    ++w;
                }
                int h = 1;
                bool done = false;
                while (!done) {
                    if (iu + h >= N) break;
                    for (int k = 0; k < w; ++k) {
                        if (merged[(iu + h) * N + iv + k]
                            || mask[(iu + h) * N + iv + k].owner != owner) {
                            done = true;
                            break;
                        }
                    }
                    if (!done) ++h;
                }

                // Mark merged cells.
                for (int a = 0; a < h; ++a)
                    for (int b = 0; b < w; ++b)
                        merged[(iu + a) * N + iv + b] = true;

                // Emit a quad. The face is located at sweep depth:
                //   positive face (+): at local depth L+1 along d
                //   negative face (-): at local depth L along d
                float depth_f = static_cast<float>((normal_sign > 0) ? (L + 1) : L);

                // Corner (in subchunk-local coords) for (iu, iv) -> (iu+h, iv+w).
                glm::vec3 corner0(0.0f), corner1(0.0f);
                corner0[d] = depth_f;
                corner1[d] = depth_f;
                corner0[u] = static_cast<float>(iu);
                corner0[v] = static_cast<float>(iv);
                corner1[u] = static_cast<float>(iu + h);
                corner1[v] = static_cast<float>(iv + w);

                // Chunk-local position offset (X/Z = 0 at chunk corner; Y = subchunk
                // base). The chunk's world placement is added on the GPU via
                // GigaVoxelChunkHeader.ChunkOrigin / the per-chunk TLAS transform.
                glm::vec3 wor = glm::vec3(block_origin);

                // Per-face texture: resolve the atlas tile for the face we are
                // rendering (back_face = the owner's face turned toward us).
                uint32_t tex = GetTextureIndex(owner, back_face);
                glm::vec2 uv_base = AtlasTileOriginUV(static_cast<BlockId>(tex));
                // UV scale: quad spans (h along u, w along v) blocks -> that many tiles.
                glm::vec2 uv_scale(static_cast<float>(h), static_cast<float>(w));

                // Normal.
                glm::vec3 normal(0.0f);
                normal[d] = static_cast<float>(normal_sign);

                // Emit 4 vertices + 2 triangles.
                // Quad corners (in (u,v) local):
                //   p00 = corner0           (u=iu,    v=iv)
                //   p10 = corner0 + u*h     (u=iu+h,  v=iv)
                //   p11 = corner1           (u=iu+h,  v=iv+w)
                //   p01 = corner0 + v*w     (u=iu,    v=iv+w)
                glm::vec3 p00 = wor + corner0;
                glm::vec3 p10 = wor + corner0; p10[u] += static_cast<float>(h);
                glm::vec3 p11 = wor + corner1;
                glm::vec3 p01 = wor + corner0; p01[v] += static_cast<float>(w);

                uint32_t base = static_cast<uint32_t>(out_mesh.vertices.size());
                out_mesh.vertices.emplace_back(p00, normal, uv_base, uv_scale, tex);
                out_mesh.vertices.emplace_back(p10, normal, uv_base, uv_scale, tex);
                out_mesh.vertices.emplace_back(p11, normal, uv_base, uv_scale, tex);
                out_mesh.vertices.emplace_back(p01, normal, uv_base, uv_scale, tex);

                // Winding: ensure CCW when viewed from the front (normal side),
                // so backface culling / front-face determination is correct.
                //
                // The base winding (0,1,2),(0,2,3) [tri0 = p00,p10,p11] has a
                // geometric normal that depends on the (d,u,v) axis layout's
                // handedness: for d in {X,Z} it points +axis[d], but for d == Y
                // it points -axis[d]. We want the geometric normal to equal the
                // stored face normal (normal_sign * axis[d]); flip the two
                // triangles' first two indices when the base winding disagrees.
                //   d in {X,Z}: base normal = +axis[d] -> flip iff sign < 0.
                //   d == Y   : base normal = -axis[d] -> flip iff sign > 0.
                const bool flip_winding = (d == 1) ? (normal_sign > 0) : (normal_sign < 0);
                if (!flip_winding) {
                    out_mesh.indices.push_back(base + 0);
                    out_mesh.indices.push_back(base + 1);
                    out_mesh.indices.push_back(base + 2);
                    out_mesh.indices.push_back(base + 0);
                    out_mesh.indices.push_back(base + 2);
                    out_mesh.indices.push_back(base + 3);
                } else {
                    out_mesh.indices.push_back(base + 0);
                    out_mesh.indices.push_back(base + 2);
                    out_mesh.indices.push_back(base + 1);
                    out_mesh.indices.push_back(base + 0);
                    out_mesh.indices.push_back(base + 3);
                    out_mesh.indices.push_back(base + 2);
                }
            }
        }
    }
}

MACROMC_MESHING_NAMESPACE_END
