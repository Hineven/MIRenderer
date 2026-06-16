/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <gtest/gtest.h>
#include "registry/block_registry.h"

using namespace macromc;

class BlockRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        GetGlobalBlockRegistry().RegisterBuiltinBlocks();
    }
};

TEST_F(BlockRegistryTest, BuiltinBlocksRegistered) {
    auto& reg = GetGlobalBlockRegistry();
    EXPECT_EQ(reg.GetBlockId("air"), BuiltinBlocks::kAirId);
    EXPECT_EQ(reg.GetBlockId("stone"), BuiltinBlocks::kStoneId);
    EXPECT_EQ(reg.GetBlockId("dirt"), BuiltinBlocks::kDirtId);
    EXPECT_EQ(reg.GetBlockId("grass"), BuiltinBlocks::kGrassId);
    EXPECT_EQ(reg.GetBlockId("bedrock"), BuiltinBlocks::kBedrockId);
}

TEST_F(BlockRegistryTest, GetDefinitionById) {
    auto& reg = GetGlobalBlockRegistry();
    auto* stone_def = reg.GetDefinition(BuiltinBlocks::kStoneId);
    ASSERT_NE(stone_def, nullptr);
    EXPECT_EQ(stone_def->id, BuiltinBlocks::kStoneId);
    EXPECT_EQ(stone_def->name, std::string("stone"));
    EXPECT_TRUE(stone_def->solid);
    EXPECT_FALSE(stone_def->transparent);
    EXPECT_TRUE(stone_def->render);
}

TEST_F(BlockRegistryTest, GetDefinitionByName) {
    auto& reg = GetGlobalBlockRegistry();
    auto* grass_def = reg.GetDefinition(std::string("grass"));
    ASSERT_NE(grass_def, nullptr);
    EXPECT_EQ(grass_def->id, BuiltinBlocks::kGrassId);
}

TEST_F(BlockRegistryTest, InvalidBlockReturnsNull) {
    auto& reg = GetGlobalBlockRegistry();
    auto* def = reg.GetDefinition(static_cast<BlockId>(9999));
    EXPECT_EQ(def, nullptr);
}

TEST_F(BlockRegistryTest, NonexistentNameReturnsInvalidId) {
    auto& reg = GetGlobalBlockRegistry();
    BlockId id = reg.GetBlockId(std::string("nonexistent_block"));
    EXPECT_EQ(id, kInvalidBlockId);
}

TEST_F(BlockRegistryTest, BlockDefinitionPerFaceTexture) {
    auto& reg = GetGlobalBlockRegistry();

    // Grass uses a top/side/bottom split (SetTopSideBottom(6,7,1)).
    auto* grass_def = reg.GetDefinition(BuiltinBlocks::kGrassId);
    ASSERT_NE(grass_def, nullptr);
    EXPECT_EQ(grass_def->GetFaceTexture(BlockFace::kPosY), 6);  // top
    EXPECT_EQ(grass_def->GetFaceTexture(BlockFace::kNegY), 1);  // bottom == dirt
    EXPECT_EQ(grass_def->GetFaceTexture(BlockFace::kPosX), 7);  // side
    EXPECT_EQ(grass_def->GetFaceTexture(BlockFace::kNegX), 7);  // side
    EXPECT_EQ(grass_def->GetFaceTexture(BlockFace::kPosZ), 7);  // side
    EXPECT_EQ(grass_def->GetFaceTexture(BlockFace::kNegZ), 7);  // side

    // Stone uses SetUniformTexture(0): every face the same tile.
    auto* stone_def = reg.GetDefinition(BuiltinBlocks::kStoneId);
    ASSERT_NE(stone_def, nullptr);
    for (int i = 0; i < static_cast<int>(BlockFace::kCount); ++i) {
        EXPECT_EQ(stone_def->face_textures[i], 0);
    }
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
