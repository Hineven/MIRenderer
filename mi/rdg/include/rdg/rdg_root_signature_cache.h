/*
 * Created: 2026/4/24
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RDG_ROOT_SIGNATURE_CACHE_H
#define MI_RDG_ROOT_SIGNATURE_CACHE_H

#include <mutex>
#include <unordered_map>
#include "core/common.h"
#include "core/base.h"
#include "core/refcounted.h"
#include "rhi/rhi_root_signature.h"
#include "rhi/rhi_desc.h"
#include "core/util/unordered_hashing.h"

MI_NAMESPACE_BEGIN

struct RDGShaderParamStructAndSizeInfo;

struct RDGRootSignatureKey {
    // This is allocated from RDGGlobalMemoryCollector and is guaranteed to be valid during the whole execution of the program, 
    // so we can safely use the pointer as part of the key and do pointer comparison for equality check.
    const RDGShaderParamStructAndSizeInfo * param_info;
    uint32_t push_constant_size;

    FORCEINLINE bool operator==(const RDGRootSignatureKey & other) const {
        return param_info == other.param_info
            && push_constant_size == other.push_constant_size;
    }
};

struct RDGRootSignatureKeyHash {
    FORCEINLINE size_t operator()(const RDGRootSignatureKey & key) const {
        ZobristSetHashing hasher;
        hasher.Add(&key.param_info, sizeof(key.param_info));
        hasher.Add(&key.push_constant_size, sizeof(key.push_constant_size));
        return hasher.GetResult();
    }
};

class RDGShaderRootSignatureCache;

class RDGShaderRootSignatureKeeper : public RefCounted<true> {
public:
    RDGShaderRootSignatureKeeper(
        RHIPipelineRootSignatureRef root_sig,
        RDGRootSignatureKey key,
        RDGShaderRootSignatureCache & cache
    );
    ~RDGShaderRootSignatureKeeper() override;

    FORCEINLINE RHIPipelineRootSignature * GetRootSignature() const { return root_sig_.Raw(); }

private:
    RHIPipelineRootSignatureRef root_sig_;
    RDGRootSignatureKey key_;
    RDGShaderRootSignatureCache * cache_;
};

class RDGShaderRootSignatureCache : public NonCopyable, public NonMovable {
public:
    friend class RDGShaderRootSignatureKeeper;
    static RDGShaderRootSignatureCache & Get();

    TRef<RDGShaderRootSignatureKeeper> GetOrCreate(
        const RDGShaderParamStructAndSizeInfo * param_info,
        uint32_t push_constant_size
    );

    // Process deferred RHI resource releases. Must be called from the render thread.
    void FlushUnused();
    void Clear();

private:
    RDGShaderRootSignatureCache() = default;
    ~RDGShaderRootSignatureCache() { FlushUnused(); }

    void OnKeeperDestroyed(RDGRootSignatureKey key, RHIPipelineRootSignatureRef root_sig);

    std::mutex mutex_;
    std::mutex pending_mutex_;
    std::unordered_map<RDGRootSignatureKey, RDGShaderRootSignatureKeeper *, RDGRootSignatureKeyHash> cache_;
    std::vector<RHIPipelineRootSignatureRef> pending_releases_;
};

MI_NAMESPACE_END

#endif // MI_RDG_ROOT_SIGNATURE_CACHE_H
