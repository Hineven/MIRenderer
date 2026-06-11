/*
 * Created: 2025/3/8
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include "rdg/rdg_param.h"
#include "rdg/rdg_global_memory_collector.h"
#include "core/crc.h"

MI_NAMESPACE_BEGIN

namespace details {
    bool zzFinalizeParams(std::vector<RDGShaderParamInfo> &params) {
        uint32_t vertex_buffer_index = 0;
        uint32_t vertex_attribute_index = 0;
        uint32_t render_target_index = 0;
        for (auto & e : params) {
            if (e.type == RHIParamType::kVertexBuffer) {
                // Finalize vertex buffer index now
                e.cpp_extra.vertex_buffer_info->index = vertex_buffer_index++;
            }
            if (e.type == RHIParamType::kVertexAttribute) {
                // Finalize vertex attribute index now
                e.cpp_extra.vertex_attribute_info->attribute_index = vertex_attribute_index++;
            }
            if (e.type == RHIParamType::kRenderTarget) {
                // Finalize render target index now
                if (!IsDepthStencilPixelFormat(e.cpp_extra.render_targets_info->format))
                    e.cpp_extra.render_targets_info->target_index = render_target_index++;
                else e.cpp_extra.render_targets_info->target_index = UINT32_MAX; // stands for depth stencil
            }
        }
        // Do some simple validation
        bool index_present = false;
        std::vector<bool> used_vertex_buffer;
        used_vertex_buffer.resize(vertex_buffer_index, false);
        for (auto & e : params) {
            if (e.type == RHIParamType::kVertexAttribute) {
                if (e.cpp_extra.vertex_attribute_info->buffer_index >= vertex_buffer_index) {
                    MI_LOG(MIInfraLogType::kError,
                        "Invalid vertex attribute buffer index {} is out of range (vertex buffer count {}), attribute name {}).",
                        e.cpp_extra.vertex_attribute_info->buffer_index, vertex_buffer_index, e.name);
                    return false;
                } else {
                    used_vertex_buffer[e.cpp_extra.vertex_attribute_info->buffer_index] = true;
                }
            }
            if (e.type == RHIParamType::kIndexBuffer) {
                if (!index_present) index_present = true;
                else {
                    MI_LOG(MIInfraLogType::kError,
                        "More than one index buffer is provided. (provided {}).", e.name
                    );
                }
            }
        }
        for (int i = 0; i < (int)used_vertex_buffer.size(); i++) {
            if (!used_vertex_buffer[i]) {
                MI_LOG(MIInfraLogType::kWarning,
                    "Vertex buffer index {} is not used by any vertex attribute.", i
                );
            }
        }
        return true;
    }

    void zzFinalizeTopLevelParamsStructInfo(RDGShaderParamStructAndSizeInfo *info) {
        std::vector<RDGShaderParameterLocation> storage_buffers, uniform_buffers,
            uavs, srvs, samplers, acceleration_structures, vertex_buffers, vertex_attributes,
            render_targets;
        RDGShaderParameterLocation index_buffer {};
        {
            for (auto & e : info->cpp_members) {
                uint32_t cpp_offset = e.cpp_offset;
                if (e.type == RHIParamType::kStorageBuffer) {
                    storage_buffers.emplace_back(&e, cpp_offset, 0);
                } else if (e.type == RHIParamType::kUniformBuffer) {
                    uniform_buffers.emplace_back(&e, cpp_offset, e.size);
                } else if (e.type == RHIParamType::kUAVTexture || e.type == RHIParamType::kUAVTextureArray) {
                    uavs.emplace_back(&e, cpp_offset, 0);
                } else if (e.type == RHIParamType::kSRVTexture || e.type == RHIParamType::kSRVTextureArray) {
                    srvs.emplace_back(&e, cpp_offset, 0);
                } else if (e.type == RHIParamType::kVertexBuffer) {
                    vertex_buffers.emplace_back(&e, cpp_offset, 0);
                } else if (e.type == RHIParamType::kVertexAttribute) {
                    vertex_attributes.emplace_back(&e, cpp_offset, 0);
                } else if (e.type == RHIParamType::kIndexBuffer) {
                    index_buffer = {&e, cpp_offset, 0};
                } else if (e.type == RHIParamType::kRenderTarget) {
                    render_targets.emplace_back(&e, cpp_offset, 0);
                } else if (e.type == RHIParamType::kAccelerationStructure){
                    acceleration_structures.emplace_back(&e, cpp_offset, 0);
                } else if (e.type == RHIParamType::kSampler) {
                    samplers.emplace_back(&e, cpp_offset, 0);
                } else if (e.type == RHIParamType::kPushConstant) {
                    // At most one push constant per struct. Only its SIZE matters (for root signature
                    // sizing and reflection validation); the VALUE is supplied per-dispatch.
                    mi_assert(!info->push_constant_.info,
                        "Only one push constant per shader parameter struct is allowed");
                    info->push_constant_ = {&e, cpp_offset, e.size};
                } else {
                    assert(false && "Unimplemented");
                }
            }
        }
        {
            if (!storage_buffers.empty()) {
                info->storage_buffers_ = std::span(RDGGlobalMemoryCollector::Get().NewArray<RDGShaderParameterLocation>(storage_buffers.size()), storage_buffers.size());
                std::copy(storage_buffers.begin(), storage_buffers.end(), info->storage_buffers_.begin());
            } else info->storage_buffers_ = {};
            if (!uniform_buffers.empty()) {
                info->uniform_buffers_ = std::span(RDGGlobalMemoryCollector::Get().NewArray<RDGShaderParameterLocation>(uniform_buffers.size()), uniform_buffers.size());
                std::copy(uniform_buffers.begin(), uniform_buffers.end(), info->uniform_buffers_.begin());
            } else info->uniform_buffers_ = {};
            if (!uavs.empty()) {
                info->uavs_ = std::span(RDGGlobalMemoryCollector::Get().NewArray<RDGShaderParameterLocation>(uavs.size()), uavs.size());
                std::copy(uavs.begin(), uavs.end(), info->uavs_.begin());
            } else info->uavs_ = {};
            if (!srvs.empty()) {
                info->srvs_ = std::span(RDGGlobalMemoryCollector::Get().NewArray<RDGShaderParameterLocation>(srvs.size()), srvs.size());
                std::copy(srvs.begin(), srvs.end(), info->srvs_.begin());
            } else info->srvs_ = {};
            if (!samplers.empty()) {
                info->samplers_ = std::span(RDGGlobalMemoryCollector::Get().NewArray<RDGShaderParameterLocation>(samplers.size()), samplers.size());
                std::copy(samplers.begin(), samplers.end(), info->samplers_.begin());
            } else info->samplers_ = {};
            if (!acceleration_structures.empty()) {
                info->acceleration_structures_ = std::span(RDGGlobalMemoryCollector::Get().NewArray<RDGShaderParameterLocation>(acceleration_structures.size()), acceleration_structures.size());
                std::copy(acceleration_structures.begin(), acceleration_structures.end(), info->acceleration_structures_.begin());
            } else info->acceleration_structures_ = {};
            if (!vertex_buffers.empty()) {
                info->render_pass_info_.vertex_buffers_ = std::span(RDGGlobalMemoryCollector::Get().NewArray<RDGShaderParameterLocation>(vertex_buffers.size()), vertex_buffers.size());
                std::copy(vertex_buffers.begin(), vertex_buffers.end(), info->render_pass_info_.vertex_buffers_.begin());
            } else info->render_pass_info_.vertex_buffers_ = {};
            if (!vertex_attributes.empty()) {
                info->render_pass_info_.vertex_attributes_ = std::span(RDGGlobalMemoryCollector::Get().NewArray<RDGShaderParameterLocation>(vertex_attributes.size()), vertex_attributes.size());
                std::copy(vertex_attributes.begin(), vertex_attributes.end(), info->render_pass_info_.vertex_attributes_.begin());
            } else info->render_pass_info_.vertex_attributes_ = {};
            if (!render_targets.empty()) {
                info->render_pass_info_.render_targets_ = std::span(RDGGlobalMemoryCollector::Get().NewArray<RDGShaderParameterLocation>(render_targets.size()), render_targets.size());
                std::copy(render_targets.begin(), render_targets.end(), info->render_pass_info_.render_targets_.begin());
            } else info->render_pass_info_.render_targets_ = {};
            if (index_buffer.info) {
                info->render_pass_info_.index_buffer_ = index_buffer;
            } else info->render_pass_info_.index_buffer_ = {};
        }
        // Some late validations
        {
            for (auto [i, e] : std::views::enumerate(info->render_pass_info_.vertex_attributes_)) {
                auto component_size = GetVertexAttributeFormatSize(e.info->cpp_extra.vertex_attribute_info->format);
                uint32_t end_offset = e.info->cpp_extra.vertex_attribute_info->offset + component_size;
                auto vbuf_index = e.info->cpp_extra.vertex_attribute_info->buffer_index;
                auto vbuf_stride = info->render_pass_info_.vertex_buffers_[vbuf_index].info->cpp_extra.vertex_buffer_info->stride;
                if (end_offset > vbuf_stride) {
                    MI_LOG(MIInfraLogType::kWarning,
                        "Vertex attribute {} have end offset {}, which exceeds the "
                        "stride of vertex buffer {}, which is {}.",
                        i, end_offset, vbuf_index, vbuf_stride
                    );
                }
            }
        }
    }

}

MI_NAMESPACE_END
