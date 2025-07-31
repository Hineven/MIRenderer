/*
 * Created: 2025/7/5
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RHI_TYPE_HELPERS_H
#define RHI_TYPE_HELPERS_H

#include <rhi/rhi_types.h>
MI_NAMESPACE_BEGIN

FORCEINLINE RHIGPUAccessFlags GetReadAccessFlags(RHIGPUAccessFlags access) {
    return access & RHIGPUAccessFlagBits::kRead;
}

FORCEINLINE RHIGPUAccessFlags GetWriteAccessFlags(RHIGPUAccessFlags access) {
    return access & RHIGPUAccessFlagBits::kWrite;
}

FORCEINLINE RHIPipelineStageFlags GetStageFlagsFromShaderFrequencies (RHIShaderFrequencyFlags freq) {
    RHIPipelineStageFlags flags = RHIPipelineStageFlagBits::kNone;
    if (freq & RHIShaderFrequencyFlagBits::kVertex) {
        flags = flags | RHIPipelineStageFlagBits::kVertex;
        freq = freq ^ RHIShaderFrequencyFlagBits::kVertex; // Remove vertex from the frequency
    }
    if (freq & RHIShaderFrequencyFlagBits::kFragment) {
        flags = flags | RHIPipelineStageFlagBits::kFragment;
        freq = freq ^ RHIShaderFrequencyFlagBits::kFragment;
    }
    if (freq & RHIShaderFrequencyFlagBits::kGeometry) {
        flags = flags | RHIPipelineStageFlagBits::kGeometry;
        freq = freq ^ RHIShaderFrequencyFlagBits::kGeometry;
    }
    if (freq & RHIShaderFrequencyFlagBits::kTask) {
        flags = flags | RHIPipelineStageFlagBits::kTaskMesh;
        freq = freq ^ RHIShaderFrequencyFlagBits::kTask;
    }
    if (freq & RHIShaderFrequencyFlagBits::kMesh) {
        flags = flags | RHIPipelineStageFlagBits::kTaskMesh;
        freq = freq ^ RHIShaderFrequencyFlagBits::kMesh;
    }
    if (freq & RHIShaderFrequencyFlagBits::kCompute) {
        flags = flags | RHIPipelineStageFlagBits::kCompute;
        freq = freq ^ RHIShaderFrequencyFlagBits::kCompute;
    }
    if (freq & RHIShaderFrequencyFlagBits::kRayTracing) {
        flags = flags | RHIPipelineStageFlagBits::kRayTracing;
        freq = freq & (~RHIShaderFrequencyFlagBits::kRayTracing);
    }
    assert(freq == RHIShaderFrequencyFlagBits::kNone && "Unknown shader frequency flags in GetStageFlagsFromShaderFrequencies().");
    return flags;
}

MI_NAMESPACE_END

#endif //RHI_TYPE_HELPERS_H
