/*
 * Created: 2025/3/8
 * Author:  hineven
 * See LICENSE for licensing.
 */
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
        : source_location_(std::move(ini.source_location)),
          type_(ini.type) {
    shaders_.compute_entry = ini.compute_entry_;
    shaders_.vertex_entry = ini.vertex_entry_;
    shaders_.fragment_entry = ini.fragment_entry_;
    child_methods_.GetShaderParamInfo = ini.GetShaderParamInfo;
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
    const RHIParamStructInfo * shader_reflected_struct
) {
    // Check if both structs have the same number of members
    if (cpp_declared_struct->cpp_members.size() != shader_reflected_struct->members.size()) {
        if (cpp_declared_struct->cpp_members.size() != shader_reflected_struct->members.size()) {
        MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' - Struct '{}' has {} members in C++ but {} members in shader",
                   shader_name, struct_path_prefix,
               cpp_declared_struct->cpp_members.size(),
               shader_reflected_struct->members.size());
        return;
    }
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
        }

        const auto& shader_member = shader_reflected_struct->members[shader_member_idx];

        // Check if types match
        if (cpp_member.type != shader_member.type) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' - Member '{}::{}' has type mismatch between C++ and shader",
                   shader_name, struct_path_prefix, cpp_member.name);
        }

        // If it's a nested struct, recursively check it
        if (cpp_member.type == RHIParamType::kStruct && shader_member.type == RHIParamType::kStruct) {
            std::string nested_path = struct_path_prefix + "::" + cpp_member.name;
            RecursiveCheckConstantBufferDefinitions(
                shader_name,
                nested_path,
                cpp_member.cpp_struct_info,
                shader_member.struct_info
            );
        }
    }
}

void RDGShader::CheckShaderReflection(TRef<RHIShader> shader, const RDGShaderParamStructInfo &info) {
    // Firstly, export uniform buffers from cpp shader param struct reflection
    TOneTimeLinearAllocator<> aloc;
    std::vector<const RHIParamStructInfo*> cpp_ref_uniform_buffers;
    for (const auto& member : info.cpp_members) {
        if (member.type == RHIParamType::kStruct
        && member.cpp_imported_struct_info.cpp_import_type == RDGShaderParamStructImportType::kReference) {
            cpp_ref_uniform_buffers.push_back(member.cpp_imported_struct_info.cpp_struct_info);
        }
    }
    // Quick compare with hash values for the referenced structs
    auto & shader_reflected_uniform_buffers = shader->GetUniformBufferDesc();


    // Then, gather the global uniform buffer.

    // Check uniform buffers
    for (const auto& ub : shader->GetUniformBufferDesc()) {
        int index = info.GetMemberIndex(ub.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' uses uniform buffer '{}' which is not defined in shader parameters",
                   source_location_, ub.name);
        } else {
            auto & member = info.cpp_members[index];
            if (member.type != RHIParamType::kStruct) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' defines '{}' as a struct but parameter has incompatible type",
                       source_location_, ub.name);
            } else {
                // Recursively reflect the constant buffer struct definition in cpp and shader.

            }
        }
    }

    // Check storage buffers
    for (const auto& sb : shader->GetStorageBufferDesc()) {
        int index = info.GetMemberIndex(sb.name);
        if (index == -1) {
            MI_LOG(MIInfraLogType::kWarning,
                   "Shader '{}' uses storage buffer '{}' which is not defined in shader parameters",
                   source_location_, sb.name);
        } else {
            auto & member = info.cpp_members[index];
            if (member.type != RHIParamType::kUAVBuffer) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Shader '{}' defines '{}' as storage buffer but parameter has incompatible type",
                       source_location_, sb.name);
            }
        }
    }

}


bool RDGShader::Recompile() {
    // Re-compile the shader
    // Firstly, load the source code from the source location
    auto source_code = LoadFile(source_location_);
    if (source_code.empty()) {
        MI_LOG(MIInfraLogType::kError, "Failed to load shader source: {}", source_location_);
        return false;
    }
    // Reflect shader struct param info
    const RDGShaderParamStructInfo & param_info = *child_methods_.GetShaderParamInfo();

    // Compile the shader and create RHI shaders
    std::string errmsg;
    std::wstring source_location_wstr(source_location_.begin(), source_location_.end());
    if(type_ == RHIPipelineType::kCompute) {
        auto result = GetInfra().CompileHLSLToSPIRV(
                source_location_wstr.c_str(), std::string(GetComputeEntryPoint()), "cs_6_6",
                std::span(source_code.data(), source_code.size()), {}, errmsg
        );
        if (result.empty()) {
            MI_LOG(MIInfraLogType::kError, "Failed to compile shader: {}", errmsg);
            return false;
        }
        // Create the compute shader
        auto bytecode_span = std::span(reinterpret_cast<const std::byte *>(result.data()), result.size() * sizeof(uint32_t));
        auto shader = RHI::Get().CreateShader(
                RHIShaderFrequencyFlagBits::kCompute, GetComputeEntryPoint(),
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
                source_location_wstr.c_str(), std::string(GetVertexEntryPoint()), "vs_6_6",
                std::span(source_code.data(), source_code.size()), {}, errmsg
        );
        if (vs_result.empty()) {
            MI_LOG(MIInfraLogType::kError, "Failed to compile vertex shader: {}", errmsg);
            return false;
        }

        // Then compile fragment shader
        auto fs_result = GetInfra().CompileHLSLToSPIRV(
                source_location_wstr.c_str(), std::string(GetFragmentEntryPoint()), "ps_6_6",
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
}

MI_NAMESPACE_END