#include "resources/HashGridCacheResources.hlsl"

[numthreads(WAVE_SIZE, 1, 1)]	
void SSRC_ResetHashGrids (uint DispatchID : SV_DispatchThreadID) {
	if(DispatchID >= UB.HashGrids_MaxNumTiles) return;
	if(DispatchID == 0) {
		HashGrids_HistoryActiveTileCountBuffer[0] = 0;
		HashGrids_FreeTileCountBuffer[0] = UB.HashGrids_MaxNumTiles;
	}
	HashGrids_FreeTileListBuffer[DispatchID] = UB.HashGrids_MaxNumTiles - DispatchID - 1;
}

[numthreads(WAVE_SIZE, 1, 1)]
void SSRC_ReInsertHashGridTiles (uint DispatchID : SV_DispatchThreadID) {
	if(DispatchID >= HashGrids_HistoryActiveTileCountBuffer[0]) return;
	int TileIndex = HashGrids_HistoryActiveTileListBuffer[DispatchID];
	uint CurrentTimestamp = UB.FrameIndex + 1;
	uint TileTimestamp = HashGrids_TileTimestampBuffer[TileIndex];
	bool bShouldFreeTile = false;
	if(TileTimestamp + UB.HashGrids_TileLifespan < CurrentTimestamp) {
		bShouldFreeTile = true;
	}
	// Keep the tile (and insert to the hash table)
	uint TileBucketHash  = HashGrids_TileBucketHashBuffer[TileIndex];
	bool bIsNewSlot = false;
	uint SlotIndex = INVALID_UINT;
	if(!bShouldFreeTile) {
		// The hash table has been cleared, so just simply re-insert all living tiles
		// Try to insert the tile to the hash table
		SlotIndex = HashGrids_FindAndAllocate(TileBucketHash, bIsNewSlot);
	}
	if(IsInvalid(SlotIndex)) {
		// The hash table is full, we have to drop the tile
		bShouldFreeTile = true;
	}
	if(bShouldFreeTile) {
		// Free the tile
		int FreeListIndex;
		InterlockedAdd(HashGrids_FreeTileCountBuffer[0], 1, FreeListIndex);
		HashGrids_FreeTileListBuffer[FreeListIndex] = TileIndex;
		return ;
	}
	// Theorietically, this should always be true otherwise hash collision occurs
	if(bIsNewSlot) {
		// Register the tile on the active list
		int ActiveListIndex;
		InterlockedAdd(HashGrids_ActiveTileCountBuffer[0], 1, ActiveListIndex);
		HashGrids_ActiveTileListBuffer[ActiveListIndex] = TileIndex;
		// Insert the tile to the hash table
		HashGrids_BucketHashBuffer[SlotIndex] = TileBucketHash;
		HashGrids_BucketTileIndexBuffer[SlotIndex] = TileIndex;
	}
}


// Used to clear newly allocated hash grid tile cells
// Only the newly allocated cells should be cleared
[numthreads(1, 1, 1)]
void SSRC_PrepareClearNewHashGridTileCells () {
	// Clip counters
	HashGrids_FreeTileCountBuffer[0] = max(HashGrids_FreeTileCountBuffer[0], 0);
	DispatchIndirectCommand Command = (DispatchIndirectCommand) 0;
	Command.ThreadGroupCountX = 
		HashGrids_ActiveTileCountBuffer[0] 
	- HashGrids_ActiveTileCountBeforeAllocationBuffer[0];
	Command.ThreadGroupCountY = 1;
	Command.ThreadGroupCountZ = 1;
	RWDispatchIndirectCommandBuffer[0] = Command;
}

// Clear the newly allocated tile cells
[numthreads(WAVE_SIZE, 1, 1)]
void SSRC_ClearNewHashGridTileCells (uint GroupID : SV_GroupID, uint LocalID : SV_GroupThreadID) {
	uint TileIndex = HashGrids_ActiveTileListBuffer[GroupID + HashGrids_ActiveTileCountBeforeAllocationBuffer[0]];
	uint Start = TileIndex * HASHGRIDS_NUM_CELLS_PER_TILE * 2;
	for(int BaseCellOffset = 0; BaseCellOffset < (HASHGRIDS_NUM_CELLS_PER_TILE * 2); BaseCellOffset += WAVE_SIZE) {
		int DWordIndex = BaseCellOffset + LocalID;
		if(DWordIndex < (HASHGRIDS_NUM_CELLS_PER_TILE * 2)) {
			HashGrids_CellValueBuffer[Start + DWordIndex] = 0;
		}
	}
	Start = TileIndex * HASHGRIDS_TILE_CELL_MIP_OFFSET_1 * 4;
	for(int BaseCellOffset = 0; BaseCellOffset < (HASHGRIDS_TILE_CELL_MIP_OFFSET_1 * 4); BaseCellOffset += WAVE_SIZE) {
		int DWordIndex = BaseCellOffset + LocalID;
		if(DWordIndex < (HASHGRIDS_TILE_CELL_MIP_OFFSET_1 * 4)) {
			HashGrids_UpdateCellValueXBuffer[Start + DWordIndex] = 0;
		}
	}
}

// Filter hits outside of the screen or without history radiance
groupshared float4 LocalHashGridTileRadiance[HASHGRIDS_TILE_CELL_WIDTH][HASHGRIDS_TILE_CELL_WIDTH];
// Filter hash grids
[numthreads(HASHGRIDS_TILE_CELL_WIDTH, HASHGRIDS_TILE_CELL_WIDTH, 1)]
void SSRC_FilterHashGrids (uint GroupID : SV_GroupID, uint2 LocalID : SV_GroupThreadID) {
	uint  TileIndex  = HashGrids_UpdateTileListBuffer[GroupID];
	uint2 CellOffset = LocalID;
    // MIP 0
    {
        uint CellIndex     = HashGrids_GetCellIndex(TileIndex, CellOffset, 0);
		uint CompactCellIndex = HashGrids_CellIndexToCompactCellIndex(CellIndex);

        // Temporal accumulation
        float4 OldRadiance = HashGrids_GetCellRadiance(CellIndex);
        float4 NewRadiance = HashGrids_GetUpdateCellRadiance(CompactCellIndex);
        NewRadiance.rgb /= max(NewRadiance.w, 1.0f);
		float  SampleCount = min(OldRadiance.w + NewRadiance.w, UB.HashGrids_MaxNumSamples);
        
		float4 Radiance = float4(
			lerp(OldRadiance.rgb, NewRadiance.rgb, saturate(NewRadiance.w / SampleCount)),
			SampleCount
		);
        
        LocalHashGridTileRadiance[CellOffset.y][CellOffset.x] = float4(Radiance.rgb * SampleCount, SampleCount);
        uint2 PackedValue = PackFp16x4Safe(Radiance);
		HashGrids_CellValueBuffer[CellIndex * 2 + 0] = PackedValue.x;
		HashGrids_CellValueBuffer[CellIndex * 2 + 1] = PackedValue.y;

        // Clear scratch
        HashGrids_UpdateCellValueXBuffer[4 * CompactCellIndex + 0] = 0;
        HashGrids_UpdateCellValueXBuffer[4 * CompactCellIndex + 1] = 0;
        HashGrids_UpdateCellValueXBuffer[4 * CompactCellIndex + 2] = 0;
        HashGrids_UpdateCellValueXBuffer[4 * CompactCellIndex + 3] = 0;
    }

    // MIP 1
    GroupMemoryBarrierWithGroupSync();
    if(all(CellOffset < HASHGRIDS_TILE_CELL_WIDTH / 2))
    {
        uint CellIndex     = HashGrids_GetCellIndex(TileIndex, CellOffset, 1);

        // Box filter
        float4 Radiance00 = LocalHashGridTileRadiance[2 * CellOffset.y + 0][2 * CellOffset.x + 0];
        float4 Radiance01 = LocalHashGridTileRadiance[2 * CellOffset.y + 0][2 * CellOffset.x + 1];
		float4 Radiance10 = LocalHashGridTileRadiance[2 * CellOffset.y + 1][2 * CellOffset.x + 0];
		float4 Radiance11 = LocalHashGridTileRadiance[2 * CellOffset.y + 1][2 * CellOffset.x + 1];

        float4 Radiance = (Radiance00 + Radiance01 + Radiance10 + Radiance11);

        LocalHashGridTileRadiance[2 * CellOffset.y][2 * CellOffset.x] = Radiance;
		uint2 PackedValue = PackFp16x4Safe(float4(Radiance.rgb / max(Radiance.w, 1), Radiance.w));
        HashGrids_CellValueBuffer[CellIndex * 2 + 0] = PackedValue.x;
		HashGrids_CellValueBuffer[CellIndex * 2 + 1] = PackedValue.y;
    }

    // MIP 2
    GroupMemoryBarrierWithGroupSync();
	if(all(CellOffset < HASHGRIDS_TILE_CELL_WIDTH / 4))
	{
		uint CellIndex     = HashGrids_GetCellIndex(TileIndex, CellOffset, 2);

		// Box filter
		float4 Radiance00 = LocalHashGridTileRadiance[4 * CellOffset.y + 0][4 * CellOffset.x + 0];
		float4 Radiance01 = LocalHashGridTileRadiance[4 * CellOffset.y + 0][4 * CellOffset.x + 2];
		float4 Radiance10 = LocalHashGridTileRadiance[4 * CellOffset.y + 2][4 * CellOffset.x + 0];
		float4 Radiance11 = LocalHashGridTileRadiance[4 * CellOffset.y + 2][4 * CellOffset.x + 2];

		float4 Radiance = (Radiance00 + Radiance01 + Radiance10 + Radiance11);

		LocalHashGridTileRadiance[4 * CellOffset.y][4 * CellOffset.x] = Radiance;
		uint2 PackedValue = PackFp16x4Safe(float4(Radiance.rgb / max(Radiance.w, 1), Radiance.w));
		HashGrids_CellValueBuffer[CellIndex * 2 + 0] = PackedValue.x;
		HashGrids_CellValueBuffer[CellIndex * 2 + 1] = PackedValue.y;
	}

	// MIP 3
	GroupMemoryBarrierWithGroupSync();
	if(all(CellOffset < HASHGRIDS_TILE_CELL_WIDTH / 8))
	{
		uint CellIndex     = HashGrids_GetCellIndex(TileIndex, CellOffset, 3);

		// Box filter
		float4 Radiance00 = LocalHashGridTileRadiance[8 * CellOffset.y + 0][8 * CellOffset.x + 0];
		float4 Radiance01 = LocalHashGridTileRadiance[8 * CellOffset.y + 0][8 * CellOffset.x + 4];
		float4 Radiance10 = LocalHashGridTileRadiance[8 * CellOffset.y + 4][8 * CellOffset.x + 0];
		float4 Radiance11 = LocalHashGridTileRadiance[8 * CellOffset.y + 4][8 * CellOffset.x + 4];

		float4 Radiance = (Radiance00 + Radiance01 + Radiance10 + Radiance11);

		// LocalHashGridTileRadiance[8 * CellOffset.y][8 * CellOffset.x] = Radiance;
		uint2 PackedValue = PackFp16x4Safe(float4(Radiance.rgb / max(Radiance.w, 1), Radiance.w));
		HashGrids_CellValueBuffer[CellIndex * 2 + 0] = PackedValue.x;
		HashGrids_CellValueBuffer[CellIndex * 2 + 1] = PackedValue.y;
	}
}

// Resolve probe update ray radiance results from hash grid cache
[numthreads(WAVE_SIZE, 1, 1)]
void SSRC_ResolveProbeUpdateRayRadianceFromCells (uint DispatchID : SV_DispatchThreadID)
{
	if(DispatchID >= RWProbeUpdateRayHitShadeCountBuffer[0]) return ;
	int ProbeUpdateRayIndex = RWProbeUpdateRayHitShadeListBuffer[DispatchID];
    uint CellIndex          = RWProbeUpdateRayResolveHashCellIndexBuffer[ProbeUpdateRayIndex];
	float4 Radiance         = HashGrids_GetFilteredRadiance(CellIndex);
	ProbeUpdateRayResult Result = FetchProbeUpdateRayResult(ProbeUpdateRayIndex);
	if(!Result.bBypass) {
		// Resolve radiance from hash grid cache if no bypass is specified
		if(!UB.II_EnvironmentOnly) {
			Result.Radiance = Radiance.xyz;
		}
		WriteProbeUpdateRayResult(ProbeUpdateRayIndex, Result);
	}
}