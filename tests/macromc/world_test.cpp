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

using namespace macromc::world;
using namespace macromc::registry;

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
    EXPECT_EQ(palette.GetSize(), 0);

    uint8_t idx = palette.AddBlock(BuiltinBlocks::kStoneId);
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(palette.GetSize(), 1);
    EXPECT_EQ(palette.GetBlockId(0), BuiltinBlocks::kStoneId);

    // FindOrAdd existing
    uint8_t idx2 = palette.FindOrAdd(BuiltinBlocks::kStoneId);
    EXPECT_EQ(idx2, 0);  // Same index
    EXPECT_EQ(palette.GetSize(), 1);

    // Add different block
    uint8_t idx3 = palette.FindOrAdd(BuiltinBlocks::kDirtId);
    EXPECT_EQ(idx3, 1);
    EXPECT_EQ(palette.GetSize(), 2);
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
    auto chunk = mi::TRef<ChunkData>::Create(ChunkCoord{0, 0, 0});
    EXPECT_EQ(chunk->GetCoord(), ChunkCoord(0, 0, 0));
    EXPECT_EQ(chunk->GetState(), ChunkState::kUnloaded);
}

TEST_F(WorldTest, ChunkDataSetGetBlock) {
    auto chunk = mi::TRef<ChunkData>::Create(ChunkCoord{0, 0, 0});

    chunk->SetBlock(5, 100, 10, BuiltinBlocks::kStoneId);
    EXPECT_EQ(chunk->GetBlockId(5, 100, 10), BuiltinBlocks::kStoneId);
}

TEST_F(WorldTest, ChunkDataFill) {
    auto chunk = mi::TRef<ChunkData>::Create(ChunkCoord{0, 0, 0});
    chunk->Fill(BuiltinBlocks::kDirtId);

    // All positions should be dirt
    EXPECT_EQ(chunk->GetBlockId(0, 0, 0), BuiltinBlocks::kDirtId);
    EXPECT_EQ(chunk->GetBlockId(15, 2000, 15), BuiltinBlocks::kDirtId);
}

TEST_F(WorldTest, ChunkDataStateTransitions) {
    auto chunk = mi::TRef<ChunkData>::Create(ChunkCoord{0, 0, 0});
    EXPECT_EQ(chunk->GetState(), ChunkState::kUnloaded);

    chunk->SetState(ChunkState::kGenerating);
    EXPECT_EQ(chunk->GetState(), ChunkState::kGenerating);

    chunk->SetState(ChunkState::kReady);
    EXPECT_EQ(chunk->GetState(), ChunkState::kReady);
}

TEST_F(WorldTest, ChunkDataSubChunkAccess) {
    auto chunk = mi::TRef<ChunkData>::Create(ChunkCoord{0, 0, 0});

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
    EXPECT_EQ(BlockWorldToChunkCoord({0, 0, 0}), ChunkCoord(0, 0, 0));
    EXPECT_EQ(BlockWorldToChunkCoord({15, 0, 15}), ChunkCoord(0, 0, 0));
    EXPECT_EQ(BlockWorldToChunkCoord({16, 0, 16}), ChunkCoord(1, 0, 1));

    // Negative coordinates
    EXPECT_EQ(BlockWorldToChunkCoord({-1, 0, -1}), ChunkCoord(-1, 0, -1));
    EXPECT_EQ(BlockWorldToChunkCoord({-16, 0, -16}), ChunkCoord(-1, 0, -1));
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
    auto shell = mi::TRef<WorldShellData>::Create(1, ShellCategory::kTerrain);
    EXPECT_EQ(shell->GetId(), 1u);
    EXPECT_EQ(shell->GetCategory(), ShellCategory::kTerrain);
    EXPECT_EQ(shell->GetChunkCount(), 0u);
}

TEST_F(WorldTest, ShellDataAddChunk) {
    auto shell = mi::TRef<WorldShellData>::Create(1, ShellCategory::kTerrain);
    auto chunk = mi::TRef<ChunkData>::Create(ChunkCoord{0, 0, 0});

    shell->SetChunk(ChunkCoord{0, 0, 0}, chunk);
    EXPECT_EQ(shell->GetChunkCount(), 1u);
    EXPECT_TRUE(shell->HasChunk(ChunkCoord{0, 0, 0}));

    auto retrieved = shell->GetChunk(ChunkCoord{0, 0, 0});
    ASSERT_NE(retrieved.Get(), nullptr);
    EXPECT_EQ(retrieved->GetCoord(), ChunkCoord(0, 0, 0));
}

// =============================================================================
// WorldData Tests
// =============================================================================

TEST_F(WorldTest, WorldDataCreateShell) {
    auto world = mi::TRef<WorldData>::Create();
    auto shell = world->CreateShell(ShellCategory::kTerrain);

    ASSERT_NE(shell.Get(), nullptr);
    EXPECT_EQ(world->GetShellCount(), 1u);
    EXPECT_EQ(world->GetPrimaryShell(), shell.Get());
}

TEST_F(WorldTest, WorldDataMultipleShells) {
    auto world = mi::TRef<WorldData>::Create();

    auto shell1 = world->CreateShell(ShellCategory::kTerrain);
    auto shell2 = world->CreateShell(ShellCategory::kEmpty);

    EXPECT_EQ(world->GetShellCount(), 2u);
    EXPECT_NE(shell1->GetId(), shell2->GetId());
}

// =============================================================================
// WorldGen Tests
// =============================================================================

TEST_F(WorldTest, SimpleTerrainWorldgen) {
    macromc::worldgen::SimpleTerrainWorldgen gen(12345);
    auto shell = mi::TRef<WorldShellData>::Create(1, ShellCategory::kTerrain);
    auto chunk = mi::TRef<ChunkData>::Create(ChunkCoord{0, 0, 0});

    gen.GenerateChunk(shell.Get(), ChunkCoord{0, 0, 0}, chunk.Get());

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

TEST_F(WorldTest, EmptyWorldgen) {
    macromc::worldgen::EmptyWorldgen gen;
    auto shell = mi::TRef<WorldShellData>::Create(1, ShellCategory::kEmpty);
    auto chunk = mi::TRef<ChunkData>::Create(ChunkCoord{0, 0, 0});

    gen.GenerateChunk(shell.Get(), ChunkCoord{0, 0, 0}, chunk.Get());

    // Everything should be air
    EXPECT_EQ(chunk->GetBlockId(0, 0, 0), kAirBlockId);
    EXPECT_EQ(chunk->GetBlockId(8, 2048, 8), kAirBlockId);
    EXPECT_EQ(chunk->CountNonEmptySubChunks(), 0u);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
