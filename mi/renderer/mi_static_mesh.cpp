/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_static_mesh.h"

MI_NAMESPACE_BEGIN

StaticMesh::StaticMesh() {
    type_ = RenderableType::kStaticMesh;
}

StaticMesh::~StaticMesh() {}


MI_NAMESPACE_END