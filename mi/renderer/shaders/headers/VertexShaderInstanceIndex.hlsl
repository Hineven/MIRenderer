#ifndef VERTEX_SHADER_INSTANCE_INDEX_HLSL
#define VERTEX_SHADER_INSTANCE_INDEX_HLSL

#if __SHADER_TARGET_MAJOR != 6
#ifndef MI_PREPROCESSING
// Ignore checks when preprocessing with MI_PREPROCESSING
#error "This header is only intended for Shader Model 6.x"
#endif
#endif

#if __SHADER_TARGET_MINOR < 8
#define VERTEX_SHADER_INSTANCE_INDEX_SV_PARAMS \
    uint __internal_InstanceIndex : SV_InstanceID
#define VS_INSTANCE_INDEX __internal_InstanceIndex
#else // SM 6.8+ introduced SV_StartInstanceLocation
#define VERTEX_SHADER_INSTANCE_INDEX_SV_PARAMS \
    uint __internal_InstanceIndex : SV_InstanceID, \
    uint __internal_BaseInstance : SV_StartInstanceLocation
// For vulkan backend, SV_InstanceID already includes BaseInstance. So no need to add it again.
#define VS_INSTANCE_INDEX (__internal_InstanceIndex)
#endif

#endif // VERTEX_SHADER_INSTANCE_INDEX_HLSL
