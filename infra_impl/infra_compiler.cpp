/*
 * Created: 2024/9/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

// Use DXC to compile HLSL to SPIRV
// TODO: cross platform
#include <Windows.h>
#include <dxcapi.h>


#include <iostream>
#include <stringapiset.h>
#include "infra_impl/infra.h"

MI_NAMESPACE_BEGIN

struct HLSLCompilerContext {
    IDxcLibrary *dxc_lib;
    IDxcCompiler *dxc_compiler;
};

HLSLCompilerContext * MyInfra::GetHLSLCompilerContextForThread(std::thread::id thread_id) {
    auto it = hlsl_compiler_contexts_.find(thread_id);
    if(it == hlsl_compiler_contexts_.end()) {
        auto ctx = new HLSLCompilerContext;
        DxcCreateInstance(CLSID_DxcLibrary, __uuidof(IDxcLibrary), (void **)&ctx->dxc_lib);
        DxcCreateInstance(CLSID_DxcCompiler, __uuidof(IDxcCompiler), (void **)&ctx->dxc_compiler);
        hlsl_compiler_contexts_[thread_id] = ctx;
        return ctx;
    } else {
        return it->second;
    }
}

void MyInfra::DestroyHLSLCompilerContexts() {
    for(auto & [thread_id, ctx] : hlsl_compiler_contexts_) {
        ctx->dxc_lib->Release();
        ctx->dxc_compiler->Release();
        delete ctx;
    }
    hlsl_compiler_contexts_.clear();
}

std::vector<uint32_t>
MyInfra::CompileHLSLToSPIRV(std::span<const char> hlsl_code, std::vector<std::string> options, std::string & error) {
    // Initialize DXC (if not already)
    auto ctx = GetHLSLCompilerContextForThread(std::this_thread::get_id());
    IDxcLibrary *dxc_lib = ctx->dxc_lib;
    IDxcCompiler *dxc_compiler = ctx->dxc_compiler;


    // Create blob from hlsl code
    IDxcBlobEncoding *hlsl_blob;
    dxc_lib->CreateBlobWithEncodingFromPinned(hlsl_code.data(), (uint32_t)hlsl_code.size(), CP_UTF8, &hlsl_blob);

    // Compile
    IDxcOperationResult *compile_result;
    dxc_compiler->Compile(hlsl_blob, L"shader.hlsl", L"main", L"ps_6_0", reinterpret_cast<LPCWSTR *>(options.data()), (uint32_t)options.size(), nullptr, 0, nullptr, &compile_result);

    // Check compile result
    HRESULT hr;
    compile_result->GetStatus(&hr);
    if (FAILED(hr)) {
        IDxcBlobEncoding *error_blob;
        compile_result->GetErrorBuffer(&error_blob);
        std::string error_message(static_cast<const char *>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
        error = error_message;
        return {};
    }

    // Get SPIRV blob
    IDxcBlob *spirv_blob;
    compile_result->GetResult(&spirv_blob);
    std::vector<uint32_t> spirv(spirv_blob->GetBufferSize() / sizeof(uint32_t));
    memcpy(spirv.data(), spirv_blob->GetBufferPointer(), spirv_blob->GetBufferSize());
    return spirv;
}

MI_NAMESPACE_END