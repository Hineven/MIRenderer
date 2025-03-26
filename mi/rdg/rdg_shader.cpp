/*
 * Created: 2025/3/8
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include <xxhash.h>
#include "rdg/rdg_shader.h"
#include "rhi/rhi.h"
#include "rhi/rhi_pipeline.h"
#include "core/infra.h"

MI_NAMESPACE_BEGIN

size_t RDGShaderInitializationInfo::GetHash() const {
    size_t final_hash = 0;
    for (const auto & macro : macros) {
        final_hash ^= XXH64(macro.c_str(), macro.size(), 12312321);
    }
    return final_hash;
}


bool RDGShaderParamStructInfo::CanBeImported () const {
    for (auto & member : cpp_members) {
        if (member.type != RHIParamType::kStruct && member.type != RHIParamType::kBasic) {
            return false;
        }
    }
    return true;
}

bool RDGShaderParamStructInfo::CanBeRenderpass () const {
    for (auto & member : cpp_members) {
        if (member.type != RHIParamType::kRenderTarget) return false;
    }
    return true;
}

RDGShader::RDGShader (const RDGShaderClassRegistry * class_registry) : class_registry_(class_registry) {

}

RDGShader::~RDGShader () {

}

static std::string LoadFile(const std::string & path) {
    auto reader = GetInfra().RIO_Open(path, MIInfraResourceHintType::kShaderSource);
    if (!reader) {
        MI_LOG(MIInfraLogType::kError, "Failed to open resource: {}", path);
        return {};
    }
    auto size = reader->GetSize();
    std::string content(size, '\0');
    reader->ReadBlob(0, size, content.data());
    return content;
}

static void RecursiveCheckConstantBufferDefinitions (
    std::string shader_name,
    std::string struct_path_prefix,
    const RDGShaderParamStructInfo * cpp_declared_struct,
    const RHIParamStructInfo * shader_reflected_struct,
    int & remaining_mismatch_count,
    bool strict_mode = true
    // Disabled for global constant buffer, the cpp side reflection may contain more members
    // such as textures that does not exist in shader reflection.
) {
    // Check if both structs have the same number of members
    if (strict_mode && cpp_declared_struct->cpp_members.size() != shader_reflected_struct->members.size()) {
        MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' - Struct '{}' has {} members in C++ but {} members in shader",
                   shader_name, struct_path_prefix,
               cpp_declared_struct->cpp_members.size(),
               shader_reflected_struct->members.size());
        if (--remaining_mismatch_count == 0) return;
    }

    std::set<int> visited_shader_member_indices;
    int ub_member_index = 0;
    // Check each member from C++ against shader reflection
    for (const auto& cpp_member : cpp_declared_struct->cpp_members) {
        // Only compare uniforms
        if (strict_mode || (cpp_member.type == RHIParamType::kStruct || cpp_member.type == RHIParamType::kBasic)) {
            int shader_member_idx = -1;
            for (size_t i = 0; i < shader_reflected_struct->members.size(); ++i) {
                if (shader_reflected_struct->members[i].name == cpp_member.name) {
                    shader_member_idx = static_cast<int>(i);
                    break;
                }
            }

            if (shader_member_idx == -1) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' - Member '{}::{}' is defined in C++ but not found in shader",
                       shader_name, struct_path_prefix, cpp_member.name);
                if (--remaining_mismatch_count == 0) return;
            }
            if (shader_member_idx != -1 && shader_member_idx != ub_member_index) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' - Member '{}::{}' have different order in C++ and shader."
                       " Shader: {}, C++: {}",
                       shader_name, struct_path_prefix, cpp_member.name, shader_member_idx, ub_member_index);
                if (--remaining_mismatch_count == 0) return;
            }
            visited_shader_member_indices.insert(shader_member_idx);

            const auto& shader_member = shader_reflected_struct->members[shader_member_idx];

            // Check if types match
            if (cpp_member.type != shader_member.type) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' - Member '{}::{}' has type mismatch between C++ and shader",
                       shader_name, struct_path_prefix, cpp_member.name);
                if (--remaining_mismatch_count == 0) return;
            }

            // If it's a nested struct, recursively check them
            if (cpp_member.type == RHIParamType::kStruct && shader_member.type == RHIParamType::kStruct) {
                std::string nested_path = struct_path_prefix + "::" + cpp_member.name;
                RecursiveCheckConstantBufferDefinitions(
                    shader_name,
                    nested_path,
                    cpp_member.cpp_imported_struct_info.cpp_struct_info,
                    shader_member.struct_info,
                    remaining_mismatch_count
                    // Here, always use strict mode for nested structs
                );
                // Stop checking if we've reached the limit
                if (remaining_mismatch_count <= 0) return ;
            }
            ub_member_index ++;
        }
    }
    if (visited_shader_member_indices.size() != shader_reflected_struct->members.size()) {
        // Find and log the missing members
        for (auto [i, e] : std::views::enumerate(shader_reflected_struct->members)) {
            if (!visited_shader_member_indices.contains((int)i)) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' - Member '{}::{}' missing from C++ uniform buffer declaration.",
                       shader_name, struct_path_prefix, e.name);
                if (--remaining_mismatch_count == 0) return;
            }
        }
    }
}

bool RDGShader::CheckShaderReflection(RHIShader * shader, const RDGShaderParamStructAndSizeInfo &info) {
    // Firstly, export uniform buffers from cpp shader param struct reflection
    TOneTimeLinearAllocator<> aloc;

    // Quick compare with hash values for the referenced structs
    bool passed_checking = true;
    int cpp_failure_index = -1, shader_failure_index = -1;
    auto & shader_reflected_uniform_buffers = shader->GetUniformBufferDesc();
    bool has_bindless = shader->HasBindlessResources();
    bool cpp_has_globals = info.global_uniforms_.size() > 0;
    // if (shader_reflected_uniform_buffers.size() - has_bindless != info.uniform_buffers_.size() + cpp_has_globals) {
    //     MI_LOG(MIInfraLogType::kWarning,
    //            "Shader '{}' - Uniform buffer count mismatch between C++ and shader",
    //            class_registry_->source_location);
    //     passed_checking = false;
    // }
    if ((int)shader_reflected_uniform_buffers.size() - has_bindless > info.uniform_buffers_.size() + cpp_has_globals) {
        // Shader is requesting for more uniform buffers than C++ has defined, find and log the missing ones
        for (const auto& shader_ub : shader_reflected_uniform_buffers) {
            bool found = false;
            for (const auto& cpp_ub : info.uniform_buffers_) {
                if (shader_ub.name == cpp_ub.info->name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' - Uniform buffer '{}' is defined in shader but not found in C++",
                       class_registry_->source_location, shader_ub.name);
                passed_checking = false;
            }
        }
    }
    // Check and remap referenced uniform buffers
    cpp_resource_index_to_slot_[(uint32_t)RHIParamType::kUniformBuffer].resize(info.uniform_buffers_.size() + cpp_has_globals, UINT32_MAX);
    for (auto const & [i, e] : info.uniform_buffers_ | std::views::enumerate) {
        // find corresponding uniform buffer in shader reflection
        int shader_ub_idx = -1;
        for (int j = 0; j < (int)shader_reflected_uniform_buffers.size(); ++j) {
            if (shader_reflected_uniform_buffers[j].name == e.info->name) {
                shader_ub_idx = j;
                break;
            }
        }
        if (shader_ub_idx == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' - Uniform buffer '{}' is defined in C++ but not found in shader",
                   class_registry_->source_location, e.info->name);
            passed_checking = false;
        } else {
            // Check if the hash values match
            if (shader_reflected_uniform_buffers[shader_ub_idx].struct_reflection->uniforms_layout_hash
                != e.info->cpp_imported_struct_info.cpp_struct_info->uniforms_layout_hash) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' - Uniform buffer '{}' has layout hash mismatch between C++ and shader",
                       class_registry_->source_location, e.info->name);
                passed_checking = false;
                cpp_failure_index = (int)i;
                shader_failure_index = shader_ub_idx;
            }
        }
    }
    // Check the 'type.$Globals' uniform buffer
    {
        // find corresponding uniform buffer in shader reflection
        int shader_ub_idx = -1;
        for (int j = 0; j < (int)shader_reflected_uniform_buffers.size(); ++j) {
            if (shader_reflected_uniform_buffers[j].name == "type.$Globals") {
                shader_ub_idx = j;
                break;
            }
        }
        if (shader_ub_idx != -1) {
            // Check if the hash values match
            if (shader_reflected_uniform_buffers[shader_ub_idx].struct_reflection->uniforms_layout_hash
                != info.uniforms_layout_hash) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' - Global uniform buffer has layout hash mismatch between C++ and shader",
                       class_registry_->source_location);
                passed_checking = false;
                cpp_failure_index = (int)-2; // -2 means global uniform buffer
                shader_failure_index = shader_ub_idx;
            }
        }
    }
    if (!passed_checking) {
        if (cpp_failure_index > 0) {
            int remaining_mismatch_count = 5; // End comparison after first 5 mis-matches
            // Recursively check the layout mismatch for logging the problem.
            RecursiveCheckConstantBufferDefinitions(
                shader->GetSourceFilePath() + ":" + shader->GetEntryName(),
                info.uniform_buffers_[cpp_failure_index].info->name,
                info.uniform_buffers_[cpp_failure_index].info->cpp_imported_struct_info.cpp_struct_info,
                shader_reflected_uniform_buffers[shader_failure_index].struct_reflection,
                remaining_mismatch_count
            );
        } else if (cpp_failure_index == -2) {
            // Check for global uniform buffer
            int remaining_mismatch_count = 5; // End comparison after first 5 mis-matches
            // Recursively check the layout mismatch for logging the problem.
            RecursiveCheckConstantBufferDefinitions(
                shader->GetSourceFilePath() + ":" + shader->GetEntryName(),
                "<Globals>",
                &info,
                shader_reflected_uniform_buffers[shader_failure_index].struct_reflection,
                remaining_mismatch_count,
                false
            );
        }
    }

    // Check shader resources other than uniform buffers. They are easier to check.

    auto FindIndex = [&](std::span<RDGShaderParameterLocation> resources, const std::string & name) -> int {
        for (int i = 0; i < (int)resources.size(); ++i) {
            if (resources[i].info->name == name) {
                return i;
            }
        }
        return -1;
    };

    // Check storage buffers
    cpp_resource_index_to_slot_[(uint32_t)RHIParamType::kStorageBuffer].resize(info.storage_buffers_.size(), UINT32_MAX);
    for (const auto& sb : shader->GetStorageBufferDesc()) {
        int index = FindIndex(info.storage_buffers_, sb.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' uses storage buffer '{}' which is not defined in shader parameters",
                   class_registry_->source_location, sb.name);
            passed_checking = false;
        } else {
            auto & member = *info.storage_buffers_[index].info;
            if (member.access_flags != sb.access_flags) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' defines '{}' as storage buffer but parameter has incompatible access flags."
                       "Shader flags: {}, Parameter flags: {}",
                       class_registry_->source_location, sb.name, ToString(sb.access_flags), ToString(member.access_flags));
                passed_checking = false;
            }
        }
    }

    // Check UAV textures
    for (const auto& uav : shader->GetUAVDesc()) {
        int index = FindIndex(info.uavs_, uav.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' uses UAV texture '{}' which is not defined in shader parameters",
                   class_registry_->source_location, uav.name);
            passed_checking = false;
        } else {
            auto & member = *info.uavs_[index].info;
            if (member.type != RHIParamType::kUAVTexture) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' defines '{}' as UAV texture but parameter has incompatible type."
                       "Parameter type: {}",
                       class_registry_->source_location, uav.name, ToString(member.type));
                passed_checking = false;
            }
        }
    }
    // Check SRV textures
    for (const auto& srv : shader->GetSRVDesc()) {
        int index = FindIndex(info.uavs_, srv.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' uses SRV texture '{}' which is not defined in shader parameters",
                   class_registry_->source_location, srv.name);
            passed_checking = false;
        } else {
            auto & member = *info.srvs_[index].info;
            if (member.type != RHIParamType::kSRVTexture) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' defines '{}' as SRV texture but parameter has incompatible type."
                       "Parameter type: {}",
                       class_registry_->source_location, srv.name, ToString(member.type));
                passed_checking = false;
            }
        }
    }

    // Check samplers
    for (const auto& sampler : shader->GetSamplerDesc()) {
        int index = FindIndex(info.uavs_, sampler.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' uses sampler '{}' which is not defined in shader parameters",
                   class_registry_->source_location, sampler.name);
            passed_checking = false;
        } else {
            auto & member = *info.samplers_[index].info;
            if (member.type != RHIParamType::kSampler) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' defines '{}' as sampler but parameter has incompatible type."
                       "Parameter type: {}",
                       class_registry_->source_location, sampler.name, ToString(member.type));
                passed_checking = false;
            }
        }
    }

    // Check acceleration structures
    for (const auto& as : shader->GetAccelerationStructureDesc()) {
        int index = FindIndex(info.uavs_, as.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' uses acceleration structure '{}' which is not defined in shader parameters",
                   class_registry_->source_location, as.name);
            passed_checking = false;
        } else {
            auto & member = *info.acceleration_structures_[index].info;
            if (member.type != RHIParamType::kAccelerationStructure) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' defines '{}' as acceleration structure but parameter has incompatible type."
                       "Parameter type: {}",
                       class_registry_->source_location, as.name, ToString(member.type));
                passed_checking = false;
            }
        }
    }

    // Check vertex attributes
    for (const auto& vb : shader->GetVertexInputDesc()) {
        int index = info.GetCppMemberIndex(vb.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                "Shader '{}' uses vertex attribute '{}' (location {}) which is not defined in shader parameters",
                class_registry_->source_location, vb.name, vb.location);
            passed_checking = false;
            // Find the vertex attribute with corresponding location
            for (auto & member : info.cpp_members) {
                if (member.type == RHIParamType::kVertexAttribute
                && member.cpp_extra.vertex_attribute_info->attribute_index == vb.location) {
                    MI_LOG(MIInfraLogType::kWarning,
                        "The declared vertex attribute with corresponding location is {}",
                        member.name);
                    break;
                }
            }
        } else {
            auto & member = info.cpp_members[index];
            if (member.type != RHIParamType::kVertexAttribute) {
                MI_LOG(MIInfraLogType::kWarning,
                        "Shader '{}' defines '{}' as vertex attribute but parameter has incompatible type."
                        "Parameter type: {}",
                        class_registry_->source_location, vb.name, ToString(member.type));
                passed_checking = false;
            } else {
                if (member.cpp_extra.vertex_attribute_info->attribute_index != vb.location) {
                    MI_LOG(MIInfraLogType::kWarning,
                        "Shader '{}' defines '{}' as vertex attribute but parameter has incompatible location."
                        "Parameter location: {}, Shader location: {}",
                        class_registry_->source_location, vb.name, member.cpp_extra.vertex_attribute_info->attribute_index, vb.location);
                    passed_checking = false;
                }
                if (member.cpp_extra.vertex_attribute_info->format != vb.format) {
                    MI_LOG(MIInfraLogType::kWarning,
                            "Shader '{}' defines '{}' as vertex attribute but parameter has incompatible format."
                            "Parameter format: {}, Shader format: {}",
                            class_registry_->source_location, vb.name, ToString(member.cpp_extra.vertex_attribute_info->format), ToString(vb.format));
                    passed_checking = false;
                }
            }
        }
    }

    // Check fragment outputs (RenderTarget vs fragment output reflected from SPIR-V, check format compatibility)
    // (Names can be different, but the location and format must match)
    if (shader->GetFragmentOutputDesc().size()) {
        if (!info.renderpass_.info) {
            MI_LOG(MIInfraLogType::kWarning, "Shader {} defines fragment outputs but no renderpass is defined.", class_registry_->source_location);
            passed_checking = false;
        } else {
            auto ptr = info.renderpass_.info->cpp_imported_struct_info.cpp_struct_info;
            if (ptr->render_targets_.size() < shader->GetFragmentOutputDesc().size()) {
                MI_LOG(MIInfraLogType::kWarning,
                    "Shader '{}' defines {} fragment outputs but parameter only has {}, which is insufficient.",
                    class_registry_->source_location, shader->GetFragmentOutputDesc().size(), ptr->render_targets_.size());
                passed_checking = false;
            }
        }
    }
    for (const auto & [i, output] : std::views::enumerate(shader->GetFragmentOutputDesc())) {
        int index = i;
        auto pass = info.renderpass_;
        if (pass.info) {
            auto & member = *pass.info->cpp_imported_struct_info.cpp_struct_info->render_targets_[index].info;
            if (member.type != RHIParamType::kRenderTarget) {
                MI_LOG(MIInfraLogType::kWarning,
                    "Shader '{}' defines '{}' as fragment output but parameter has incompatible type."
                    "Parameter type: {}",
                    class_registry_->source_location, output.name, ToString(member.type));
                passed_checking = false;
            } else {
                if (member.cpp_extra.render_targets_info->target_index != output.location) {
                    MI_LOG(MIInfraLogType::kWarning,
                        "Shader '{}' defines '{}' as fragment output but parameter has incompatible location."
                        "Parameter location: {}, Shader location: {}",
                        class_registry_->source_location, output.name, member.cpp_extra.render_targets_info->target_index, output.location);
                    passed_checking = false;
                }
                if (!RHIIsOutputCompatiablePixelFormat(output.format, member.cpp_extra.render_targets_info->format)) {
                    MI_LOG(MIInfraLogType::kWarning,
                        "Shader '{}' defines '{}' as fragment output but parameter has incompatible format."
                        "Parameter format: {}, Shader format: {}",
                        class_registry_->source_location, output.name, ToString(member.cpp_extra.render_targets_info->format), ToString(output.format));
                    passed_checking = false;
                }
            }
        }
    }

    // TODO check immutable samplers, ...
    return passed_checking;
}

void RDGShader::RemapResourceIndexToResourceSlots() {
    // Clear the previous bindings
    for (int i = 0; i < (int)RHIParamType::kMax; i++) {
        cpp_resource_index_to_slot_[i].clear();
    }
    assert(IsValid() && "Only with an assembled pipeline can we remap bindings");
    auto & info = *class_registry_->GetShaderParamStructInfo();
    RHIPipeline * pipeline = nullptr;
    if (class_registry_->type == RHIPipelineType::kGraphics) {
        pipeline = graphics_pipeline_.Raw();
    } else if (class_registry_->type == RHIPipelineType::kCompute) {
        pipeline = compute_pipeline_.Raw();
    }
    auto FindSlotIndex = [&] <typename T> (const std::string & name, T & list) {
        for (int i = 0; i < (int)list.size(); ++i) {
            if (list[i].name == name) {
                return i;
            }
        }
        return -1;
    };
    bool cpp_has_globals = info.global_uniforms_.size() > 0;
    // Remap uniform buffers
    {
        auto & ub = pipeline->GetUniformBufferDesc();
        cpp_resource_index_to_slot_[(uint32_t)RHIParamType::kUniformBuffer].resize(info.uniform_buffers_.size() + cpp_has_globals, UINT32_MAX);
        for (auto [i, e] : std::views::enumerate(info.uniform_buffers_)) {
            auto index = FindSlotIndex(e.info->name, ub);
            if (index != -1) {
                cpp_resource_index_to_slot_[(uint32_t)RHIParamType::kUniformBuffer][i] = index;
            }
            // Potentially there are UBs declared in cpp but not present in shaders. Simply omit that case.
        }
    }
    // Remap global uniform buffer
    if (cpp_has_globals) {
        auto & ub = pipeline->GetUniformBufferDesc();
        auto index = FindSlotIndex("type.$Globals", ub);
        if (index != -1) {
            // The global uniform buffer is always the last one
            cpp_resource_index_to_slot_[(uint32_t)RHIParamType::kUniformBuffer][info.uniform_buffers_.size()] = index;
        }
    }
    // Remap storage buffers
    {
        auto & sb = pipeline->GetStorageBufferDesc();
        cpp_resource_index_to_slot_[(uint32_t)RHIParamType::kStorageBuffer].resize(info.storage_buffers_.size(), UINT32_MAX);
        for (auto [i, e] : std::views::enumerate(info.storage_buffers_)) {
            auto index = FindSlotIndex(e.info->name, sb);
            if (index != -1) {
                cpp_resource_index_to_slot_[(uint32_t)RHIParamType::kStorageBuffer][i] = index;
            }
        }
    }
    // Remap uavs
    {
        auto & uavs = pipeline->GetUAVDesc();
        cpp_resource_index_to_slot_[(uint32_t)RHIParamType::kUAVTexture].resize(info.uavs_.size(), UINT32_MAX);
        for (auto [i, e] : std::views::enumerate(info.uavs_)) {
            auto index = FindSlotIndex(e.info->name, uavs);
            if (index != -1) {
                cpp_resource_index_to_slot_[(uint32_t)RHIParamType::kUAVTexture][i] = index;
            }
        }
    }
    // Remap srvs
    {
        auto & srvs = pipeline->GetSRVDesc();
        cpp_resource_index_to_slot_[(uint32_t)RHIParamType::kSRVTexture].resize(info.srvs_.size(), UINT32_MAX);
        for (auto [i, e] : std::views::enumerate(info.srvs_)) {
            auto index = FindSlotIndex(e.info->name, srvs);
            if (index != -1) {
                cpp_resource_index_to_slot_[(uint32_t)RHIParamType::kSRVTexture][i] = index;
            }
        }
    }
    // TODO remap samplers, as, ...
}



std::string RDGShader::LoadSource () const {
    // Firstly, load the source code from the source location
    auto source_code = LoadFile(class_registry_->source_location);
    return source_code;
}

bool RDGShader::RecompileShaders(const std::string & source_code) {
    // Re-compile the shader
    shaders_ = {};
    if (source_code.empty()) {
        MI_LOG(MIInfraLogType::kError, "Empty shader source: {}", class_registry_->source_location);
        return false;
    }
    // Reflect shader struct param info
    const RDGShaderParamStructAndSizeInfo & param_info = *class_registry_->GetShaderParamStructInfo();

    // Compile the shader and create RHI shaders
    std::string errmsg;
    std::wstring source_location_wstr(class_registry_->source_location.begin(), class_registry_->source_location.end());
    if(class_registry_->type == RHIPipelineType::kCompute) {
        auto result = GetInfra().CompileHLSLToSPIRV(
                source_location_wstr.c_str(), std::string(class_registry_->compute_entry_), "cs_6_6",
                std::span(source_code.data(), source_code.size()), {}, errmsg
        );
        if (result.empty()) {
            MI_LOG(MIInfraLogType::kError, "Failed to compile shader: {}", errmsg);
            return false;
        }
        // Create the compute shader
        auto bytecode_span = std::span(reinterpret_cast<const std::byte *>(result.data()), result.size() * sizeof(uint32_t));
        auto shader = RHI::Get().CreateShader(
                RHIShaderFrequencyFlagBits::kCompute, class_registry_->compute_entry_,
                RHIShaderIRType::kSPIRV, bytecode_span
        );
        if (!shader) {
            MI_LOG(MIInfraLogType::kError, "Failed to create shader");
            return false;
        }
        shaders_.compute = shader;
        CheckShaderReflection(shader.Raw(), param_info);
    }
    if(class_registry_->type == RHIPipelineType::kGraphics) {
        // For graphics pipeline, we need to compile vertex and fragment shaders
        // First compile vertex shader
        auto vs_result = GetInfra().CompileHLSLToSPIRV(
                source_location_wstr.c_str(), std::string(class_registry_->vertex_entry_), "vs_6_3",
                std::span(source_code.data(), source_code.size()), {}, errmsg
        );
        if (vs_result.empty()) {
            MI_LOG(MIInfraLogType::kError, "Failed to compile vertex shader: {}", errmsg);
            return false;
        }

        // Then compile fragment shader
        auto fs_result = GetInfra().CompileHLSLToSPIRV(
                source_location_wstr.c_str(), std::string(class_registry_->fragment_entry_), "ps_6_3",
                std::span(source_code.data(), source_code.size()), {}, errmsg
        );
        if (fs_result.empty()) {
            MI_LOG(MIInfraLogType::kError, "Failed to compile fragment shader: {}", errmsg);
            return false;
        }

        // Create the vertex shader
        auto vs_bytecode_span = std::span(reinterpret_cast<const std::byte*>(vs_result.data()), vs_result.size() * sizeof(uint32_t));
        auto vertex_shader = RHI::Get().CreateShader(
                RHIShaderFrequencyFlagBits::kVertex, class_registry_->vertex_entry_,
                RHIShaderIRType::kSPIRV, vs_bytecode_span
        );
        if (!vertex_shader) {
            MI_LOG(MIInfraLogType::kError, "Failed to create vertex shader");
            return false;
        }
        CheckShaderReflection(vertex_shader.Raw(), param_info);
        shaders_.vertex = vertex_shader;

        // Create the fragment shader
        auto fs_bytecode_span = std::span(reinterpret_cast<const std::byte*>(fs_result.data()), fs_result.size() * sizeof(uint32_t));
        auto fragment_shader = RHI::Get().CreateShader(
                RHIShaderFrequencyFlagBits::kFragment, class_registry_->fragment_entry_,
                RHIShaderIRType::kSPIRV, fs_bytecode_span
        );
        if (!fragment_shader) {
            MI_LOG(MIInfraLogType::kError, "Failed to create fragment shader");
            return false;
        }
        CheckShaderReflection(fragment_shader.Raw(), param_info);
        shaders_.fragment = fragment_shader;
    }
    return true;
}



bool RDGShader::Recompile(RDGShaderInitializationInfo ini) {

    // Clear legacy resources
    graphics_pipeline_ = {};
    compute_pipeline_ = {};
    shaders_ = {};
    is_valid_ = false;

    auto source_code = LoadSource();
    if (source_code.empty()) {
        MI_LOG(MIInfraLogType::kError, "Failed to load shader source: {}", class_registry_->source_location);
        return false;
    }
    if (!RecompileShaders(source_code)) return false;

    auto pipeline_config = class_registry_->GetShaderPipelineConfig();

    // Assemble the pipeline
    if (class_registry_->type == RHIPipelineType::kCompute) {
        auto pipeline = RHI::Get().CreateComputePipeline(
            shaders_.compute.Raw(), class_registry_->name.c_str()
        );
        if (!pipeline) {
            MI_LOG(MIInfraLogType::kError, "Failed to create compute pipeline");
            return false;
        }
        compute_pipeline_ = pipeline;
    }
    if (class_registry_->type == RHIPipelineType::kGraphics) {
        RHIGraphicsPipelineDesc desc {};
        desc.stages.vertex_shader = shaders_.vertex.Raw();
        desc.stages.fragment_shader = shaders_.fragment.Raw();
        auto vertex_inputs = shaders_.vertex->GetVertexInputDesc();
        std::vector<RHIVertexInputBindingDesc> rhi_bindings;
        std::vector<RHIVertexInputAttributeDesc> rhi_attributes;
        // Gather vertex input configurations from shader param struct info
        auto params = class_registry_->GetShaderParamStructInfo();
        for (auto & e : params->cpp_members) {
            if (e.type == RHIParamType::kVertexBuffer) {
                auto input = e.cpp_extra.vertex_buffer_info;
                // TODO support more input rates
                rhi_bindings.emplace_back(input->index, input->stride, RHIVertexInputRateType::kVertex);
            }
            if (e.type == RHIParamType::kVertexAttribute) {
                auto attr = e.cpp_extra.vertex_attribute_info;
                rhi_attributes.emplace_back(attr->attribute_index, attr->buffer_index, attr->format, attr->offset);
            }
        }
        desc.vertex_input.vertex_buffers = rhi_bindings;
        desc.vertex_input.vertex_attributes = rhi_attributes;
        desc.topology = pipeline_config.topology;


        auto fragment_outputs = shaders_.fragment->GetFragmentOutputDesc();
        std::vector<RHIColorAttachmentDesc> color_attachments;
        RHIDepthStencilAttachmentDesc depth_stencil {};
        // Gather color attachment configurations from shader param struct info
        if (params->renderpass_.info) {
            auto pass_ptr = params->renderpass_.info->cpp_imported_struct_info.cpp_struct_info;
            for (auto & e : pass_ptr->cpp_members) {
                if (e.type == RHIParamType::kRenderTarget) {
                    if (e.cpp_extra.render_targets_info->target_index != UINT32_MAX) {
                        // TODO support more blending operations
                        RHIColorAttachmentBlendDesc blend {};
                        color_attachments.push_back({blend, e.cpp_extra.render_targets_info->format});
                    } else {
                        depth_stencil = {
                            e.cpp_extra.render_targets_info->format
                        };
                    }
                }
            }
        }
        desc.color_attachments = color_attachments;
        desc.depth_stencil_attachment = depth_stencil;
        auto pipeline = RHI::Get().CreateGraphicsPipeline(
                desc, class_registry_->name.c_str()
        );
        if (!pipeline) {
            MI_LOG(MIInfraLogType::kError, "Failed to create graphics pipeline");
            return false;
        }
        graphics_pipeline_ = pipeline;
    }

    is_valid_ = true;

    // Remap bindings, so we can actually associate the shader parameters with RHI pipeline binding slots
    RemapResourceIndexToResourceSlots();

    return true;
}

RDGShaderLibrary &RDGShaderLibrary::GetInstance() {
    static RDGShaderLibrary * instance_ptr;
    if (instance_ptr == nullptr) {
        instance_ptr = new RDGShaderLibrary();
    }
    return *instance_ptr;
}

RDGShaderLibrary::~RDGShaderLibrary() {}

void RDGShaderLibrary::Init() {
    // lalala
    MI_LOG(MIInfraLogType::kInfo, "Initializing RDGShaderLibrary");
}


RDGShader *RDGShaderLibrary::GetShader(size_t type_hash, RDGShaderInitializationInfo ini) {
    struct {
        size_t type_hash;
        size_t ini_hash;
    } hash_data;
    hash_data.type_hash = type_hash;
    hash_data.ini_hash = ini.GetHash();
    size_t shader_hash = XXH64(&hash_data, sizeof(hash_data), 0);
    auto it = cached_shaders_.find(shader_hash);
    if (it == cached_shaders_.end()) {
        // Not cached, try to create a new shader
        auto reg = registered_shaders_.find(type_hash);
        if (reg == registered_shaders_.end()) {
            // The class does not exist.
            return nullptr;
        }
        auto new_shader = reg->second->Creator(reg->second.get(), ini);
        cached_shaders_[shader_hash].reset(new_shader);
        return new_shader;
    } else {
        return it->second.get();
    }
}

void RDGShaderLibrary::RegisterShaderClass(
    size_t type_hash,
    RDGShaderClassRegistry in_reg) {
    auto it = registered_shaders_.find(type_hash);
    if (it == registered_shaders_.end()) {
        auto reg = std::make_unique<RDGShaderClassRegistry>();
        *reg = in_reg;
        registered_shaders_[type_hash] = std::move(reg);
    } else {
        assert(false && "Double registration, this should never happen!");
    }
}

void RDGShaderLibrary::ReleaseCompiledShaders() {
    cached_shaders_.clear();
}



MI_NAMESPACE_END