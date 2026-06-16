/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "registry/block_registry.h"

MACROMC_REGISTRY_NAMESPACE_BEGIN

BlockRegistry::BlockRegistry() {
    // Reserve slot 0 for Air
    definitions_.resize(1);
    definitions_[0].id = BuiltinBlocks::kAirId;
    definitions_[0].name = "air";
    definitions_[0].solid = false;
    definitions_[0].transparent = true;
    definitions_[0].render = false;
    // Air has no solid faces (so it never culls a neighbor's exposed face
    // during greedy meshing). The default face_solid[] is all-true, so reset.
    for (int i = 0; i < static_cast<int>(BlockFace::kCount); ++i) {
        definitions_[0].face_solid[i] = false;
    }
    name_to_id_["air"] = BuiltinBlocks::kAirId;
}

BlockRegistry::~BlockRegistry() = default;

BlockId BlockRegistry::RegisterBlock(const std::string& name, BlockDefinition def) {
    if (name_to_id_.find(name) != name_to_id_.end()) {
        return name_to_id_[name];  // Already registered
    }
    
    BlockId id = next_id_++;
    def.id = id;
    
    if (definitions_.size() <= id) {
        definitions_.resize(id + 1);
    }
    definitions_[id] = std::move(def);
    name_to_id_[name] = id;
    
    return id;
}

const BlockDefinition* BlockRegistry::GetDefinition(BlockId id) const {
    if (id >= definitions_.size()) {
        return nullptr;
    }
    return &definitions_[id];
}

const BlockDefinition* BlockRegistry::GetDefinition(const std::string& name) const {
    auto it = name_to_id_.find(name);
    if (it == name_to_id_.end()) {
        return nullptr;
    }
    return GetDefinition(it->second);
}

BlockId BlockRegistry::GetBlockId(const std::string& name) const {
    auto it = name_to_id_.find(name);
    if (it == name_to_id_.end()) {
        return kInvalidBlockId;
    }
    return it->second;
}

bool BlockRegistry::IsValidBlockId(BlockId id) const {
    return id < definitions_.size();
}

void BlockRegistry::RegisterBuiltinBlocks() {
    // Stone (tile 0)
    BlockDefinition stone;
    stone.id = BuiltinBlocks::kStoneId;
    stone.name = "stone";
    stone.solid = true;
    stone.transparent = false;
    stone.render = true;
    stone.SetUniformTexture(0);
    stone.SetAllFacesSolid();
    RegisterBlock("stone", stone);

    // Dirt (tile 1)
    BlockDefinition dirt;
    dirt.id = BuiltinBlocks::kDirtId;
    dirt.name = "dirt";
    dirt.solid = true;
    dirt.transparent = false;
    dirt.render = true;
    dirt.SetUniformTexture(1);
    dirt.SetAllFacesSolid();
    RegisterBlock("dirt", dirt);

    // Grass: top/side/bottom split (top=6, side=7, bottom=1=dirt).
    // Tiles 6/7 are new placeholder slots added to the bring-up atlas.
    BlockDefinition grass;
    grass.id = BuiltinBlocks::kGrassId;
    grass.name = "grass";
    grass.solid = true;
    grass.transparent = false;
    grass.render = true;
    grass.SetTopSideBottom(/*top*/ 6, /*side*/ 7, /*bottom*/ 1);
    grass.SetAllFacesSolid();
    RegisterBlock("grass", grass);

    // Bedrock (tile 3)
    BlockDefinition bedrock;
    bedrock.id = BuiltinBlocks::kBedrockId;
    bedrock.name = "bedrock";
    bedrock.solid = true;
    bedrock.transparent = false;
    bedrock.render = true;
    bedrock.SetUniformTexture(3);
    bedrock.SetAllFacesSolid();
    RegisterBlock("bedrock", bedrock);

    // Sand (tile 4)
    BlockDefinition sand;
    sand.id = BuiltinBlocks::kSandId;
    sand.name = "sand";
    sand.solid = true;
    sand.transparent = false;
    sand.render = true;
    sand.SetUniformTexture(4);
    sand.SetAllFacesSolid();
    RegisterBlock("sand", sand);

    // Water (tile 5)
    BlockDefinition water;
    water.id = BuiltinBlocks::kWaterId;
    water.name = "water";
    water.solid = false;
    water.transparent = true;
    water.render = true;
    water.SetUniformTexture(5);
    // Water faces are not solid (allows seeing through adjacent water)
    for (int i = 0; i < static_cast<int>(BlockFace::kCount); ++i) {
        water.face_solid[i] = false;
        water.face_render[i] = true;
    }
    RegisterBlock("water", water);
}

// Global registry singleton
BlockRegistry& GetGlobalBlockRegistry() {
    static BlockRegistry instance;
    return instance;
}

MACROMC_REGISTRY_NAMESPACE_END
