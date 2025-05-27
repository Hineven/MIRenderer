#ifndef BINDLESS_TEXTURES_HLSL
#define BINDLESS_TEXTURES_HLSL

Texture2D __internal__BindlessIndicesBuffer_Texture[];

Texture2D GetBindlessSRV(uint index)
{
    return __internal__BindlessIndicesBuffer_Texture[index];
}

#endif