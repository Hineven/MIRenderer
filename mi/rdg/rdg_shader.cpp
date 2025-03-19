/*
 * Created: 2025/3/8
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include "rdg/rdg_shader.h"
#include "rhi/rhi.h"
#include "rhi/rhi_pipeline.h"
#include "core/infra.h"

MI_NAMESPACE_BEGIN

bool RDGShaderParamStructInfo::CanBeImported () const {
    for (auto & member : cpp_members) {
        if (member.type != RHIParamType::kStruct && member.type != RHIParamType::kBasic) {
            return false;
        }
        // If the struct contain references to other structs, it can't be imported
        if (member.type == RHIParamType::kStruct
        && member.cpp_imported_struct_info.cpp_import_type == RDGShaderParamStructImportType::kReference) {
            return false;
        }
    }
    return true;
}

RDGShaderRegistrator::RDGShaderRegistrator(
        size_t type_hash,
        std::function<RDGShaderInitializationInfo()> get_init_info
) {
    RDGShader * shader = new RDGShader(get_init_info());
    RDGShaderLibrary::GetInstance().RegisterShader(typeid(*shader).hash_code(), shader);
}

RDGShader::RDGShader(RDGShaderInitializationInfo ini)
        : name_(ini.name),
          source_location_(std::move(ini.source_location)),
          type_(ini.type) {
    shader_entries_.compute = ini.compute_entry_;
    shader_entries_.vertex = ini.vertex_entry_;
    shader_entries_.fragment = ini.fragment_entry_;
    child_methods_.GetShaderParamInfo = ini.GetShaderParamInfo;
    child_methods_.GetShaderPipelineConfig = ini.GetShaderPipelineConfig;
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
    int & remaining_mismatch_count
) {
    // Check if both structs have the same number of members
    if (cpp_declared_struct->cpp_members.size() != shader_reflected_struct->members.size()) {
        MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' - Struct '{}' has {} members in C++ but {} members in shader",
                   shader_name, struct_path_prefix,
               cpp_declared_struct->cpp_members.size(),
               shader_reflected_struct->members.size());
        if (--remaining_mismatch_count == 0) return;
    }


    // Check each member from C++ against shader reflection
    for (const auto& cpp_member : cpp_declared_struct->cpp_members) {
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
            );
            // Stop checking if we've reached the limit
            if (remaining_mismatch_count <= 0) return ;
        }
    }
}

bool RDGShader::CheckShaderReflection(TRef<RHIShader> shader, const RDGShaderParamStructInfo &info) const {
    // Firstly, export uniform buffers from cpp shader param struct reflection
    TOneTimeLinearAllocator<> aloc;
    std::vector<const RDGShaderParamStructInfo*> cpp_ref_uniform_buffers;
    std::vector<std::string> cpp_ref_uniform_buffer_names;
    for (const auto& member : info.cpp_members) {
        if (member.type == RHIParamType::kStruct
        && member.cpp_imported_struct_info.cpp_import_type == RDGShaderParamStructImportType::kReference) {
            cpp_ref_uniform_buffers.push_back(member.cpp_imported_struct_info.cpp_struct_info);
            cpp_ref_uniform_buffer_names.push_back(member.name);
        }
    }
    // Quick compare with hash values for the referenced structs
    bool passed_checking = true;
    int cpp_failure_index = -1, shader_failure_index = -1;
    auto & shader_reflected_uniform_buffers = shader->GetUniformBufferDesc();
    bool has_bindless = shader->HasBindlessResources();
    if (shader_reflected_uniform_buffers.size() - has_bindless != cpp_ref_uniform_buffers.size()) {
        MI_LOG(MIInfraLogType::kWarning,
               "Shader '{}' - Uniform buffer count mismatch between C++ and shader",
               source_location_);
        passed_checking = false;
    }
    if (shader_reflected_uniform_buffers.size() - has_bindless > cpp_ref_uniform_buffers.size()) {
        // Shader is requesting for more uniform buffers than C++ has defined, find and log the missing ones
        for (const auto& shader_ub : shader_reflected_uniform_buffers) {
            bool found = false;
            for (const auto& cpp_ub_name : cpp_ref_uniform_buffer_names) {
                if (shader_ub.name == cpp_ub_name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' - Uniform buffer '{}' is defined in shader but not found in C++",
                       source_location_, shader_ub.name);
                passed_checking = false;
            }
        }
    }
    for (auto const & [i, e] : cpp_ref_uniform_buffers | std::views::enumerate) {
        // find corresponding uniform buffer in shader reflection
        int shader_ub_idx = -1;
        for (int j = 0; j < (int)shader_reflected_uniform_buffers.size(); ++j) {
            if (shader_reflected_uniform_buffers[j].name == cpp_ref_uniform_buffer_names[i]) {
                shader_ub_idx = j;
                break;
            }
        }
        if (shader_ub_idx == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' - Uniform buffer '{}' is defined in C++ but not found in shader",
                   source_location_, cpp_ref_uniform_buffer_names[i]);
            passed_checking = false;
            continue;
        }
        // Check if the hash values match
        if (shader_reflected_uniform_buffers[shader_ub_idx].struct_reflection->layout_hash != e->layout_hash) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' - Uniform buffer '{}' has layout hash mismatch between C++ and shader",
                   source_location_, cpp_ref_uniform_buffer_names[i]);
            passed_checking = false;
            cpp_failure_index = i;
            shader_failure_index = shader_ub_idx;
        }
    }
    if (!passed_checking) {
        if (cpp_failure_index != -1) {
            int remaining_mismatch_count = 5; // End comparison after first 5 mis-matches
            // Recursively check the layout mismatch for logging the problem.
            RecursiveCheckConstantBufferDefinitions(
                shader->GetSourceFilePath() + ":" + shader->GetEntryName(),
                "",
                cpp_ref_uniform_buffers[cpp_failure_index],
                shader_reflected_uniform_buffers[shader_failure_index].struct_reflection,
                remaining_mismatch_count
            );
        }
    }

    // Check shader resources other than uniform buffers. They are easier to check.

    // Check storage buffers
    for (const auto& sb : shader->GetStorageBufferDesc()) {
        int index = info.GetMemberIndex(sb.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' uses storage buffer '{}' which is not defined in shader parameters",
                   source_location_, sb.name);
            passed_checking = false;
        } else {
            auto & member = info.cpp_members[index];
            if (member.type != RHIParamType::kStorageBuffer) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' defines '{}' as storage buffer but parameter has incompatible type",
                       source_location_, sb.name);
                passed_checking = false;
            }
            if (member.access_flags != sb.access_flags) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' defines '{}' as storage buffer but parameter has incompatible access flags."
                       "Shader flags: {}, Parameter flags: {}",
                       source_location_, sb.name, sb.access_flags, member.access_flags);
                passed_checking = false;
            }
        }
    }

    // Check UAV textures
    for (const auto& uav : shader->GetUAVDesc()) {
        int index = info.GetMemberIndex(uav.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' uses UAV texture '{}' which is not defined in shader parameters",
                   source_location_, uav.name);
            passed_checking = false;
        } else {
            auto & member = info.cpp_members[index];
            if (member.type != RHIParamType::kUAVTexture) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' defines '{}' as UAV texture but parameter has incompatible type."
                       "Parameter type: {}",
                       source_location_, uav.name, member.type);
                passed_checking = false;
            }
        }
    }
    // Check SRV textures
    for (const auto& srv : shader->GetSRVDesc()) {
        int index = info.GetMemberIndex(srv.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' uses SRV texture '{}' which is not defined in shader parameters",
                   source_location_, srv.name);
            passed_checking = false;
        } else {
            auto & member = info.cpp_members[index];
            if (member.type != RHIParamType::kSRVTexture) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' defines '{}' as SRV texture but parameter has incompatible type."
                       "Parameter type: {}",
                       source_location_, srv.name, member.type);
                passed_checking = false;
            }
        }
    }
    // Check samplers
    for (const auto& sampler : shader->GetSamplerDesc()) {
        int index = info.GetMemberIndex(sampler.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' uses sampler '{}' which is not defined in shader parameters",
                   source_location_, sampler.name);
            passed_checking = false;
        } else {
            auto & member = info.cpp_members[index];
            if (member.type != RHIParamType::kSampler) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' defines '{}' as sampler but parameter has incompatible type."
                       "Parameter type: {}",
                       source_location_, sampler.name, member.type);
                passed_checking = false;
            }
        }
    }
    // Check acceleration structures
    for (const auto& as : shader->GetAccelerationStructureDesc()) {
        int index = info.GetMemberIndex(as.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' uses acceleration structure '{}' which is not defined in shader parameters",
                   source_location_, as.name);
            passed_checking = false;
        } else {
            auto & member = info.cpp_members[index];
            if (member.type != RHIParamType::kAccelerationStructure) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' defines '{}' as acceleration structure but parameter has incompatible type."
                       "Parameter type: {}",
                       source_location_, as.name, member.type);
                passed_checking = false;
            }
        }
    }

    // Check vertex attributes
    for (const auto& vb : shader->GetVertexInputDesc()) {
        int index = info.GetMemberIndex(vb.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                "Shader '{}' uses vertex attribute '{}' (location {}) which is not defined in shader parameters",
                source_location_, vb.name, vb.location);
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
                        source_location_, vb.name, member.type);
                passed_checking = false;
            } else {
                if (member.cpp_extra.vertex_attribute_info->attribute_index != vb.location) {
                    MI_LOG(MIInfraLogType::kWarning,
                        "Shader '{}' defines '{}' as vertex attribute but parameter has incompatible location."
                        "Parameter location: {}, Shader location: {}",
                        source_location_, vb.name, member.cpp_extra.vertex_attribute_info->attribute_index, vb.location);
                    passed_checking = false;
                }
                if (member.cpp_extra.vertex_attribute_info->format != vb.format) {
                    MI_LOG(MIInfraLogType::kWarning,
                            "Shader '{}' defines '{}' as vertex attribute but parameter has incompatible format."
                            "Parameter format: {}, Shader format: {}",
                            source_location_, vb.name, member.cpp_extra.vertex_attribute_info->format, vb.format);
                    passed_checking = false;
                }
            }
        }
    }

    // Check fragment outputs (RenderTarget vs fragment output reflected from SPIR-V, check format compatibility)
    for (const auto & output : shader->GetFragmentOutputDesc()) {
        int index = info.GetMemberIndex(output.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                "Shader '{}' uses fragment output '{}' (location {}) which is not defined in shader parameters",
                source_location_, output.name, output.location);
            passed_checking = false;
            // Find the fragment output with corresponding location
            for (auto & member : info.cpp_members) {
                if (member.type == RHIParamType::kRenderTarget
                && member.cpp_extra.render_targets_info->target_index == output.location) {
                    MI_LOG(MIInfraLogType::kWarning,
                        "The declared fragment output with corresponding location is {}",
                        member.name);
                    break;
                }
            }
        } else {
            auto & member = info.cpp_members[index];
            if (member.type != RHIParamType::kRenderTarget) {
                MI_LOG(MIInfraLogType::kWarning,
                    "Shader '{}' defines '{}' as fragment output but parameter has incompatible type."
                    "Parameter type: {}",
                    source_location_, output.name, member.type);
                passed_checking = false;
            } else {
                if (member.cpp_extra.render_targets_info->target_index != output.location) {
                    MI_LOG(MIInfraLogType::kWarning,
                        "Shader '{}' defines '{}' as fragment output but parameter has incompatible location."
                        "Parameter location: {}, Shader location: {}",
                        source_location_, output.name, member.cpp_extra.render_targets_info->target_index, output.location);
                    passed_checking = false;
                }
                if (!RHIIsOutputCompatiablePixelFormat(output.format, member.cpp_extra.render_targets_info->format)) {
                    MI_LOG(MIInfraLogType::kWarning,
                        "Shader '{}' defines '{}' as fragment output but parameter has incompatible format."
                        "Parameter format: {}, Shader format: {}",
                        source_location_, output.name, member.cpp_extra.render_targets_info->format, output.format);
                    passed_checking = false;
                }
            }
        }
    }

    // TODO check immutable samplers, ...
    return passed_checking;
}


std::string RDGShader::LoadSource () const {
    // Firstly, load the source code from the source location
    auto source_code = LoadFile(source_location_);
    return source_code;
}

bool RDGShader::RecompileShaders(const std::string & source_code) {
    // Re-compile the shader
    shaders_ = {};
    if (source_code.empty()) {
        MI_LOG(MIInfraLogType::kError, "Empty shader source: {}", source_location_);
        return false;
    }
    // Reflect shader struct param info
    const RDGShaderParamStructInfo & param_info = *child_methods_.GetShaderParamInfo();

    // Compile the shader and create RHI shaders
    std::string errmsg;
    std::wstring source_location_wstr(source_location_.begin(), source_location_.end());
    if(type_ == RHIPipelineType::kCompute) {
        auto result = GetInfra().CompileHLSLToSPIRV(
                source_location_wstr.c_str(), std::string(GetShaderEntries().compute), "cs_6_6",
                std::span(source_code.data(), source_code.size()), {}, errmsg
        );
        if (result.empty()) {
            MI_LOG(MIInfraLogType::kError, "Failed to compile shader: {}", errmsg);
            return false;
        }
        // Create the compute shader
        auto bytecode_span = std::span(reinterpret_cast<const std::byte *>(result.data()), result.size() * sizeof(uint32_t));
        auto shader = RHI::Get().CreateShader(
                RHIShaderFrequencyFlagBits::kCompute, GetShaderEntries().compute,
                RHIShaderIRType::kSPIRV, bytecode_span
        );
        if (!shader) {
            MI_LOG(MIInfraLogType::kError, "Failed to create shader");
            return false;
        }
        shaders_.compute = shader;
        CheckShaderReflection(shader, param_info);
    }
    if(type_ == RHIPipelineType::kGraphics) {
        // For graphics pipeline, we need to compile vertex and fragment shaders
        // First compile vertex shader
        auto vs_result = GetInfra().CompileHLSLToSPIRV(
                source_location_wstr.c_str(), std::string(GetShaderEntries().vertex), "vs_6_6",
                std::span(source_code.data(), source_code.size()), {}, errmsg
        );
        if (vs_result.empty()) {
            MI_LOG(MIInfraLogType::kError, "Failed to compile vertex shader: {}", errmsg);
            return false;
        }

        // Then compile fragment shader
        auto fs_result = GetInfra().CompileHLSLToSPIRV(
                source_location_wstr.c_str(), std::string(GetShaderEntries().fragment), "ps_6_6",
                std::span(source_code.data(), source_code.size()), {}, errmsg
        );
        if (fs_result.empty()) {
            MI_LOG(MIInfraLogType::kError, "Failed to compile fragment shader: {}", errmsg);
            return false;
        }

        // Create the vertex shader
        auto vs_bytecode_span = std::span(reinterpret_cast<const std::byte*>(vs_result.data()), vs_result.size() * sizeof(uint32_t));
        auto vertex_shader = RHI::Get().CreateShader(
                RHIShaderFrequencyFlagBits::kVertex, "VSMain",
                RHIShaderIRType::kSPIRV, vs_bytecode_span
        );
        if (!vertex_shader) {
            MI_LOG(MIInfraLogType::kError, "Failed to create vertex shader");
            return false;
        }
        CheckShaderReflection(vertex_shader, param_info);
        shaders_.vertex = vertex_shader;

        // Create the fragment shader
        auto fs_bytecode_span = std::span(reinterpret_cast<const std::byte*>(fs_result.data()), fs_result.size() * sizeof(uint32_t));
        auto fragment_shader = RHI::Get().CreateShader(
                RHIShaderFrequencyFlagBits::kFragment, "PSMain",
                RHIShaderIRType::kSPIRV, fs_bytecode_span
        );
        if (!fragment_shader) {
            MI_LOG(MIInfraLogType::kError, "Failed to create fragment shader");
            return false;
        }
        CheckShaderReflection(fragment_shader, param_info);
        shaders_.fragment = fragment_shader;
    }
    return true;
}



bool RDGShader::Recompile(RDGShaderParamStructAndSizeInfo * info) {

    // Clear legacy resources
    graphics_pipeline_ = {};
    shaders_ = {};

    auto source_code = LoadSource();
    if (source_code.empty()) {
        MI_LOG(MIInfraLogType::kError, "Failed to load shader source: {}", source_location_);
        return false;
    }
    if (!RecompileShaders(source_code)) return false;

    auto pipeline_config = child_methods_.GetShaderPipelineConfig();

    // Assemble the pipeline
    if (type_ == RHIPipelineType::kCompute) {
        auto pipeline = RHI::Get().CreateComputePipeline(
            shaders_.compute.Raw(), GetName().c_str()
        );
        if (!pipeline) {
            MI_LOG(MIInfraLogType::kError, "Failed to create compute pipeline");
            return false;
        }
        compute_pipeline_ = pipeline;
    }
    if (type_ == RHIPipelineType::kGraphics) {
        RHIGraphicsPipelineDesc desc {};
        desc.stages.vertex_shader = shaders_.vertex.Raw();
        desc.stages.fragment_shader = shaders_.fragment.Raw();
        auto vertex_inputs = shaders_.vertex->GetVertexInputDesc();
        std::vector<RHIVertexInputBindingDesc> rhi_bindings;
        std::vector<RHIVertexInputAttributeDesc> rhi_attributes;
        // Gather vertex input configurations from shader param struct info
        for (auto & e : info->cpp_members) {
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
        // TODO support more primitive topologies
        desc.topology = RHIPrimitiveTopologyType::kTriangleList;


        auto fragment_outputs = shaders_.fragment->GetFragmentOutputDesc();
        std::vector<RHIColorAttachmentDesc> color_attachments;
        // Gather color attachment configurations from shader param struct info
        for (auto & e : info->cpp_members) {
            if (e.type == RHIParamType::kRenderTarget) {
                // TODO support more blending operations
                RHIColorAttachmentBlendDesc blend {};
                color_attachments.push_back({blend, e.cpp_extra.render_targets_info->format});
            }
        }
        // TODO support depth stencil
        desc.color_attachments = color_attachments;
        auto pipeline = RHI::Get().CreateGraphicsPipeline(
                desc, GetName().c_str()
        );
        if (!pipeline) {
            MI_LOG(MIInfraLogType::kError, "Failed to create graphics pipeline");
            return false;
        }
        graphics_pipeline_ = pipeline;
    }
}

MI_NAMESPACE_END