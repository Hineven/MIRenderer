/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_REGISTRY_BLOCK_REGISTRY_H
#define MACROMC_REGISTRY_BLOCK_REGISTRY_H

#include "registry/common.h"
#include "registry/types.h"
#include "registry/block_def.h"
#include <vector>
#include <unordered_map>
#include <string>

MACROMC_REGISTRY_NAMESPACE_BEGIN

// BlockRegistry: Block type registration system
class BlockRegistry {
public:
    BlockRegistry();
    ~BlockRegistry();
    
    // Non-copyable
    BlockRegistry(const BlockRegistry&) = delete;
    BlockRegistry& operator=(const BlockRegistry&) = delete;
    
    // Register a block type
    BlockId RegisterBlock(const std::string& name, BlockDefinition def);
    
    // Query methods
    const BlockDefinition* GetDefinition(BlockId id) const;
    const BlockDefinition* GetDefinition(const std::string& name) const;
    BlockId GetBlockId(const std::string& name) const;
    
    // Check if a block ID is valid
    bool IsValidBlockId(BlockId id) const;
    
    // Register builtin blocks
    void RegisterBuiltinBlocks();
    
private:
    std::vector<BlockDefinition> definitions_;
    std::unordered_map<std::string, BlockId> name_to_id_;
    BlockId next_id_ = 1;  // 0 is reserved for Air
};

// Global registry access
BlockRegistry& GetGlobalBlockRegistry();

MACROMC_REGISTRY_NAMESPACE_END

#endif // MACROMC_REGISTRY_BLOCK_REGISTRY_H
