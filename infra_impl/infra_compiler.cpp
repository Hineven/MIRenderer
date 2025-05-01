/*
 * Created: 2024/9/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

// 跨平台实现：使用DXC编译HLSL到SPIRV
#ifdef _WIN32
#include <Windows.h>
#include <dxcapi.h>
#else
#include <dlfcn.h>
#include "dxc_linux.h"  // 我们将创建这个头文件来处理Linux上的DXC接口
#endif

#include <iostream>
#include <string>
#include <codecvt>
#include <locale>
#include "infra_impl/infra.h"

MI_NAMESPACE_BEGIN

struct HLSLCompilerContext {
    IDxcLibrary *dxc_lib;
    IDxcCompiler *dxc_compiler;
#ifndef _WIN32
    void* dxc_library_handle; // Linux上的动态库句柄
#endif
};

// 辅助函数：UTF-8字符串转宽字符串
std::wstring Utf8ToWide(const std::string& str) {
#ifdef _WIN32
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &result[0], size_needed);
    return result;
#else
    // Linux上没有MultiByteToWideChar，继续使用std::wstring_convert
    // 虽然被弃用，但仍然可用
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    return converter.from_bytes(str);
#endif
}

HLSLCompilerContext * MyInfra::GetHLSLCompilerContextForThread(std::thread::id thread_id) {
    auto it = hlsl_compiler_contexts_.find(thread_id);
    if(it == hlsl_compiler_contexts_.end()) {
        auto ctx = new HLSLCompilerContext;

#ifdef _WIN32
        // Windows实现
        DxcCreateInstance(CLSID_DxcLibrary, __uuidof(IDxcLibrary), (void **)&ctx->dxc_lib);
        DxcCreateInstance(CLSID_DxcCompiler, __uuidof(IDxcCompiler), (void **)&ctx->dxc_compiler);
#else
        // Linux实现
        ctx->dxc_library_handle = dlopen("libdxcompiler.so", RTLD_LAZY);
        if (!ctx->dxc_library_handle) {
            std::cerr << "Failed to load DXC library: " << dlerror() << std::endl;
            delete ctx;
            return nullptr;
        }

        // 获取创建实例的函数指针
        auto DxcCreateInstance = (DxcCreateInstanceProc)dlsym(ctx->dxc_library_handle, "DxcCreateInstance");
        if (!DxcCreateInstance) {
            std::cerr << "Failed to get DxcCreateInstance function: " << dlerror() << std::endl;
            dlclose(ctx->dxc_library_handle);
            delete ctx;
            return nullptr;
        }

        // 创建DXC实例
        HRESULT hr = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&ctx->dxc_lib));
        if (FAILED(hr)) {
            std::cerr << "Failed to create DXC library instance" << std::endl;
            dlclose(ctx->dxc_library_handle);
            delete ctx;
            return nullptr;
        }

        hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&ctx->dxc_compiler));
        if (FAILED(hr)) {
            std::cerr << "Failed to create DXC compiler instance" << std::endl;
            ctx->dxc_lib->Release();
            dlclose(ctx->dxc_library_handle);
            delete ctx;
            return nullptr;
        }
#endif

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
#ifndef _WIN32
        if (ctx->dxc_library_handle) {
            dlclose(ctx->dxc_library_handle);
        }
#endif
        delete ctx;
    }
    hlsl_compiler_contexts_.clear();
}

std::vector<uint32_t>
MyInfra::CompileHLSLToSPIRV(
        const wchar_t * shader_path,
        std::string entry_point,
        std::string target_profile,
        std::span<const char> hlsl_code,
        std::vector<std::string> options,
        std::string & error) {
    // 初始化DXC (如果尚未初始化)
    auto ctx = GetHLSLCompilerContextForThread(std::this_thread::get_id());
    if (!ctx) {
        error = "Failed to initialize DXC compiler";
        return {};
    }
    
    IDxcLibrary *dxc_lib = ctx->dxc_lib;
    IDxcCompiler *dxc_compiler = ctx->dxc_compiler;

    // 从HLSL代码创建blob
    IDxcBlobEncoding *hlsl_blob;
    dxc_lib->CreateBlobWithEncodingFromPinned(hlsl_code.data(), (uint32_t)hlsl_code.size(), CP_UTF8, &hlsl_blob);

    // 转换参数
    std::wstring entry_point_w = Utf8ToWide(entry_point);
    std::wstring target_profile_w = Utf8ToWide(target_profile);

    // 转换编译选项
    auto w_options = std::vector<std::wstring>(options.size());
    for (size_t i = 0; i < options.size(); i++) {
        w_options[i] = Utf8ToWide(options[i]);
    }
    
    auto add_option = [&](std::wstring option) {
        for(auto & opt : w_options) {
            if(opt == option) {
                return;
            }
        }
        w_options.push_back(option);
    };
    
    // Instruct dxc to compile adequate SPIRV
    add_option(L"-spirv");
    add_option(L"-Ges");
    add_option(L"-fspv-reflect");
    add_option(L"-fspv-debug=vulkan-with-source");

    auto w_options_cstr = std::vector<const wchar_t *>(w_options.size());
    for (size_t i = 0; i < w_options.size(); i++) {
        w_options_cstr[i] = w_options[i].c_str();
    }
    
    // 编译
    IDxcOperationResult *compile_result;
    dxc_compiler->Compile(hlsl_blob, shader_path, entry_point_w.c_str(), target_profile_w.c_str(),
                          w_options_cstr.data(), (uint32_t)w_options_cstr.size(),
                          nullptr, 0, nullptr, &compile_result);

    // 检查编译结果
    HRESULT hr;
    compile_result->GetStatus(&hr);
    if (FAILED(hr)) {
        IDxcBlobEncoding *error_blob;
        compile_result->GetErrorBuffer(&error_blob);
        error.resize(error_blob->GetBufferSize());
        memcpy(error.data(), error_blob->GetBufferPointer(), error_blob->GetBufferSize());
        error_blob->Release();
        compile_result->Release();
        hlsl_blob->Release();
        return {};
    }

    // 获取SPIRV blob
    IDxcBlob *spirv_blob;
    compile_result->GetResult(&spirv_blob);
    std::vector<uint32_t> spirv(spirv_blob->GetBufferSize() / sizeof(uint32_t));
    memcpy(spirv.data(), spirv_blob->GetBufferPointer(), spirv_blob->GetBufferSize());
    
    // 释放资源
    spirv_blob->Release();
    compile_result->Release();
    hlsl_blob->Release();
    
    return spirv;
}

MI_NAMESPACE_END
