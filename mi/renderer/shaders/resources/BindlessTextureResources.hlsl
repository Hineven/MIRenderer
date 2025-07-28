#ifndef BINDLESS_TEXTURE_RESOURCES_HLSL
#define BINDLESS_TEXTURE_RESOURCES_HLSL

Texture2D __internal__BindlessIndicesBuffer_Texture[];

Texture2D GetBindlessSRV(uint index)
{
    return __internal__BindlessIndicesBuffer_Texture[index];
}

#endif