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
#include "dxc_linux.h"  // 处理Linux上的DXC接口
#endif

#include <xxhash.h>
#include <iostream>
#include <string>
#include <codecvt>
#include <locale>
#include <unordered_set>

#include "infra_impl/infra.h"
// 不再需要包含ShaderIncludeCollector
// #include "infra_impl/shader_include_collector.h"

MI_NAMESPACE_BEGIN

struct HLSLCompilerContext {
    IDxcLibrary *dxc_lib;
    IDxcCompiler *dxc_compiler;
    IDxcIncludeHandler *include_handler;
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
        ctx->dxc_lib->CreateIncludeHandler(&ctx->include_handler);
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
        hr = ctx->dxc_lib->CreateIncludeHandler(&ctx->include_handler);
#endif
        // 古法引用计数，匠心独运，传承配方（主要是因为ComPtr Linux上还得写代码）
        ctx->dxc_compiler->AddRef();
        ctx->dxc_lib->AddRef();
        ctx->include_handler->AddRef();
        hlsl_compiler_contexts_[thread_id] = ctx;
        return ctx;
    } else {
        return it->second;
    }
}

void MyInfra::DestroyHLSLCompilerContexts() {
    for(auto & [thread_id, ctx] : hlsl_compiler_contexts_) {
        // 古法引用释放
        ctx->include_handler->Release();
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

static std::vector<std::wstring> GetImplicitCompileOptions (const wchar_t * shader_path, std::vector<std::string> options, bool preprocess_only = false) {
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

    if (preprocess_only) {
        // 对于预处理，添加-P选项
        add_option(L"-P");
    } else {
        // Instruct dxc to compile adequate SPIRV
        add_option(L"-spirv");
        add_option(L"-fspv-target-env=universal1.5"); // Highest version
        // Compatibility
        add_option(L"-fvk-use-dx-layout");
        add_option(L"-fspv-use-vulkan-memory-model");
        add_option(L"-Ges"); // Strict mode
        // Debugging flag
        add_option(L"-Zi");
        add_option(L"-fspv-reflect");
        add_option(L"-fspv-debug=vulkan-with-source");
        // Warnings as errors
        add_option(L"-WX");
        // Debug printf (automatically used , no need to add it)
        // add_option(L"-fspv-extension=SPV_KHR_non_semantic_info");
    }
    // Add a default include path (same as the shader parent directory)
    std::filesystem::path shader_path_fs = shader_path;
    std::filesystem::path shader_dir = shader_path_fs.parent_path();
    std::wstring shader_dir_w = Utf8ToWide("\"" + shader_dir.string() + "\"");
    add_option(L"-I");
    add_option(shader_dir_w);
    return w_options;
}

// 添加预处理并计算哈希的辅助函数
static uint64_t PreprocessAndComputeHash(
        IDxcCompiler* dxc_compiler,
        IDxcIncludeHandler* include_handler,
        IDxcBlobEncoding* source_blob,
        const wchar_t* shader_path,
        const std::vector<const wchar_t*>& options_cstr,
        uint32_t options_count) {
    
    // 进行预处理操作
    IDxcOperationResult *preprocess_result;
    dxc_compiler->Compile(
        source_blob,
        shader_path,
        L"main", // 入口点名称不重要，因为我们只是预处理
        L"vs_6_0", // 目标配置文件不重要，因为我们只是预处理
        (LPCWSTR*)options_cstr.data(), options_count,
        nullptr, 0,
        include_handler, // 使用标准include handler
        &preprocess_result
    );
    
    // 检查预处理结果
    HRESULT hr;
    preprocess_result->GetStatus(&hr);
    uint64_t hash_value = 0;
    
    if (SUCCEEDED(hr)) {
        // 获取预处理后的代码
        IDxcBlob *preprocessed_code;
        preprocess_result->GetResult(&preprocessed_code);
        
        // 计算预处理后代码的哈希值
        hash_value = XXH64(preprocessed_code->GetBufferPointer(), preprocessed_code->GetBufferSize(), 0);
        
        preprocessed_code->Release();
    } else {
        // 如果预处理失败，获取错误信息
        IDxcBlobEncoding *error_blob;
        preprocess_result->GetErrorBuffer(&error_blob);
        if (error_blob) {
            std::string error_message(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize() - 1);
            MI_LOG(MIInfraLogType::kError, "HLSL Preprocessing Error: {}", error_message);
            error_blob->Release();
        } else {
            MI_LOG(MIInfraLogType::kError, "HLSL Preprocessing Failed with unknown error.");
        }
    }
    
    preprocess_result->Release();
    return hash_value;
}

std::vector<uint32_t>
MyInfra::CompileHLSLToSPIRV(
        const wchar_t * shader_path,
        std::string entry_point,
        std::string target_profile,
        std::span<const char> hlsl_code,
        std::vector<std::string> options,
        std::string & error, std::wstring * out_compile_command,
        uint64_t * out_shader_xxhash64) {

    auto ctx = GetHLSLCompilerContextForThread(std::this_thread::get_id());
    if (!ctx) {
        error = "Failed to initialize DXC compiler";
        return {};
    }
    
    IDxcLibrary *dxc_lib = ctx->dxc_lib;
    IDxcCompiler *dxc_compiler = ctx->dxc_compiler;

    IDxcBlobEncoding *hlsl_blob;
    dxc_lib->CreateBlobWithEncodingFromPinned(hlsl_code.data(), (uint32_t)hlsl_code.size(), CP_UTF8, &hlsl_blob);

    std::wstring entry_point_w = Utf8ToWide(entry_point);
    std::wstring target_profile_w = Utf8ToWide(target_profile);

    if (out_shader_xxhash64) {
        *out_shader_xxhash64 = 0;
        // Include extra options in the hash
        for (const auto& e : options) {
            *out_shader_xxhash64 = XXH64(e.c_str(), e.size() * sizeof(char), *out_shader_xxhash64);
        }
    }

    // 获取编译选项
    auto w_options = GetImplicitCompileOptions(shader_path, options);

    auto w_options_cstr = std::vector<const wchar_t *>(w_options.size());
    for (size_t i = 0; i < w_options.size(); i++) {
        w_options_cstr[i] = w_options[i].c_str();
    }

    if (out_compile_command) {
        std::wstring compile_command = L"dxc -E " + entry_point_w + L" -T " + target_profile_w;
        for (auto & opt : w_options) {
            compile_command += L" " + opt;
        }
        compile_command += L" \"" + std::wstring(shader_path) + L"\"";
        *out_compile_command = compile_command;
    }

    // 首先，计算预处理后代码的哈希值
    // 为了预处理，创建预处理选项
    auto prep_options = GetImplicitCompileOptions(shader_path, options, true);
    auto prep_options_cstr = std::vector<const wchar_t *>(prep_options.size());
    for (size_t i = 0; i < prep_options.size(); i++) {
        prep_options_cstr[i] = prep_options[i].c_str();
    }

    if (out_shader_xxhash64) {
        // 计算预处理后代码的哈希值
        *out_shader_xxhash64 ^= PreprocessAndComputeHash(
            dxc_compiler,
            ctx->include_handler,
            hlsl_blob,
            shader_path,
            prep_options_cstr,
            (uint32_t)prep_options_cstr.size()
        );
    }

    // 进行实际编译
    IDxcOperationResult *compile_result;
    dxc_compiler->Compile(hlsl_blob, shader_path, entry_point_w.c_str(), target_profile_w.c_str(),
                          w_options_cstr.data(), (uint32_t)w_options_cstr.size(),
                          nullptr, 0, ctx->include_handler, &compile_result);

    HRESULT hr;
    compile_result->GetStatus(&hr);
    if (FAILED(hr)) {
        IDxcBlobEncoding *error_blob;
        compile_result->GetErrorBuffer(&error_blob);
        error.resize(error_blob->GetBufferSize() - 1); // -1 to exclude null terminator
        memcpy(error.data(), error_blob->GetBufferPointer(), error_blob->GetBufferSize() - 1);
        error_blob->Release();
        compile_result->Release();
        hlsl_blob->Release();
        return {};
    }

    IDxcBlob *spirv_blob;
    compile_result->GetResult(&spirv_blob);
    std::vector<uint32_t> spirv(spirv_blob->GetBufferSize() / sizeof(uint32_t));
    memcpy(spirv.data(), spirv_blob->GetBufferPointer(), spirv_blob->GetBufferSize());

    spirv_blob->Release();
    compile_result->Release();
    hlsl_blob->Release();
    
    return spirv;
}

uint64_t MyInfra::GetShaderXXHashFromShaderResourcePath(
        const MIResourcePath & res_path,
        std::vector<std::string> options,
        bool & is_shader_valid) {
    
    is_shader_valid = false;
    
    // 转换资源路径为文件路径并尝试打开着色器文件
    std::filesystem::path shader_path = TranslateResPathToFilePath(res_path);
    if (!std::filesystem::exists(shader_path)) {
        return 0; // 文件不存在
    }
    
    std::wstring shader_path_w = shader_path.wstring();
    
    // 读取着色器源码
    std::ifstream file(shader_path, std::ios::binary);
    if (!file.is_open()) {
        return 0; // 无法打开文件
    }
    
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::string hlsl_code(file_size, '\0');
    file.read(hlsl_code.data(), file_size);
    file.close();
    
    // 获取 DXC 编译上下文
    auto ctx = GetHLSLCompilerContextForThread(std::this_thread::get_id());
    if (!ctx) {
        return 0; // 无法初始化编译器
    }
    
    IDxcLibrary *dxc_lib = ctx->dxc_lib;
    IDxcCompiler *dxc_compiler = ctx->dxc_compiler;
    
    // 创建着色器代码 blob
    IDxcBlobEncoding *hlsl_blob;
    dxc_lib->CreateBlobWithEncodingFromPinned(hlsl_code.data(), (uint32_t)hlsl_code.size(), CP_UTF8, &hlsl_blob);
    
    // 准备预处理选项（添加-P选项）
    auto w_options = GetImplicitCompileOptions(shader_path_w.c_str(), options, true);

    auto w_options_cstr = std::vector<const wchar_t *>(w_options.size());
    for (size_t i = 0; i < w_options.size(); i++) {
        w_options_cstr[i] = w_options[i].c_str();
    }

    uint64_t hash_value = 0;

    // Include extra options in the hash
    for (const auto& e : options) {
        hash_value = XXH64(e.c_str(), e.size() * sizeof(char), hash_value);
    }

    // 计算预处理后代码的哈希值
    hash_value ^= PreprocessAndComputeHash(
        dxc_compiler, 
        ctx->include_handler, 
        hlsl_blob, 
        shader_path_w.c_str(), 
        w_options_cstr, 
        (uint32_t)w_options_cstr.size()
    );
    
    // 如果哈希值不为0，说明预处理成功，shader有效
    is_shader_valid = (hash_value != 0);
    
    // 释放资源
    hlsl_blob->Release();
    
    return hash_value;
}

MI_NAMESPACE_END
