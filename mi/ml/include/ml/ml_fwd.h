/*
 * Created: 2024/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_MI_FWD_H
#define MIRENDERER_MI_FWD_H

#include "core/common.h"
#include "core/refcounted.h"
MI_NAMESPACE_BEGIN

class MIFoundation;
class MaterialRoot;
class Material;
class Geometry;
class Mesh;
class Light;
class Renderable;

typedef TRef<MaterialRoot> MaterialRootRef;
typedef TRef<Material> MaterialRef;
typedef TRef<Geometry> GeometryRef;
typedef TRef<Mesh> MeshRef;
typedef TRef<Light> LightRef;
typedef TRef<Renderable> RenderableRef;

MI_NAMESPACE_END
#endif //MIRENDERER_MI_FWD_H
