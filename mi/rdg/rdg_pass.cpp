/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_pass.h"

#include <rdg/rdg_param.h>
#include <rdg/rdg_shader.h>

MI_NAMESPACE_BEGIN
    RDGPass::RDGPass(
        std::string name,
        int index,
        RDGPassType pass_type,
        RDGPassFlags flags,
        RDGPassLambda && pass
    ) : name_(name),
        index_(index),
        type_(pass_type),
        flags_(flags),
        pass_(std::move(pass)) {
}

RDGPass::~RDGPass() {
}

RDGPass * RDGPass::AddTexture(RDGTexture *texture, RDGTextureUsageType usage) {
    // assert(!is_compiled_);
    if (usage != RDGTextureUsageType::kTransferDst) {
        compiled_.in_textures.emplace_back(texture);
    }
    switch (usage) {
        case RDGTextureUsageType::kShaderReadWrite:
        case RDGTextureUsageType::kOutputAttachment:
        case RDGTextureUsageType::kDepthStencilAttachment:
        case RDGTextureUsageType::kTransferDst:
            compiled_.out_textures.emplace_back(texture);
            break;
        default:
            // do nothing
            break;
    }
    compiled_.used_textures.emplace_back(usage, texture);
    return this;
}

RDGPass * RDGPass::AddBuffer(RDGBuffer *buffer, RHIGPUAccessFlags access) {
    // assert(!is_compiled_);
    if (access & RHIGPUAccessFlagBits::kRead) compiled_.in_buffers.emplace_back(buffer);
    if (access & RHIGPUAccessFlagBits::kWrite) compiled_.out_buffers.emplace_back(buffer);
    compiled_.used_buffers.emplace_back(access, buffer);
    return this;
}

void RDGPass::Compile() {

    assert(!is_compiled_ && "Each pass may only be compiled once.");
    // No need to compile as we have no shader parameters present
    if (shader_param_struct_info_) {
        assert(shader_param_data_);
        // Iterate through all shader parameters using reflection
        for (const auto & field : shader_param_struct_info_->cpp_members) {
            const void* field_data = static_cast<const char*>(shader_param_data_) + field.cpp_offset;

            // Check resource type
            if (field.type == RHIParamType::kSRVTexture) { // SRV
                RDGTexture* texture = *static_cast<RDGTexture* const*>(field_data);
                if (!texture) continue ;
                if (RDGParameter_IsUnsetPointer(texture)) {
                    MI_WARN("Pass {}: Unset parameter pointer {}."
                            "If you want manually set it in the pass, "
                            "use RDGParameter_UnsetPointer as initial value to disable this warning.",
                            name_, field.name);
                    continue;
                }
                AddTexture(texture, RDGTextureUsageType::kShaderRead);
            }
            else if (field.type == RHIParamType::kUAVTexture) { // UAV
                RDGTexture* texture = *static_cast<RDGTexture* const*>(field_data);
                if (!texture) continue;
                if (RDGParameter_IsUnsetPointer(texture)) {
                    MI_WARN("Pass {}: Unset parameter pointer {}."
                            "If you want manually set it in the pass, "
                            "use RDGParameter_UnsetPointer as initial value to disable this warning.",
                            name_, field.name);
                    continue;
                }
                AddTexture(texture, RDGTextureUsageType::kShaderReadWrite);
            } else if (field.type == RHIParamType::kStorageBuffer) { // Storage buffer
                RDGBuffer* buffer = *static_cast<RDGBuffer* const*>(field_data);
                if (!buffer) continue;
                if (RDGParameter_IsUnsetPointer(buffer)) {
                    MI_WARN("Pass {}: Unset parameter pointer {}."
                            "If you want manually set it in the pass, "
                            "use RDGParameter_UnsetPointer as initial value to disable this warning.",
                            name_, field.name);
                    continue;
                }
                auto usage = RDGBufferUsage{{}, buffer};
                AddBuffer(buffer, field.access_flags);
            }
            else if (field.type == RHIParamType::kUniformBuffer
                || field.type == RHIParamType::kVertexBuffer
                || field.type == RHIParamType::kIndexBuffer
                || field.type == RHIParamType::kDispatchCommand) { // Vertex / index/ dispatch command
                RDGBuffer * buffer = *static_cast<RDGBuffer* const*>(field_data);
                if (!buffer) continue;
                if (RDGParameter_IsUnsetPointer(buffer)) {
                    MI_WARN("Pass {}: Unset parameter pointer {}."
                            "If you want manually set it in the pass, "
                            "use RDGParameter_UnsetPointer as initial value to disable this warning.",
                            name_, field.name);
                    continue;
                }
                AddBuffer(buffer, RHIGPUAccessFlagBits::kRead);
            } else if (field.type == RHIParamType::kRenderPass) {
                // Render pass
                auto ptr = *static_cast<void* const*>(field_data);
                auto pass_info = field.cpp_imported_struct_info.cpp_struct_info;
                for (auto e : pass_info->render_targets_) {
                    RDGTexture * texture = *(RDGTexture**)((std::byte*)ptr + e.cpp_offset);
                    if (!texture) continue;
                    if (RDGParameter_IsUnsetPointer(texture)) {
                        MI_WARN("Pass {}: Unset parameter pointer {}."
                                "If you want manually set it in the pass, "
                                "use RDGParameter_UnsetPointer as initial value to disable this warning.",
                                name_, e.info->name);
                        continue;
                    }
                    auto usage = RDGTextureUsageType::kOutputAttachment;
                    if (UINT32_MAX == e.info->cpp_extra.render_targets_info->target_index) {
                        usage = RDGTextureUsageType::kDepthStencilAttachment;
                    }
                    AddTexture(texture, usage);
                }
            } else if (field.type == RHIParamType::kVertexAttribute) {
                // Do nothing
            } else if (field.type == RHIParamType::kBasic || field.type == RHIParamType::kStruct) {
                // Do nothing
            } else if (field.type == RHIParamType::kSampler) {
                // Do nothing
            } else {
                assert(false && "Unsupported parameter type.");
            }
        }
    }
    is_compiled_ = true;
}

MI_NAMESPACE_END