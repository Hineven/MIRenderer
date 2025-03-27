/*
 * Created: 2024/9/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_MI_MESH_H
#define MIRENDERER_MI_MESH_H

#include <core/common.h>

class Mesh {
public:
    Mesh() = default;
    ~Mesh() = default;
    uint32_t GetNumTriangles () ;
    uint32_t GetNumVertices () ;
    // Does it have virtualized geometries generated.
    // Cannot be activated if it is dynamic.
    bool IsVirtualized () ;
    // If it is dynamic (updated frequently).
    bool IsDynamic () ;
    // If it can be organized into acceleration structure.
    bool IsRayTraced () ;
    // Procedural meshes have their own implementations for
    // generation & update & virtualization & rasterization & acceleration structure build.
    bool IsProcedural () ;
    // If it is informed within geometry ticks.
    bool IsTickable () ;
    // Called frame to frame, parallelized by the engine.
    virtual void OnGeometryTick (const GeometryTickContext & context) ;

protected:
    bool is_procedural_ = false;
    bool is_virtualized_ = false;
    bool is_dynamic_ = false;
};

class ProceduralMesh : public Mesh {
public:
};

#endif //MIRENDERER_MI_MESH_H
