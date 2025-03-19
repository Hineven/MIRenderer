/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_pass.h"

#include <rdg/rdg_param.h>

MI_NAMESPACE_BEGIN
void RDGPass::GatherResourceAccesses() {
    // Enumerate the shader_param_data_ using reflection from shader_param_struct_info_, and gather accessed resources
    // Store them in in_xxx and out_xxx. Also, gather uniform buffers accessed.
    if (!shader_param_struct_info_ || !shader_param_data_) {
        return;
    }

    // Clear previous resources
    in_textures_.clear();
    in_buffers_.clear();
    out_textures_.clear();
    out_buffers_.clear();
    referenced_uniform_buffers_.clear();

    used_textures_.clear();
    used_buffers_.clear();

    // Iterate through all shader parameters using reflection
    for (int i = 0; i < (int)shader_param_struct_info_->cpp_members.size(); i++) {
        const auto& field = shader_param_struct_info_->cpp_members[i];
        const void* field_data = static_cast<const char*>(shader_param_data_) + field.cpp_offset;

        // Check resource type
        if (field.type == RHIParamType::kSRVTexture) { // SRV
            RDGTexture* texture = *static_cast<RDGTexture* const*>(field_data);
            if (!texture) {
                // TODO should we log a warning here?
                continue;
            }
            in_textures_.push_back(texture);
            used_textures_.emplace_back(
                RDGTextureUsage::kShaderRead,
                texture
            );
        }
        else if (field.type == RHIParamType::kUAVTexture) { // UAV
            RDGTexture* texture = *static_cast<RDGTexture* const*>(field_data);
            if (!texture) {
                // TODO should we log a warning here?
                continue;
            }
            out_textures_.push_back(texture);
            used_textures_.emplace_back(
                RDGTextureUsage::kShaderReadWrite,
                texture
            );
        } else if (field.type == RHIParamType::kStorageBuffer) { // Storage buffer
            RDGBuffer* buffer = *static_cast<RDGBuffer* const*>(field_data);
            if (!buffer) continue;
            auto usage = RDGBufferUsage{{}, buffer};
            // Add to appropriate collection based on access flags
            if (field.access_flags & RHIGPUAccessFlagBits::kWrite) {
                out_buffers_.push_back(buffer);
                usage.usage = RDGBufferUsage::kReadWriteStorage;
            } else {
                usage.usage = RDGBufferUsage::kReadOnlyStorge;
            }
            if (field.access_flags & RHIGPUAccessFlagBits::kRead) {
                in_buffers_.push_back(buffer);
            }
            used_buffers_.emplace_back(usage);
        }
        else if (field.cpp_imported_struct_info.cpp_struct_info) { // Imported uniform buffer
            if (field.cpp_imported_struct_info.cpp_import_type == RDGShaderParamStructImportType::kReference) {
                auto ub = *static_cast<RDGBuffer*const *>(field_data);
                referenced_uniform_buffers_.push_back(ub);
                used_buffers_.emplace_back(RDGBufferUsage::kUniformBuffer, ub);
            }
        } else if (field.type == RHIParamType::kVertexBuffer
            || field.type == RHIParamType::kIndexBuffer
            || field.type == RHIParamType::kDispatchCommand) { // Vertex / index/ dispatch command
            RDGBuffer * buffer = *static_cast<RDGBuffer* const*>(field_data);
            if (!buffer) continue;
            auto usage = RDGBufferUsage{{}, buffer};
            if (field.type == RHIParamType::kVertexBuffer) {
                usage.usage = RDGBufferUsage::kVertexBuffer;
            } else if (field.type == RHIParamType::kIndexBuffer) {
                usage.usage = RDGBufferUsage::kIndexBuffer;
            } else if (field.type == RHIParamType::kDispatchCommand) {
                usage.usage = RDGBufferUsage::kIndirectBuffer;
            }
            in_buffers_.push_back(buffer);
            used_buffers_.emplace_back(usage);
        } else if (field.type == RHIParamType::kRenderTarget) {
            // Render target
            RDGTexture * texture = *static_cast<RDGTexture* const*>(field_data);
            if (!texture) continue;
            auto usage = RDGTextureUsage::kOutputAttachment;
            if (IsDepthStencilPixelFormat(field.cpp_extra.render_targets_info->format)) {
                usage = RDGTextureUsage::kDepthStencilAttachment;
            }
            in_textures_.push_back(texture);
            out_textures_.push_back(texture);
            used_textures_.emplace_back(
                usage,
                texture
            );
        } else {
            assert(false && "Unsupported resource type");
        }
    }

    // Lastly, create and store the uniform buffer usage
    uniform_buffer_ = new RDGBuffer(RHIBufferUsageFlagBits::kUniform, shader_param_struct_info_->size);
    in_buffers_.emplace_back(uniform_buffer_.Raw());
    used_buffers_.emplace_back(RDGBufferUsage::kUniformBuffer, uniform_buffer_);
}

MI_NAMESPACE_END