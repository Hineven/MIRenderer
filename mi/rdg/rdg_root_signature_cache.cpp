/*
 * Created: 2026/4/24
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "rdg/rdg_root_signature_cache.h"
#include "rdg/rdg_param.h"
#include "rhi/rhi.h"
#include "core/crc.h"

MI_NAMESPACE_BEGIN

RDGShaderRootSignatureKeeper::RDGShaderRootSignatureKeeper(
    RHIPipelineRootSignatureRef root_sig,
    RDGRootSignatureKey key,
    RDGShaderRootSignatureCache & cache)
    : root_sig_(std::move(root_sig))
    , key_(key)
    , cache_(&cache)
{}

RDGShaderRootSignatureKeeper::~RDGShaderRootSignatureKeeper() {
    cache_->OnKeeperDestroyed(key_, std::move(root_sig_));
}

RDGShaderRootSignatureCache & RDGShaderRootSignatureCache::Get() {
    static RDGShaderRootSignatureCache instance;
    return instance;
}

static void FillRootSignatureDescFromParamInfo(
    const RDGShaderParamStructAndSizeInfo & param_info,
    uint32_t push_constant_size,
    RHIPipelineRootSignatureDesc & out_desc,
    std::vector<std::vector<uint32_t>> & crc_storage)
{
    out_desc.push_constant_size = push_constant_size;
    out_desc.num_resources[(uint32_t)RHIPipelineResourceType::kUniformBuffer] = (uint32_t)param_info.uniform_buffers_.size();
    out_desc.num_resources[(uint32_t)RHIPipelineResourceType::kStorageBuffer] = (uint32_t)param_info.storage_buffers_.size();
    out_desc.num_resources[(uint32_t)RHIPipelineResourceType::kUAV] = (uint32_t)param_info.uavs_.size();
    out_desc.num_resources[(uint32_t)RHIPipelineResourceType::kSRV] = (uint32_t)param_info.srvs_.size();
    out_desc.num_resources[(uint32_t)RHIPipelineResourceType::kSampler] = (uint32_t)param_info.samplers_.size();
    out_desc.num_resources[(uint32_t)RHIPipelineResourceType::kAccelerationStructure] = (uint32_t)param_info.acceleration_structures_.size();
    out_desc.num_resources[(uint32_t)RHIPipelineResourceType::kPartitionedAccelerationStructure] = (uint32_t)param_info.partitioned_acceleration_structures_.size();

    auto Fill = [&](std::span<const RDGShaderParameterLocation> locs, uint32_t type_index) {
        out_desc.type_names[type_index].count = (uint32_t)locs.size();
        if (!locs.empty()) {
            auto & storage = crc_storage.emplace_back(locs.size());
            for (uint32_t i = 0; i < locs.size(); i++) {
                storage[i] = CRC32String(locs[i].info->name.c_str());
            }
            out_desc.type_names[type_index].name_crcs = storage.data();
        }
    };
    Fill(param_info.uniform_buffers_, (uint32_t)RHIPipelineResourceType::kUniformBuffer);
    Fill(param_info.storage_buffers_, (uint32_t)RHIPipelineResourceType::kStorageBuffer);
    Fill(param_info.uavs_, (uint32_t)RHIPipelineResourceType::kUAV);
    Fill(param_info.srvs_, (uint32_t)RHIPipelineResourceType::kSRV);
    Fill(param_info.samplers_, (uint32_t)RHIPipelineResourceType::kSampler);
    Fill(param_info.acceleration_structures_, (uint32_t)RHIPipelineResourceType::kAccelerationStructure);
    Fill(param_info.partitioned_acceleration_structures_, (uint32_t)RHIPipelineResourceType::kPartitionedAccelerationStructure);
}

TRef<RDGShaderRootSignatureKeeper> RDGShaderRootSignatureCache::GetOrCreate(
    const RDGShaderParamStructAndSizeInfo * param_info,
    uint32_t push_constant_size)
{
    RDGRootSignatureKey key{param_info, push_constant_size};
    std::lock_guard lock(mutex_);
    {
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            auto * keeper = it->second;
            return TRef<RDGShaderRootSignatureKeeper>(keeper);
        }
    }

    RHIPipelineRootSignatureDesc desc {};
    std::vector<std::vector<uint32_t>> crc_storage;
    FillRootSignatureDescFromParamInfo(*param_info, push_constant_size, desc, crc_storage);
    auto rs = RHI::Get().CreateRootSignature(desc);

    auto * keeper = new RDGShaderRootSignatureKeeper(rs, key, *this);
    TRef<RDGShaderRootSignatureKeeper> result(keeper);

    cache_[key] = keeper;
    return result;
}

void RDGShaderRootSignatureCache::OnKeeperDestroyed(RDGRootSignatureKey key, RHIPipelineRootSignatureRef root_sig) {
    {
        std::lock_guard lock(mutex_);
        cache_.erase(key);
    }
    std::lock_guard lock(pending_mutex_);
    pending_releases_.push_back(std::move(root_sig));
}

void RDGShaderRootSignatureCache::FlushUnused() {
    std::vector<RHIPipelineRootSignatureRef> pending;
    {
        std::lock_guard lock(pending_mutex_);
        pending.swap(pending_releases_);
    }
    // TRefs released here on the render thread, DecRef on correct thread
    pending.clear();
}

void RDGShaderRootSignatureCache::Clear() {
    {
        std::lock_guard lock(mutex_);
        cache_.clear();
    }
    FlushUnused();
}

MI_NAMESPACE_END
