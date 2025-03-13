/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_pass.h"

#include <rdg/rdg_param.h>

MI_NAMESPACE_BEGIN
void RDGPass::GatherInOutResources() {
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

    // Iterate through all shader parameters using reflection
    for (int i = 0; i < (int)shader_param_struct_info_->cpp_members.size(); i++) {
        const auto& field = shader_param_struct_info_->cpp_members[i];
        const void* field_data = static_cast<const char*>(shader_param_data_) + field.cpp_offset;

        // Check resource type
        if (field.type == RHIParamType::kSRVTexture) {
            RDGTexture* texture = *static_cast<RDGTexture* const*>(field_data);
            if (!texture) {
                // TODO should we log a warning here?
                continue;
            }
            in_textures_.push_back(texture);
        }
        else if (field.type == RHIParamType::kUAVTexture) {
            RDGTexture* texture = *static_cast<RDGTexture* const*>(field_data);
            if (!texture) {
                // TODO should we log a warning here?
                continue;
            }
            out_textures_.push_back(texture);
        } else if (field.type == RHIParamType::kStorageBuffer) {
            RDGBuffer* buffer = *static_cast<RDGBuffer* const*>(field_data);
            if (!buffer) continue;

            // Add to appropriate collection based on access flags
            if (field.access_flags & RHIGPUAccessFlagBits::kWrite) {
                out_buffers_.push_back(buffer);
            }
            if (field.access_flags & RHIGPUAccessFlagBits::kRead) {
                in_buffers_.push_back(buffer);
            }
        }
        else if (field.cpp_imported_struct_info.cpp_struct_info) {
            if (field.cpp_imported_struct_info.cpp_import_type == RDGShaderParamStructImportType::kReference) {
                auto ub = *static_cast<RDGBuffer*const *>(field_data);
                referenced_uniform_buffers_.push_back(ub);
            }
        } else {
            assert(false && "Unsupported");
        }
    }
}

MI_NAMESPACE_END