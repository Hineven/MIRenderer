/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_pass.h"

#include <rdg/rdg_param.h>
#include <rdg/rdg_shader.h>

#include "rhi/rhi_pipeline.h"

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

static RHIPipelineStageFlags GetTextureStagesFromUsage (RDGPassType pass_type, RDGTextureUsageType usage) {
    RHIPipelineStageFlags stages = {};
    switch (usage) {
        case RDGTextureUsageType::kTransferRead:
        case RDGTextureUsageType::kTransferWrite:
            stages = RHIPipelineStageFlagBits::kTransfer;
            break;
        case RDGTextureUsageType::kOverwriteOutputAttachment:
        case RDGTextureUsageType::kOutputAttachment:
            stages = RHIPipelineStageFlagBits::kFramebufferOutput;
            break;
        case RDGTextureUsageType::kDepthStencilAttachment:
            stages = RHIPipelineStageFlagBits::kFramebufferOutput | RHIPipelineStageFlagBits::kFragment;
            break;
        case RDGTextureUsageType::kReadonlyDepthStencilAttachment:
            stages = RHIPipelineStageFlagBits::kFragment;
            break;
        case RDGTextureUsageType::kShaderRead:
        case RDGTextureUsageType::kShaderReadWrite:
            if (pass_type == RDGPassType::kCompute) {
                stages = RHIPipelineStageFlagBits::kCompute;
            } else if (pass_type == RDGPassType::kGraphics) {
                stages = RHIPipelineStageFlagBits::kAllGraphics;
            } else if (pass_type == RDGPassType::kRayTracing) {
                stages = RHIPipelineStageFlagBits::kRayTracing;
            } else if (pass_type == RDGPassType::kGeneric) {
                stages = RHIPipelineStageFlagBits::kAll;
            } else {
                assert(false && "Unsupported RDGPassType for shader read texture.");
            }
            break;

        default:
            assert("RDGPass::AddTexture: unidentified usage.");
    }
    return stages;
}

static RHIGPUAccessFlags GetTextureAccessFromUsage (RDGTextureUsageType usage) {
    if (usage == RDGTextureUsageType::kShaderRead) {
        // The barrier-chain rule allows us to replace the access mask with the new one.
        // For example, Write, Read, Read produces a W-R barrier and a R-R barrier.
        // The R-R barrier may be incorrect if it is standalone, but it is correct if it's following the
        // W-R barrier (chaining up, see Vulkan Barrier Chain).
        return RHIGPUAccessFlagBits::kShaderRead;
    } else if (usage == RDGTextureUsageType::kShaderReadWrite) {
        return RHIGPUAccessFlagBits::kShaderRead | RHIGPUAccessFlagBits::kShaderWrite;
    } else if (usage == RDGTextureUsageType::kOutputAttachment) {
        // Read for (potentially) alpha blending
        return RHIGPUAccessFlagBits::kColorAttachmentRead | RHIGPUAccessFlagBits::kColorAttachmentWrite;
    } else if (usage == RDGTextureUsageType::kOverwriteOutputAttachment) {
        return RHIGPUAccessFlagBits::kColorAttachmentWrite;
    } else if (usage == RDGTextureUsageType::kDepthStencilAttachment) {
        return RHIGPUAccessFlagBits::kDepthStencilRead | RHIGPUAccessFlagBits::kDepthStencilWrite;
    } else if (usage == RDGTextureUsageType::kReadonlyDepthStencilAttachment) {
        return RHIGPUAccessFlagBits::kDepthStencilRead;
    } else if (usage == RDGTextureUsageType::kTransferRead) {
        return RHIGPUAccessFlagBits::kTransferRead;
    } else if (usage == RDGTextureUsageType::kTransferWrite) {
        return RHIGPUAccessFlagBits::kTransferWrite;
    } else if (usage == RDGTextureUsageType::kNone) {
        return RHIGPUAccessFlagBits::kNone;
    } else {
        assert(false);
        return RHIGPUAccessFlagBits::kNone; // Default return to avoid compiler warnings
    }
}
static RHITextureLayoutType GetTextureLayoutFromUsage (RDGTextureUsageType usage) {
    if (usage == RDGTextureUsageType::kShaderRead) {
        return RHITextureLayoutType::kShaderReadOnlyOptimal;
    } else if (usage == RDGTextureUsageType::kShaderReadWrite) {
        return RHITextureLayoutType::kGeneral;
    } else if (usage == RDGTextureUsageType::kOutputAttachment
        || usage == RDGTextureUsageType::kOverwriteOutputAttachment) {
        return RHITextureLayoutType::kColorAttachment;
    } else if (usage == RDGTextureUsageType::kDepthStencilAttachment
    || usage == RDGTextureUsageType::kReadonlyDepthStencilAttachment) {
        return RHITextureLayoutType::kDepthStencilAttachment;
    } else if (usage == RDGTextureUsageType::kTransferRead) {
        return RHITextureLayoutType::kTransferSrcOptimal;
    } else if (usage == RDGTextureUsageType::kTransferWrite) {
        return RHITextureLayoutType::kTransferDstOptimal;
    } else if (usage == RDGTextureUsageType::kNone) {
        return RHITextureLayoutType::kUndefined;
    } else {
        assert(false);
        return RHITextureLayoutType::kUndefined; // Default return to avoid compiler warnings
    }
}

RDGPass * RDGPass::AddTexture(RDGTexture *texture, RDGTextureUsageType usage, RHIPipelineStageFlags stages) {
    if (!texture) return this; // Do nothing if the texture is null
    if (usage == RDGTextureUsageType::kNone) {
        // If the usage is kNone, we don't care about the texture.
        return this;
    }
    auto layout = GetTextureLayoutFromUsage(usage);
    auto access = GetTextureAccessFromUsage(usage);
    if (stages == RHIPipelineStageFlagBits::kNone) {
        // If the usage stages are not specified, auto-detect them.
        stages = GetTextureStagesFromUsage(GetType(), usage);
    }
    AddTexture(texture, layout, access, stages);
    return this;
}

RDGPass * RDGPass::AddTexture(RDGTexture *texture, RHITextureLayoutType layout,
    RHIGPUAccessFlags access, RHIPipelineStageFlags stages) {
    if (!texture) return this; // Do nothing if the texture is null
    if (access & RHIGPUAccessFlagBits::kRead) compiled_.in_textures.emplace_back(texture);
    if (access & RHIGPUAccessFlagBits::kWrite) compiled_.out_textures.emplace_back(texture);
    assert(stages != RHIPipelineStageFlagBits::kNone);
    compiled_.used_textures.emplace_back(layout, access, stages, texture);
    if (access & RHIGPUAccessFlagBits::kRead) {
        compiled_.in_textures.emplace_back(texture);
    }
    if (access & RHIGPUAccessFlagBits::kWrite) {
        compiled_.out_textures.emplace_back(texture);
    }
    return this;
}


RDGPass * RDGPass::AddBuffer(RDGBuffer *buffer, RHIGPUAccessFlags access, RHIPipelineStageFlags usage_stages) {
    if (!buffer) return this; // Do nothing if the buffer is null
    if (access & RHIGPUAccessFlagBits::kRead) compiled_.in_buffers.emplace_back(buffer);
    if (access & RHIGPUAccessFlagBits::kWrite) compiled_.out_buffers.emplace_back(buffer);
    RHIPipelineStageFlags stages = usage_stages;
    if (stages == RHIPipelineStageFlagBits::kNone) {
        // If the usage stages are not specified, auto-detect them.
        if (GetType() == RDGPassType::kCompute) {
            if (access & RHIGPUAccessFlagBits::kShaderRW) {
                stages = RHIPipelineStageFlagBits::kCompute;
            }
            if (access & RHIGPUAccessFlagBits::kIndirectCommandRead) {
                stages = stages | RHIPipelineStageFlagBits::kIndirect;
            }
        } else if (GetType() == RDGPassType::kGraphics) {
            if (access & RHIGPUAccessFlagBits::kShaderRW) {
                stages = RHIPipelineStageFlagBits::kAllGraphics;
            }
            if (access & (RHIGPUAccessFlagBits::kVertexAttributeRead | RHIGPUAccessFlagBits::kIndexRead)) {
                stages = stages | RHIPipelineStageFlagBits::kVertex;
            }
            if (access & RHIGPUAccessFlagBits::kColorAttachmentRW) {
                stages = stages | RHIPipelineStageFlagBits::kFramebufferOutput;
            }
            if (access & RHIGPUAccessFlagBits::kDepthStencilRW) {
                stages = stages | RHIPipelineStageFlagBits::kFramebufferOutput | RHIPipelineStageFlagBits::kFragment;
            }
            if (access & RHIGPUAccessFlagBits::kIndirectCommandRead) {
                stages = stages | RHIPipelineStageFlagBits::kIndirect;
            }
        } else if (GetType() == RDGPassType::kRayTracing) {
            if (access & RHIGPUAccessFlagBits::kShaderRW) {
                stages = RHIPipelineStageFlagBits::kRayTracing;
            }
            if (access & RHIGPUAccessFlagBits::kIndirectCommandRead) {
                stages = stages | RHIPipelineStageFlagBits::kIndirect;
            }
        } else if (GetType() == RDGPassType::kGeneric) {
            stages = RHIPipelineStageFlagBits::kAll;
            if (access & RHIGPUAccessFlagBits::kIndirectCommandRead) {
                stages = stages | RHIPipelineStageFlagBits::kIndirect;
            }
        } else {
            assert(false && "Unsupported RDGPassType for buffer.");
        }
    }
    compiled_.used_buffers.emplace_back(access, stages, buffer);
    if (access & RHIGPUAccessFlagBits::kRead) {
        compiled_.in_buffers.emplace_back(buffer);
    }
    if (access & RHIGPUAccessFlagBits::kWrite) {
        compiled_.out_buffers.emplace_back(buffer);
    }
    return this;
}

RDGPass *RDGPass::AddAS_NoAutomaticBarrier(RHIAccelerationStructure *as, RHIGPUAccessFlags access, RHIPipelineStageFlags usage_stages) {
    if (!as) return this; // Do nothing if the buffer is null
    if (access & RHIGPUAccessFlagBits::kRead) compiled_.in_acceleration_structures.emplace_back(as);
    if (access & RHIGPUAccessFlagBits::kWrite) compiled_.out_acceleration_structures.emplace_back(as);
    RHIPipelineStageFlags stages = usage_stages;
    if (stages == RHIPipelineStageFlagBits::kNone) {
        // If the usage stages are not specified, auto-detect them.
        if (GetType() == RDGPassType::kCompute) {
            stages = RHIPipelineStageFlagBits::kCompute;
        } else if (GetType() == RDGPassType::kGraphics) {
            assert(false && "RDGPassType::kGraphics does not support acceleration structures.");
        } else if (GetType() == RDGPassType::kRayTracing) {
            stages = RHIPipelineStageFlagBits::kRayTracing;
        } else if (GetType() == RDGPassType::kGeneric) {
            if (access & RHIGPUAccessFlagBits::kRead)
                stages = stages | RHIPipelineStageFlagBits::kRayTracing | RHIPipelineStageFlagBits::kCompute
                | RHIPipelineStageFlagBits::kAccelerationStructureBuild | RHIPipelineStageFlagBits::kTransfer;
            if (access & RHIGPUAccessFlagBits::kWrite)
                stages = stages | RHIPipelineStageFlagBits::kAccelerationStructureBuild | RHIPipelineStageFlagBits::kTransfer;
        } else {
            assert(false && "Unsupported RDGPassType for acceleration structure.");
        }
    }
    compiled_.used_acceleration_structures.emplace_back(access, stages, as);
    return this;
}


void RDGPass::Compile() {


    // TODO: It is possible to reflect more accurate shader stages for the textures and buffers.

    assert(!is_compiled_ && "Each pass may only be compiled once.");
    // No need to compile as we have no shader parameters present
    if (shader_param_struct_info_) {
        assert(shader_param_data_);
        // Iterate through all shader parameters using reflection
        for (const auto & field : shader_param_struct_info_->cpp_members) {
            const void* field_data = static_cast<const char*>(shader_param_data_) + field.cpp_offset;
            // Check resource type
            if (field.type == RHIParamType::kSRVTexture || field.type == RHIParamType::kSRVTextureArray) { // SRV
                RDGTexture* texture = *static_cast<RDGTexture* const*>(field_data);
                if (!texture) continue ;
                if (RDGParameter_IsUnsetPointer(texture)) {
                    MI_WARN("Pass {}: Unset parameter pointer {}."
                            "If you really want it set to null in the pass, "
                            "use nullptr as initial value to disable this warning.",
                            name_, field.name);
                    continue;
                }
                AddTexture(texture, RDGTextureUsageType::kShaderRead);
            }
            else if (field.type == RHIParamType::kUAVTexture || field.type == RHIParamType::kUAVTextureArray) { // UAV
                RDGTexture* texture = *static_cast<RDGTexture* const*>(field_data);
                if (!texture) continue;
                if (RDGParameter_IsUnsetPointer(texture)) {
                    MI_WARN("Pass {}: Unset parameter pointer {}."
                            "If you really want it set to null in the pass, "
                            "use nullptr as initial value to disable this warning.",
                            name_, field.name);
                    continue;
                }
                AddTexture(texture, RDGTextureUsageType::kShaderReadWrite);
            } else if (field.type == RHIParamType::kStorageBuffer) { // Storage buffer
                RDGBuffer* buffer = *static_cast<RDGBuffer* const*>(field_data);
                if (!buffer) continue;
                if (RDGParameter_IsUnsetPointer(buffer)) {
                    MI_WARN("Pass {}: Unset parameter pointer {}."
                            "If you really want it set to null in the pass, "
                            "use nullptr as initial value to disable this warning.",
                            name_, field.name);
                    continue;
                }
                AddBuffer(buffer, field.access_flags);
            }
            else if (field.type == RHIParamType::kVertexBuffer
                || field.type == RHIParamType::kIndexBuffer
                || field.type == RHIParamType::kDispatchCommand) { // Vertex / index/ dispatch command
                RDGBuffer * buffer = *static_cast<RDGBuffer* const*>(field_data);
                if (!buffer) continue;
                if (RDGParameter_IsUnsetPointer(buffer)) {
                    MI_WARN("Pass {}: Unset parameter pointer {}."
                            "If you really want it set to null in the pass, "
                            "use nullptr as initial value to disable this warning.",
                            name_, field.name);
                    continue;
                }
                RHIGPUAccessFlags access_flags = field.access_flags;
                AddBuffer(buffer, access_flags);
            } else if (field.type == RHIParamType::kVertexAttribute) {
                // Do nothing
            } else if (field.type == RHIParamType::kSampler) {
                // Do nothing
            } else if (field.type == RHIParamType::kUniformBuffer) {
                // The field value is actually a pointer to a UB struct.
                // Device ub is allocated when the graph is executed. And dependencies is
                // generated at runtime. So, do nothing here.
            } else if (field.type == RHIParamType::kRenderTarget) {
                const RDGShaderRenderTargetParameter & render_target = *static_cast<RDGShaderRenderTargetParameter const*>(field_data);
                if (!render_target.texture) continue ;
                if (RDGParameter_IsUnsetPointer(render_target.texture)) {
                    MI_WARN("Pass {}: Unset parameter pointer {}."
                            "If you really want it set to null in the pass, "
                            "use nullptr as initial value to disable this warning.",
                            name_, field.name);
                    continue;
                }
                if (IsDepthStencilPixelFormat(field.cpp_extra.render_targets_info->format)) {
                    bool read = false, write = false;
                    if (render_target.load_op == RHILoadOpType::kLoad) read = true;
                    if (render_target.store_op == RHIStoreOpType::kStore) write = true;
                    if (!read && !write) {
                        MI_WARN("Pass {}: Render target {} is not used in the pass, "
                                "but it is a depth/stencil texture. "
                                "If you really want it set to null in the pass, "
                                "use nullptr as initial value to disable this warning.",
                                name_, field.name);
                        continue;
                    }
                    if (read && write) {
                        AddTexture(render_target.texture, RDGTextureUsageType::kDepthStencilAttachment);
                    } else if (read) {
                        AddTexture(render_target.texture, RDGTextureUsageType::kReadonlyDepthStencilAttachment);
                    } else {
                        // TODO add a new type for the overwrite case
                        AddTexture(render_target.texture, RDGTextureUsageType::kDepthStencilAttachment);
                    }
                } else {
                    bool read = false, write = false;
                    if (render_target.load_op == RHILoadOpType::kLoad) read = true;
                    if (render_target.store_op == RHIStoreOpType::kStore) write = true;
                    if (!write) {
                        MI_WARN("Pass {}: Render target {} is non-writable. "
                                "If you really want it set to null in the pass,"
                                "use nullptr as initial value to disable this warning.",
                                name_, field.name);
                        continue;
                    }
                    if (read) {
                        AddTexture(render_target.texture, RDGTextureUsageType::kOutputAttachment);
                    } else {
                        AddTexture(render_target.texture, RDGTextureUsageType::kOverwriteOutputAttachment);
                    }
                }
            } else {
                assert(false && "Unsupported parameter type.");
            }
        }
    }
    is_compiled_ = true;
}

MI_NAMESPACE_END