// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_RENDERABLE_HLSL
#define MI_RENDERER_SHADERS_SHARED_RENDERABLE_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

// Default vertex format
struct RenderableHeader {
    float4 Metadata;
};

struct StaticMeshInstanceHeader {
    // Index of the static mesh which the instance refers to.
    uint StaticMeshIndex;
    uint Padding0;
    uint Padding1;
    uint Padding2;
};

struct VolumePrimitivesInstanceHeader {
    // Index of the volume primitives which the instance refers to.
    uint VolumePrimitivesIndex;
    uint Padding0;
    uint Padding1;
    uint Padding2;
};

// The instance custom index is a 20-bit index and a 4-bit flag field.
#define INSTANCE_CUSTOM_INDEX_INDEX_MASK 0x000FFFFFu
#define INSTANCE_CUSTOM_INDEX_FLAGS_MASK 0x00F00000u
// The flag indicates that the instance is a volume primitives instance.
#define INSTANCE_CUSTOM_INDEX_FLAG_VOLUME_PRIMITIVES 0x00100000u

MI_SHARED_HLSL_END

#endif // MI_RENDERER_SHADERS_SHARED_RENDERABLE_HLSL