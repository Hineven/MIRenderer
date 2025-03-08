/*
 * Created: 2025/3/8
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_shader.h"
#include "core/infra.h"

MI_NAMESPACE_BEGIN

RDGShaderRegistrator::RDGShaderRegistrator(
        size_t type_hash,
        std::function<RDGShaderInitializationInfo()> get_init_info
) {
    RDGShader * shader = new RDGShader(get_init_info());
    RDGShaderLibrary::GetInstance().RegisterShader(typeid(*shader).hash_code(), shader);
}

RDGShader::RDGShader(RDGShaderInitializationInfo ini)
        : source_location_(std::move(ini.source_location)),
          entry_point_(std::move(ini.entry_point)),
          type_(ini.type) {}

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

bool RDGShader::Recompile() {
    // Re-compile the shader
    // Firstly, load the source code from the source location
    auto source_code = LoadFile(source_location_);
    if (source_code.empty()) {
        MI_LOG(MIInfraLogType::kError, "Failed to load shader source: {}", source_location_);
        return false;
    }
    // Compile the shader
    std::string errmsg;
    GetInfra().CompileHLSLToSPIRV(
            source_location_.c_str(), entry_point_, type_ == RHIPipelineType::kCompute ? "cs_6_3" : "vs_6_3",
            std::span(source_code.data(), source_code.size()), {}, errmsg
    );
}

MI_NAMESPACE_END