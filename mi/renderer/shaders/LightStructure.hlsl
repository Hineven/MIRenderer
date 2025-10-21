#include "resources/LightGridSampling.hlsl"

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void ClearLightStructureHistory (uint DispatchID : SV_DispatchThreadID) {
    if(DispatchID >= LightStructure_UB.LightGridNumGrids) return;
    for(uint i = 0; i < LIGHT_GRID_NUM_HISTORY_FRAMES; i++) {
        uint Base = LIGHT_GRID_NUM_HISTORY_FRAMES * DispatchID;
        LightGrid_BloomFilterBuffer[Base + i] = 0;
        LightGrid_EnvironmentVisibilityHistoryBuffer[Base + i] = 0;
    }
}

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void UpdateLightStructureHistory (uint DispatchID : SV_DispatchThreadID) {
    if(DispatchID >= LightStructure_UB.LightGridNumGrids) return;
    // Copy current to next
    uint Base = DispatchID * LIGHT_GRID_NUM_HISTORY_FRAMES;
    uint Offset = LightStructure_UB.FrameIndex % LIGHT_GRID_NUM_HISTORY_FRAMES;
    LightGrid_BloomFilterBuffer[Base + Offset] = LightGrid_NextBloomFilterBuffer[DispatchID];
    LightGrid_EnvironmentVisibilityHistoryBuffer[Base + Offset] = LightGrid_NextEnvironmentVisibilityBuffer[DispatchID];
}