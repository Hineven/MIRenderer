#ifndef COMMONVERTEX_HLSL
#define COMMONVERTEX_HLSL
struct DefaultStaticMeshVertex {
    float3 Position : position;
    float3 Normal   : normal;
    float2 UV       : uv;
};

#endif