#ifndef GEOMETRY_BUFFERS_HLSL
#define GEOMETRY_BUFFERS_HLSL

#include "Camera.hlsl"
#include "Transform.hlsl"

// This bit on the flags texture indicates that the pixel is invalid for SSRT to 
// step through. For example, volumetric primitives, or some other geometry that 
// doesn't have proper depth information.
#define FLAG_BITS_TEXTURE_INVALID_FOR_SSRT 0x1

#endif