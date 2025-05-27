#ifndef CONVENTIONS_HLSL
#define CONVENTIONS_HLSL 

#define INVALID_UINT 0xffffffffu

bool IsValid (uint v) {
    return v != INVALID_UINT;
}

#endif