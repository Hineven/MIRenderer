/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

// GigaVoxel bring-up bridge for the 3d_viewer.
//
// This is the integration glue between macromc's CPU voxel pipeline (worldgen
// + greedy meshing) and the renderer-layer GigaVoxel asset. It exists only in
// the 3d_viewer app so the macromc libraries stay free of renderer deps.
//
// CreateGigaVoxelBringUp() generates a small voxel terrain in-process (worldgen
// over a small chunk grid -> greedy meshing -> flatten to GigaVoxelVertex),
// builds a placeholder 4096^2 block atlas, uploads everything into a GigaVoxel
// asset and returns a GigaVoxelInstance registered into the given scene.

#ifndef VIEWER_GIGA_VOXEL_H
#define VIEWER_GIGA_VOXEL_H

#include <core/refcounted.h>
#include <renderer/mi_renderer_fwd.h>

MI_NAMESPACE_BEGIN

class ViewerApp;

// Create a GigaVoxel terrain bring-up instance and register it with the scene.
// Returns null on failure. The instance self-registers its RenderableIndex slot
// through its constructor (same pattern as StaticMeshInstance::Create).
//
// asset_out (optional): if non-null, receives the underlying GigaVoxel asset so
// the caller can keep it alive (it must outlive the instance).
mi::TRef<class GigaVoxelInstance> CreateGigaVoxelBringUp(
    class Scene * scene,
    class DeviceBindlessResourceAllocator * allocator,
    mi::TRef<class GigaVoxel> * asset_out = nullptr
);

MI_NAMESPACE_END

#endif // VIEWER_GIGA_VOXEL_H
