#ifndef RENDERABLE_RESOURCES_HLSL
#define RENDERABLE_RESOURCES_HLSL

#include "../shared/SharedRenderable.hlsl"

StructuredBuffer<RenderableHeader> RenderableHeaderBuffer;
StructuredBuffer<float3x4> RenderableTransformBuffer;
StructuredBuffer<float3x4> RenderableInverseTransformBuffer;
StructuredBuffer<float3x3> RenderableNormalTransformBuffer; // transpose(inverse(RenderableTransforms))
StructuredBuffer<float3x4> PrevRenderableTransformBuffer;
StructuredBuffer<uint> RenderableHashBuffer;
StructuredBuffer<uint> PrevRenderableHashBuffer;

#endif