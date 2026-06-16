/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <gtest/gtest.h>
#include "world/block_data.h"
#include "world/chunk_data.h"
#include "world/shell_data.h"
#include "world/world_data.h"
#include "world/sub_chunk.h"
#include "world/palette.h"
#include "world/chunk_coord.h"
#include "worldgen/simple_terrain_worldgen.h"
#include "worldgen/empty_worldgen.h"
#include "registry/block_registry.h"

using namespace macromc;

// glm vector == returns a bool-vector, not a bool, so it can't be used directly
// in gtest's EXPECT_EQ. This helper compares two ivec3 componentwise.
static testing::AssertionResult CmpIVec3(const char* a_expr, const char* b_expr,
                                         const glm::ivec3& a, const glm::ivec3& b) {
    if (glm::all(glm::equal(a, b))) return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << a_expr << " != " << b_expr << ": (" << a.x << "," << a.y << "," << a.z
           << ") vs (" << b.x << "," << b.y << "," << b.z << ")";
}
#define EXPECT_IVEC3_EQ(a, b) EXPECT_PRED_FORMAT2(CmpIVec3, a, b)

class WorldTest : public ::testing::Test {
protected:
    void SetUp() override {
        GetGlobalBlockRegistry().RegisterBuiltinBlocks();
    }
};

// =============================================================================
// BlockData Tests
// =============================================================================

TEST_F(WorldTest, BlockDataDefaultIsAir) {
    BlockData block;
    EXPECT_TRUE(block.IsAir());
    EXPECT_EQ(block.id, kAirBlockId);
}

TEST_F(WorldTest, BlockDataSetStone) {
    BlockData block;
    block.id = BuiltinBlocks::kStoneId;
    EXPECT_FALSE(block.IsAir());
    EXPECT_EQ(block.id, BuiltinBlocks::kStoneId);
}

// =============================================================================
// Palette Tests
// =============================================================================

TEST_F(WorldTest, PaletteAddAndFind) {
    Palette palette;
    // Slot 0 is reserved for Air by construction (SubChunk relies on this).
    EXPECT_EQ(palette.GetSize(), 1);
    EXPECT_EQ(palette.GetBlockId(0), kAirBlockId);

    uint8_t idx = palette.AddBlock(BuiltinBlocks::kStoneId);
    EXPECT_EQ(idx, 1);
    EXPECT_EQ(palette.GetSize(), 2);
    EXPECT_EQ(palette.GetBlockId(1), BuiltinBlocks::kStoneId);

    // FindOrAdd existing
    uint8_t idx2 = palette.FindOrAdd(BuiltinBlocks::kStoneId);
    EXPECT_EQ(idx2, 1);  // Same index
    EXPECT_EQ(palette.GetSize(), 2);

    // Add different block
    uint8_t idx3 = palette.FindOrAdd(BuiltinBlocks::kDirtId);
    EXPECT_EQ(idx3, 2);
    EXPECT_EQ(palette.GetSize(), 3);
}

TEST_F(WorldTest, PaletteAlwaysReservesAirAtSlotZero) {
    // The air-at-slot-0 invariant must hold regardless of insertion order.
    Palette palette;
    palette.AddBlock(BuiltinBlocks::kStoneId);   // -> 1
    palette.AddBlock(BuiltinBlocks::kBedrockId); // -> 2

    EXPECT_EQ(palette.GetBlockId(0), kAirBlockId);
    EXPECT_EQ(palette.Find(kAirBlockId).value_or(0xFF), 0);

    // After Clear, the invariant must still hold.
    palette.Clear();
    EXPECT_EQ(palette.GetSize(), 1);
    EXPECT_EQ(palette.GetBlockId(0), kAirBlockId);
    EXPECT_EQ(palette.Find(kAirBlockId).value_or(0xFF), 0);
}

TEST_F(WorldTest, PaletteDifferentStatesGetDifferentEntries) {
    // Same id, different state must occupy distinct palette slots.
    Palette palette;
    BlockData top_slab{BuiltinBlocks::kStoneId, 1};
    BlockData bot_slab{BuiltinBlocks::kStoneId, 2};

    uint8_t i_top = palette.FindOrAdd(top_slab);
    uint8_t i_bot = palette.FindOrAdd(bot_slab);
    ASSERT_NE(i_top, i_bot);

    // Same {id,state} must map back to the same slot.
    EXPECT_EQ(palette.FindOrAdd(top_slab), i_top);
    EXPECT_EQ(palette.FindOrAdd(bot_slab), i_bot);

    // GetBlockData round-trips the full identity.
    EXPECT_EQ(palette.GetBlockData(i_top), top_slab);
    EXPECT_EQ(palette.GetBlockData(i_bot), bot_slab);
    // GetBlockId returns just the id (same for both).
    EXPECT_EQ(palette.GetBlockId(i_top), BuiltinBlocks::kStoneId);
    EXPECT_EQ(palette.GetBlockId(i_bot), BuiltinBlocks::kStoneId);
}

// =============================================================================
// SubChunk Tests
// =============================================================================

TEST_F(WorldTest, SubChunkEmptyByDefault) {
    SubChunk sc(0);
    EXPECT_TRUE(sc.IsEmpty());
    EXPECT_EQ(sc.GetIndex(5, 5, 5), 0);
}

TEST_F(WorldTest, SubChunkSetAndGet) {
    SubChunk sc(0);
    sc.SetIndex(3, 4, 5, 42);
    EXPECT_TRUE(sc.IsMixed());
    EXPECT_EQ(sc.GetIndex(3, 4, 5), 42);
    EXPECT_EQ(sc.GetIndex(0, 0, 0), 0);  // Other positions remain 0
}

TEST_F(WorldTest, SubChunkFill) {
    SubChunk sc(0);
    sc.Fill(7);
    EXPECT_TRUE(sc.IsUniform());
    EXPECT_EQ(sc.GetUniformIndex(), 7);
    EXPECT_EQ(sc.GetIndex(0, 0, 0), 7);
    EXPECT_EQ(sc.GetIndex(15, 15, 15), 7);
}

TEST_F(WorldTest, SubChunkFillAir) {
    SubChunk sc(0);
    sc.Fill(5);
    sc.Fill(0);  // Fill with air -> should become empty
    EXPECT_TRUE(sc.IsEmpty());
}

// =============================================================================
// ChunkData Tests
// =============================================================================

TEST_F(WorldTest, ChunkDataCreate) {
    auto chunk = mi::Create<ChunkData>(ChunkCoord{0, 0, 0});
    EXPECT_IVEC3_EQ(chunk->GetCoord(), ChunkCoord(0, 0, 0));
    EXPECT_EQ(chunk->GetState(), ChunkState::kUnloaded);
}

TEST_F(WorldTest, ChunkDataSetGetBlock) {
    auto chunk = mi::Create<ChunkData>(ChunkCoord{0, 0, 0});

    chunk->SetBlock(5, 100, 10, BuiltinBlocks::kStoneId);
    EXPECT_EQ(chunk->GetBlockId(5, 100, 10), BuiltinBlocks::kStoneId);
}

TEST_F(WorldTest, ChunkDataFill) {
    auto chunk = mi::Create<ChunkData>(ChunkCoord{0, 0, 0});
    chunk->Fill(BuiltinBlocks::kDirtId);

    // All positions should be dirt
    EXPECT_EQ(chunk->GetBlockId(0, 0, 0), BuiltinBlocks::kDirtId);
    EXPECT_EQ(chunk->GetBlockId(15, 2000, 15), BuiltinBlocks::kDirtId);
}

TEST_F(WorldTest, ChunkDataSetBlockWithState) {
    auto chunk = mi::Create<ChunkData>(ChunkCoord{0, 0, 0});

    // Set a block carrying state, read back the full {id, state}.
    BlockData top_slab{BuiltinBlocks::kStoneId, 1};
    chunk->SetBlock(5, 100, 10, top_slab);
    EXPECT_EQ(chunk->GetBlock(5, 100, 10), top_slab);
    EXPECT_EQ(chunk->GetBlock(5, 100, 10).state, 1);

    // A neighbouring cell with the same id but different state must store
    // distinctly (distinct palette entries).
    BlockData bot_slab{BuiltinBlocks::kStoneId, 2};
    chunk->SetBlock(6, 100, 10, bot_slab);
    EXPECT_EQ(chunk->GetBlock(6, 100, 10), bot_slab);

    // Id-only SetBlock defaults state to 0.
    chunk->SetBlock(7, 100, 10, BuiltinBlocks::kStoneId);
    EXPECT_EQ(chunk->GetBlock(7, 100, 10).state, 0);
}

TEST_F(WorldTest, ChunkDataStateTransitions) {
    auto chunk = mi::Create<ChunkData>(ChunkCoord{0, 0, 0});
    EXPECT_EQ(chunk->GetState(), ChunkState::kUnloaded);

    chunk->SetState(ChunkState::kGenerating);
    EXPECT_EQ(chunk->GetState(), ChunkState::kGenerating);

    chunk->SetState(ChunkState::kReady);
    EXPECT_EQ(chunk->GetState(), ChunkState::kReady);
}

TEST_F(WorldTest, ChunkDataSubChunkAccess) {
    auto chunk = mi::Create<ChunkData>(ChunkCoord{0, 0, 0});

    // All subchunks should be empty initially
    for (size_t i = 0; i < kSubChunksPerChunkY; ++i) {
        EXPECT_TRUE(chunk->GetSubChunk(static_cast<uint8_t>(i)).IsEmpty());
    }
    EXPECT_EQ(chunk->CountNonEmptySubChunks(), 0u);
}

// =============================================================================
// Coordinate Utility Tests
// =============================================================================

TEST_F(WorldTest, CoordConversion) {
    // Positive coordinates
    EXPECT_IVEC3_EQ(BlockWorldToChunkCoord({0, 0, 0}), ChunkCoord(0, 0, 0));
    EXPECT_IVEC3_EQ(BlockWorldToChunkCoord({15, 0, 15}), ChunkCoord(0, 0, 0));
    EXPECT_IVEC3_EQ(BlockWorldToChunkCoord({16, 0, 16}), ChunkCoord(1, 0, 1));

    // Negative coordinates
    EXPECT_IVEC3_EQ(BlockWorldToChunkCoord({-1, 0, -1}), ChunkCoord(-1, 0, -1));
    EXPECT_IVEC3_EQ(BlockWorldToChunkCoord({-16, 0, -16}), ChunkCoord(-1, 0, -1));
}

TEST_F(WorldTest, SubChunkIndexConversion) {
    EXPECT_EQ(LocalYToSubChunkIndex(0), 0);
    EXPECT_EQ(LocalYToSubChunkIndex(15), 0);
    EXPECT_EQ(LocalYToSubChunkIndex(16), 1);
    EXPECT_EQ(LocalYToSubChunkIndex(4095), 255);

    EXPECT_EQ(SubChunkIndexToLocalY(0), 0u);
    EXPECT_EQ(SubChunkIndexToLocalY(1), 16u);
    EXPECT_EQ(SubChunkIndexToLocalY(255), 4080u);
}

// =============================================================================
// WorldShellData Tests
// =============================================================================

TEST_F(WorldTest, ShellDataCreate) {
    auto shell = mi::Create<WorldShellData>(1, ShellCategory::kTerrain);
    EXPECT_EQ(shell->GetId(), 1u);
    EXPECT_EQ(shell->GetCategory(), ShellCategory::kTerrain);
    EXPECT_EQ(shell->GetChunkCount(), 0u);
}

TEST_F(WorldTest, ShellDataAddChunk) {
    auto shell = mi::Create<WorldShellData>(1, ShellCategory::kTerrain);
    auto chunk = mi::Create<ChunkData>(ChunkCoord{0, 0, 0});

    shell->SetChunk(ChunkCoord{0, 0, 0}, chunk);
    EXPECT_EQ(shell->GetChunkCount(), 1u);
    EXPECT_TRUE(shell->HasChunk(ChunkCoord{0, 0, 0}));

    auto retrieved = shell->GetChunk(ChunkCoord{0, 0, 0});
    ASSERT_NE(retrieved.Raw(), nullptr);
    // glm::ivec3 (==) returns a bvec3, not bool; compare componentwise for gtest.
    EXPECT_IVEC3_EQ(retrieved->GetCoord(), ChunkCoord(0, 0, 0));
}

// =============================================================================
// WorldData Tests
// =============================================================================

TEST_F(WorldTest, WorldDataCreateShell) {
    auto world = mi::Create<WorldData>();
    auto shell = world->CreateShell(ShellCategory::kTerrain);

    ASSERT_NE(shell.Raw(), nullptr);
    EXPECT_EQ(world->GetShellCount(), 1u);
    EXPECT_EQ(world->GetPrimaryShell(), shell.Raw());
}

TEST_F(WorldTest, WorldDataMultipleShells) {
    auto world = mi::Create<WorldData>();

    auto shell1 = world->CreateShell(ShellCategory::kTerrain);
    auto shell2 = world->CreateShell(ShellCategory::kEmpty);

    EXPECT_EQ(world->GetShellCount(), 2u);
    EXPECT_NE(shell1->GetId(), shell2->GetId());
}

// =============================================================================
// WorldGen Tests
// =============================================================================

TEST_F(WorldTest, SimpleTerrainWorldgen) {
    macromc::SimpleTerrainWorldgen gen(12345);
    auto shell = mi::Create<WorldShellData>(1, ShellCategory::kTerrain);
    auto chunk = mi::Create<ChunkData>(ChunkCoord{0, 0, 0});

    gen.GenerateChunk(shell.Raw(), ChunkCoord{0, 0, 0}, chunk.Raw());

    // Bottom (y=0) should be bedrock (not air)
    EXPECT_NE(chunk->GetBlockId(0, 0, 0), kAirBlockId);

    // Top should be air (y > terrain height)
    int air_count = 0;
    for (uint32_t x = 0; x < 16; x++) {
        for (uint32_t z = 0; z < 16; z++) {
            if (chunk->GetBlockId(x, 4095, z) == kAirBlockId) air_count++;
        }
    }
    EXPECT_EQ(air_count, 256);  // All top blocks should be air
}

TEST_F(WorldTest, SimpleTerrainWorldgenAirSubChunksAreEmpty) {
    // Regression guard for the air-at-slot-0 invariant.
    // Before the fix, worldgen wrote explicit air blocks for the entire
    // ~3900m above the surface, which forced those SubChunks into kMixed
    // (4KB each) and silently dropped the bedrock at y=0 (palette[0]).
    // After the fix, air is the default and above-surface SubChunks must
    // stay kEmpty, consuming zero memory.
    macromc::SimpleTerrainWorldgen gen(12345);
    auto shell = mi::Create<WorldShellData>(1, ShellCategory::kTerrain);
    auto chunk = mi::Create<ChunkData>(ChunkCoord{0, 0, 0});

    gen.GenerateChunk(shell.Raw(), ChunkCoord{0, 0, 0}, chunk.Raw());

    // Bedrock at y=0 must actually be present (was silently dropped before fix).
    EXPECT_EQ(chunk->GetBlockId(0, 0, 0), BuiltinBlocks::kBedrockId);

    // Surface terrain stays in the bottom few SubChunks only (base_height=64,
    // variation=32, so at most y=96 -> subchunks 0..5). Everything above
    // must remain kEmpty since worldgen never writes air there.
    size_t non_empty = chunk->CountNonEmptySubChunks();
    EXPECT_LE(non_empty, 8u) << "Above-surface SubChunks must stay kEmpty";

    // The very top SubChunk must be empty (read-back returns air).
    const SubChunk& top = chunk->GetSubChunk(255);
    EXPECT_TRUE(top.IsEmpty());
    EXPECT_EQ(chunk->GetBlockId(0, 4095, 0), kAirBlockId);
}

TEST_F(WorldTest, EmptyWorldgen) {
    macromc::EmptyWorldgen gen;
    auto shell = mi::Create<WorldShellData>(1, ShellCategory::kEmpty);
    auto chunk = mi::Create<ChunkData>(ChunkCoord{0, 0, 0});

    gen.GenerateChunk(shell.Raw(), ChunkCoord{0, 0, 0}, chunk.Raw());

    // Everything should be air
    EXPECT_EQ(chunk->GetBlockId(0, 0, 0), kAirBlockId);
    EXPECT_EQ(chunk->GetBlockId(8, 2048, 8), kAirBlockId);
    EXPECT_EQ(chunk->CountNonEmptySubChunks(), 0u);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
