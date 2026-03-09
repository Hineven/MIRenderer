/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_BLOCK_DATA_H
#define MACROMC_WORLD_BLOCK_DATA_H

#include "world/common.h"
#include "world/types.h"
#include "registry/types.h"
#include <cstdint>
#include <cstring>

MACROMC_WORLD_NAMESPACE_BEGIN

// BlockState: Block state data, max 64 bytes
// Design principle: No animation or instant state changes
struct BlockState {
    uint8_t data[kBlockStateSize];  // State data buffer
    
    BlockState() {
        std::memset(data, 0, kBlockStateSize);
    }
    
    // Helper methods for common state values
    uint8_t GetDirection() const { return data[0] & 0x07; }  // 0-5
    void SetDirection(uint8_t dir) { data[0] = (data[0] & ~0x07) | (dir & 0x07); }
    
    uint8_t GetGrowthStage() const { return (data[0] >> 3) & 0x0F; }  // 0-15
    void SetGrowthStage(uint8_t stage) { data[0] = (data[0] & 0x87) | ((stage & 0x0F) << 3); }
    
    uint8_t GetSignalStrength() const { return data[1] & 0x0F; }  // 0-15
    void SetSignalStrength(uint8_t strength) { data[1] = (data[1] & 0xF0) | (strength & 0x0F); }
    
    bool GetOpenState() const { return (data[1] & 0x10) != 0; }
    void SetOpenState(bool open) { 
        if (open) data[1] |= 0x10; 
        else data[1] &= ~0x10; 
    }
};

// BlockData: Complete data for a single block
struct BlockData {
    MACROMC_REGISTRY_NAMESPACE::BlockId id = MACROMC_REGISTRY_NAMESPACE::kAirBlockId;
    BlockState state{};
    
    bool IsAir() const { return id == MACROMC_REGISTRY_NAMESPACE::kAirBlockId; }
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_BLOCK_DATA_H
