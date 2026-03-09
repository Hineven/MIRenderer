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
#include "world/worldgen.h"
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

TEST_F(WorldTest, BlockStateDirection) {
    BlockState state;
    EXPECT_EQ(state.GetDirection(), 0);
    state.SetDirection(5);
    EXPECT_EQ(state.GetDirection(), 5);
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
    
    BlockData stone;
    stone.id = BuiltinBlocks::kStoneId;
    chunk->SetBlock(5, 100, 10, stone);
    
    auto& block = chunk->GetBlock(5, 100, 10);
    EXPECT_EQ(block.id, BuiltinBlocks::kStoneId);
}

TEST_F(WorldTest, ChunkDataStateTransitions) {
    auto chunk = mi::TRef<ChunkData>::Create(ChunkCoord{0, 0, 0});
    EXPECT_EQ(chunk->GetState(), ChunkState::kUnloaded);
    
    chunk->SetState(ChunkState::kGenerating);
    EXPECT_EQ(chunk->GetState(), ChunkState::kGenerating);
    
    chunk->SetState(ChunkState::kReady);
    EXPECT_EQ(chunk->GetState(), ChunkState::kReady);
}

// =============================================================================
// WorldShellData Tests
// =============================================================================

TEST_F(WorldTest, ShellDataCreate) {
    auto shell = mi::TRef<WorldShellData>::Create(1, ShellCategory::kTerrain);
    EXPECT_EQ(shell->GetId(), 1);
    EXPECT_EQ(shell->GetCategory(), ShellCategory::kTerrain);
    EXPECT_EQ(shell->GetChunkCount(), 0);
}

TEST_F(WorldTest, ShellDataAddChunk) {
    auto shell = mi::TRef<WorldShellData>::Create(1, ShellCategory::kTerrain);
    auto chunk = mi::TRef<ChunkData>::Create(ChunkCoord{0, 0, 0});
    
    shell->SetChunk(ChunkCoord{0, 0, 0}, chunk);
    EXPECT_EQ(shell->GetChunkCount(), 1);
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
    EXPECT_EQ(world->GetShellCount(), 1);
    EXPECT_EQ(world->GetPrimaryShell(), shell.Get());
}

TEST_F(WorldTest, WorldDataMultipleShells) {
    auto world = mi::TRef<WorldData>::Create();
    
    auto shell1 = world->CreateShell(ShellCategory::kTerrain);
    auto shell2 = world->CreateShell(ShellCategory::kEmpty);
    
    EXPECT_EQ(world->GetShellCount(), 2);
    EXPECT_NE(shell1->GetId(), shell2->GetId());
}

// =============================================================================
// WorldGen Tests
// =============================================================================

TEST_F(WorldTest, SimpleTerrainWorldgen) {
    SimpleTerrainWorldgen gen(12345);
    auto shell = mi::TRef<WorldShellData>::Create(1, ShellCategory::kTerrain);
    auto chunk = mi::TRef<ChunkData>::Create(ChunkCoord{0, 0, 0});
    
    gen.GenerateChunk(shell.Get(), ChunkCoord{0, 0, 0}, chunk.Get());
    
    // Bottom should have blocks
    EXPECT_FALSE(chunk->GetBlock(0, 0, 0).IsAir());
    
    // Top should mostly be air
    int air_count = 0;
    for (uint32_t x = 0; x < 16; x++) {
        for (uint32_t z = 0; z < 16; z++) {
            if (chunk->GetBlock(x, 255, z).IsAir()) air_count++;
        }
    }
    EXPECT_GT(air_count, 200);  // Most should be air
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
