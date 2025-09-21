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

RDGPass * RDGPass::AddTextureH(RDGTexture *texture, RDGTextureUsageType usage, RHIPipelineStageFlags stages) {
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
    if (stages == RHIPipelineStageFlagBits::kNone || access == RHIGPUAccessFlagBits::kNone)
        return this; // If no access or stages are specified, do not add the texture.
    if (access & RHIGPUAccessFlagBits::kRead) compiled_.in_textures.emplace_back(texture);
    if (access & RHIGPUAccessFlagBits::kWrite) compiled_.out_textures.emplace_back(texture);
    used_textures.emplace_back(layout, access, stages, texture);
    return this;
}

RDGPass * RDGPass::AddBufferH(RDGBuffer *buffer, RHIGPUAccessFlags access, RHIPipelineStageFlags usage_stages) {
    if (!buffer) return this; // Do nothing if the buffer is null
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
    return AddBuffer(buffer, access, stages);
}

RDGPass *RDGPass::AddBuffer(RDGBuffer *buffer, RHIGPUAccessFlags access, RHIPipelineStageFlags stages) {
    if (access == RHIGPUAccessFlagBits::kNone || stages == RHIPipelineStageFlagBits::kNone) {
        // If the access is kNone, we don't care about the buffer.
        return this;
    }
    if (access & RHIGPUAccessFlagBits::kRead) compiled_.in_buffers.emplace_back(buffer);
    if (access & RHIGPUAccessFlagBits::kWrite) compiled_.out_buffers.emplace_back(buffer);
    used_buffers.emplace_back(access, stages, buffer);
    // if (access & RHIGPUAccessFlagBits::kRead) {
    //     compiled_.in_buffers.emplace_back(buffer);
    // }
    // if (access & RHIGPUAccessFlagBits::kWrite) {
    //     compiled_.out_buffers.emplace_back(buffer);
    // }
    return this;
}


RDGPass *RDGPass::AddASH_NoAutomaticBarrier(RHIAccelerationStructure *as, RHIGPUAccessFlags access, RHIPipelineStageFlags usage_stages) {
    if (!as) return this; // Do nothing if the buffer is null
    if (access == RHIGPUAccessFlagBits::kNone || usage_stages == RHIPipelineStageFlagBits::kNone) {
        // If the access is kNone, we don't care about the acceleration structure.
        return this;
    }
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
    used_acceleration_structures.emplace_back(access, stages, as);
    return this;
}


void RDGPass::PreCompile() {

    assert(!is_pre_compiled_ && "Each pass may only be pre compiled once.");
    // No need to compile as we have no shader parameters present
    if (shader_param_struct_info_) {
        assert(shader_param_data_);
        // Iterate through all shader parameters using reflection
        for (const auto & field : shader_param_struct_info_->cpp_members) {
            RHIPipelineStageFlags stages = RHIPipelineStageFlagBits::kNone;
            RHIGPUAccessFlags access = RHIGPUAccessFlagBits::kNone;
            // Initialize access flags with the shader reflection data
            if (shader_) {
                if (field.type == RHIParamType::kStorageBuffer
                    || field.type == RHIParamType::kSRVTexture
                    || field.type == RHIParamType::kSRVTextureArray
                    || field.type == RHIParamType::kUAVTexture
                    || field.type == RHIParamType::kUAVTextureArray
                    || field.type == RHIParamType::kAccelerationStructure
                    || field.type == RHIParamType::kSampler) {
                    // Query the actual usage of the resource statically reflected in the shader
                    auto shader_access = shader_->QueryShaderAccess(field.name);
                    stages = shader_access.stages;
                    access = shader_access.access;
                }
            }
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
                AddTexture(texture,
                    RHITextureLayoutType::kShaderReadOnlyOptimal,
                    // Sometimes there are storage read access (texture.Load())
                    RHIGPUAccessFlagBits::kShaderSampledRead | access,
                    stages
                );
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
                AddTexture(texture, RHITextureLayoutType::kGeneral,
                    access,
                    stages
                );
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
                AddBuffer(buffer, field.access_flags & access, stages);
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
                AddBufferH(buffer, field.access_flags);
            } else if (field.type == RHIParamType::kVertexAttribute) {
                // Do nothing
            } else if (field.type == RHIParamType::kSampler) {
                // Do nothing
            } else if (field.type == RHIParamType::kUniformBuffer) {
                // The field value is actually a pointer to a UB struct.
                // Device ub is allocated when the graph is executed. And dependencies are
                // generated at runtime. So, do nothing here.
            } else if (field.type == RHIParamType::kAccelerationStructure) {
                auto as = *static_cast<RHIAccelerationStructure* const*>(field_data);
                if (!as) continue;
                if (RDGParameter_IsUnsetPointer(as)) {
                    MI_WARN("Pass {}: Unset parameter pointer {}."
                            "If you really want it set to null in the pass, "
                            "use nullptr as initial value to disable this warning.",
                            name_, field.name);
                    continue;
                }
                AddASH_NoAutomaticBarrier(as, field.access_flags & access, stages);
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
                        AddTextureH(render_target.texture, RDGTextureUsageType::kDepthStencilAttachment);
                    } else if (read) {
                        AddTextureH(render_target.texture, RDGTextureUsageType::kReadonlyDepthStencilAttachment);
                    } else {
                        // TODO add a new type for the overwrite case
                        AddTextureH(render_target.texture, RDGTextureUsageType::kDepthStencilAttachment);
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
                        AddTextureH(render_target.texture, RDGTextureUsageType::kOutputAttachment);
                    } else {
                        AddTextureH(render_target.texture, RDGTextureUsageType::kOverwriteOutputAttachment);
                    }
                }
            } else {
                assert(false && "Unsupported parameter type.");
            }
        }
    }
    is_pre_compiled_ = true;
}

void RDGPass::Compile() {
    assert(!is_compiled_);
    // Detect resource aliasing for textures and spawn final relations
    // Textures
    {
        std::map<void*, RDGTextureUsage*> combined_textures;
        for (auto & e : used_textures) {
            auto it = combined_textures.find(e.texture.Raw());
            if (it != combined_textures.end()) {
                // If the texture is already in the map, merge the usage
                it->second->access |= e.access;
                it->second->stages |= e.stages;
                // Detect layout.
                if (it->second->access & RHIGPUAccessFlagBits::kShaderSampledRead
                && it->second->access & RHIGPUAccessFlagBits::kShaderStorageRW) {
                    // Downgrade to general layout to keep compatible with sampled read and storage rw
                    it->second->layout = RHITextureLayoutType::kGeneral;
                }
            } else {
                // Otherwise, insert a new entry
                combined_textures[e.texture.Raw()] = &e;
            }
        }
        // Add to compiled
        for (auto e : combined_textures) {
            auto & usage = *e.second;
            compiled_.textures.emplace_back(usage);
            if (usage.access & RHIGPUAccessFlagBits::kRead) {
                compiled_.in_textures.emplace_back(usage.texture.Raw());
            }
            if (usage.access & RHIGPUAccessFlagBits::kWrite) {
                compiled_.out_textures.emplace_back(usage.texture.Raw());
            }
            if (usage.access & RHIGPUAccessFlagBits::kShaderSampledRead
                && usage.access & RHIGPUAccessFlagBits::kShaderStorageRW) {
                // It's suggested to avoid using both sampled read and storage rw on the same texture.
                // Pop a warning
                MI_WARN("Pass {}, Texture {}: Using both sampled read and storage read/write on the same texture is not recommended. "
                        "This may cause performance downgrade.",
                        name_, usage.texture->GetName());
            }
        }
        used_textures.clear();
    }
    // Buffers
    {
        std::map<void*, RDGBufferUsage*> combined_buffers;
        for (auto & e : used_buffers) {
            auto it = combined_buffers.find(e.buffer.Raw());
            if (it != combined_buffers.end()) {
                it->second->access |= e.access;
                it->second->stages |= e.stages;
            } else {
                combined_buffers[e.buffer.Raw()] = &e;
            }
        }
        for (auto e : combined_buffers) {
            auto & usage = *e.second;
            compiled_.buffers.emplace_back(usage);
            if (usage.access & RHIGPUAccessFlagBits::kRead) {
                compiled_.in_buffers.emplace_back(usage.buffer.Raw());
            }
            if (usage.access & RHIGPUAccessFlagBits::kWrite) {
                compiled_.out_buffers.emplace_back(usage.buffer.Raw());
            }
        }
        used_buffers.clear();
    }
    // AS
    {
        std::map<void*, RDGASUsage*> combined_acceleration_structures;
        for (auto & e : used_acceleration_structures) {
            auto it = combined_acceleration_structures.find(e.as);
            if (it != combined_acceleration_structures.end()) {
                it->second->access |= e.access;
                it->second->stages |= e.stages;
            } else {
                combined_acceleration_structures[e.as] = &e;
            }
        }
        for (auto e : combined_acceleration_structures) {
            auto & usage = *e.second;
            compiled_.acceleration_structures.emplace_back(usage);
            if (usage.access & RHIGPUAccessFlagBits::kRead) {
                compiled_.in_acceleration_structures.emplace_back(usage.as);
            }
            if (usage.access & RHIGPUAccessFlagBits::kWrite) {
                compiled_.out_acceleration_structures.emplace_back(usage.as);
            }
        }
        used_acceleration_structures.clear();
    }
    is_compiled_ = true;
}


MI_NAMESPACE_END