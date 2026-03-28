#include "resources/LightGridSampling.hlsl"

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void ClearLightStructureHistory (uint DispatchID : SV_DispatchThreadID) {
    if(DispatchID >= LightStructure_UB.LightGridNumGrids) return;
    for(uint i = 0; i < LIGHT_GRID_NUM_HISTORY_FRAMES; i++) {
        uint Base = LIGHT_GRID_NUM_HISTORY_FRAMES * DispatchID;
        LightGrid_RWBloomFilterBuffer[Base + i] = 0;
        LightGrid_RWEnvironmentVisibilityHistoryBuffer[Base + i] = 0;
    }
}

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void UpdateLightStructureHistory (uint DispatchID : SV_DispatchThreadID) {
    if(DispatchID >= LightStructure_UB.LightGridNumGrids) return;
    // Copy current to next
    uint Base = DispatchID * LIGHT_GRID_NUM_HISTORY_FRAMES;
    uint Offset = LightStructure_UB.FrameIndex % LIGHT_GRID_NUM_HISTORY_FRAMES;
    LightGrid_RWBloomFilterBuffer[Base + Offset] = LightGrid_RWNextBloomFilterBuffer[DispatchID];
    LightGrid_RWEnvironmentVisibilityHistoryBuffer[Base + Offset] = LightGrid_RWNextEnvironmentVisibilityBuffer[DispatchID];
}