/*
 * Created: 2025/4/25
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "infra_impl/infra.h"
#include "core/util/debug_prof.h"
#include <spirv_cross/spirv_hlsl.hpp>

using namespace mi;

std::span<const char> ReadFromFile(std::filesystem::path path) {
    static std::vector<char> buffer;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        printf("Failed to open file: %s", path.string().c_str());
        return {};
    }

    auto size = file.tellg();
    buffer.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(buffer.data(), size);
    file.close();

    return {buffer.data(), buffer.size()};
}

int main () {

    auto pwd = std::filesystem::current_path();

    // Transfer ownership of underlying infrastructure and initialize
    auto infra = std::make_unique<MyInfra>();
    TransferInfra(std::move(infra));
    GetInfra().Init();

    std::vector<std::string> options;
    std::string error;
    auto path = pwd / "resources" / "test_shader.hlsl";
    auto hlsl_code = ReadFromFile(path);

    auto spv_code = GetInfra().CompileHLSLToSPIRV(path.c_str(), "Main", "cs_6_3", hlsl_code, options, error);
    if (!error.empty()) {
        printf("Error: %s", error.c_str());
        return -1;
    }

    auto compiler = spirv_cross::CompilerHLSL(spv_code);
    auto resources = compiler.get_shader_resources();
    printf("%d\n", (int)resources.storage_buffers.size());
    auto type_id = compiler.get_type(resources.storage_buffers[0].type_id);
    auto array_size = compiler.get_type(resources.storage_buffers[0].type_id).array[0];
    auto is_literal = compiler.get_type(resources.storage_buffers[0].type_id).array_size_literal[0];
    printf("%d %s\n", (int)array_size, is_literal ? "yes" : "no");

    GetInfra().Shutdown();
    DestroyInfra();
}