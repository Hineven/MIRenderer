/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_CORE_TYPES_H
#define MIRENDERER_CORE_TYPES_H

// Helper macro to easily make flags for a certain enum type.
#define MAKE_FLAGS(FlagName) \
struct FlagName##Flags { \
uint32_t flags; \
FORCEINLINE FlagName##Flags() : flags(0) {} \
FORCEINLINE FlagName##Flags(FlagName##FlagBits flag) : flags(static_cast<uint32_t>(flag)) {}  \
FORCEINLINE FlagName##Flags(uint32_t flags) : flags(flags) {}  \
FORCEINLINE explicit operator bool() const { return flags != 0; }                                  \
FORCEINLINE operator unsigned () const { return flags; }                          \
FORCEINLINE FlagName##Flags operator ~ () const { return ~flags; }                        \
FORCEINLINE bool operator==(FlagName##Flags other) const { return flags == other.flags; } \
FORCEINLINE bool operator!=(FlagName##Flags other) const { return flags != other.flags; } \
FORCEINLINE FlagName##Flags & operator|=(FlagName##Flags other) { flags |= other.flags; return * this; } \
FORCEINLINE FlagName##Flags & operator&=(FlagName##Flags other) { flags &= other.flags; return * this; } \
}; \
FORCEINLINE FlagName##Flags operator|(FlagName##FlagBits a, FlagName##FlagBits b) { \
return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator&(FlagName##FlagBits a, FlagName##FlagBits b) { \
return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator|(FlagName##Flags a, FlagName##FlagBits b) { \
return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator&(FlagName##Flags a, FlagName##FlagBits b) { \
return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator|(FlagName##FlagBits a, FlagName##Flags b) { \
return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator&(FlagName##FlagBits a, FlagName##Flags b) { \
return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator|(FlagName##Flags a, FlagName##Flags b) { \
return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator&(FlagName##Flags a, FlagName##Flags b) { \
return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator^(FlagName##Flags a, FlagName##Flags b) { \
return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) ^ static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator~(FlagName##FlagBits a) { \
return static_cast<FlagName##Flags>(~static_cast<uint32_t>(a)); \
}

#include "core/common.h"
MI_NAMESPACE_BEGIN

enum class ThreadPerformanceType {
    kHigh,
    kLow,
    kMax
};

// The type of threads in the renderer.
// There are only 2 threads in the renderer, and they are performance sensitive, so please allocate them to large cores
// if possible.
enum class RendererThreadType {
    // A single thread interacting directly with graphics APIs.
    kRHI,
    // The main thread performing all the rendering stuff like scene traverse, RDG recording, etc.
    kRender,
    kMax
};

// Types from render resources
enum class LightType {
    // Directional light (sunshine)
    kDirectional,
    // Skylight
    kInfinite,
    // Point light
    kPoint,
    // Area lights (meshed lights)
    kArea,
    kMax
};

enum class BlobResourceAccessFlagBits : uint32_t {
    kRead = 1u<<0,
    kWrite = 1u<<1,
    kAll = 0xffffffffu
};

// Weird that CLion keeps giving me linting errors flag operations if I use MAKE_FLAGS
// MAKE_FLAGS(BlobResourceAccess)

// The following is the same as MAKE_FLAGS(BlobResourceAccess), just do so manually to avoid linting errors in CLion.
struct BlobResourceAccessFlags {
    uint32_t flags;
    FORCEINLINE BlobResourceAccessFlags() : flags(0) {}
    FORCEINLINE BlobResourceAccessFlags(BlobResourceAccessFlagBits flag) : flags(static_cast<uint32_t>(flag)) {}
    FORCEINLINE BlobResourceAccessFlags(uint32_t flags) : flags(flags) {}
    FORCEINLINE operator bool() const { return flags != 0; }
    FORCEINLINE explicit operator unsigned() const { return flags; }
    FORCEINLINE bool operator==(BlobResourceAccessFlags other) const { return flags == other. flags; }
    FORCEINLINE bool operator!=(BlobResourceAccessFlags other) const { return flags != other. flags; }
};
FORCEINLINE BlobResourceAccessFlags operator|(BlobResourceAccessFlagBits a, BlobResourceAccessFlagBits b) {
    return static_cast<BlobResourceAccessFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
FORCEINLINE BlobResourceAccessFlags operator&(BlobResourceAccessFlagBits a, BlobResourceAccessFlagBits b) {
    return static_cast<BlobResourceAccessFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
FORCEINLINE BlobResourceAccessFlags operator|(BlobResourceAccessFlags a, BlobResourceAccessFlagBits b) {
    return static_cast<BlobResourceAccessFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
FORCEINLINE BlobResourceAccessFlags operator&(BlobResourceAccessFlags a, BlobResourceAccessFlagBits b) {
    return static_cast<BlobResourceAccessFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
FORCEINLINE BlobResourceAccessFlags operator|(BlobResourceAccessFlagBits a, BlobResourceAccessFlags b) {
    return static_cast<BlobResourceAccessFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
FORCEINLINE BlobResourceAccessFlags operator&(BlobResourceAccessFlagBits a, BlobResourceAccessFlags b) {
    return static_cast<BlobResourceAccessFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

MI_NAMESPACE_END
#endif //MIRENDERER_CORE_TYPES_H
