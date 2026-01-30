/*
 * Created: 2024/9/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

// 跨平台实现：使用DXC编译HLSL到SPIRV
#ifdef _WIN32
#include <Windows.h>
#include <directx-dxc/dxcapi.h>
#else
#include <dlfcn.h>
#include "dxc_linux.h"  // 处理Linux上的DXC接口
#endif

#include <xxhash.h>
#include <iostream>
#include <string>
#include <codecvt>
#include <locale>
#include <filesystem>
#include <atomic>

#include "infra_impl/infra.h"
// 不再需要包含ShaderIncludeCollector
// #include "infra_impl/shader_include_collector.h"

MI_NAMESPACE_BEGIN

// Custom include handler that routes file reads through Infra RIO_Open only
class InfraIncludeHandler : public IDxcIncludeHandler {
public:
    InfraIncludeHandler(MyInfra* infra, IDxcUtils* dxc_lib)
        : infra_(infra), dxc_lib_(dxc_lib) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IDxcIncludeHandler) || riid == __uuidof(IUnknown)) {
            *ppvObject = static_cast<IDxcIncludeHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_count_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG newCount = --ref_count_;
        if (newCount == 0) delete this;
        return newCount;
    }

    // IDxcIncludeHandler
    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename, IDxcBlob** ppIncludeSource) override {
        if (!pFilename || !ppIncludeSource) return E_INVALIDARG;
        *ppIncludeSource = nullptr;
        try {
            // Convert include path to resource path (relative to resource dir if absolute)
            std::filesystem::path inc_path = std::filesystem::path(pFilename);
            std::filesystem::path res_dir = infra_->GetResourceDirectory();
            MIResourcePath res_path;
            if (inc_path.is_absolute()) {
                std::error_code ec;
                auto rel = std::filesystem::relative(inc_path, res_dir, ec);
                res_path = (!ec && !rel.empty()) ? rel.generic_string() : inc_path.generic_string();
            } else {
                res_path = inc_path.generic_string();
            }

            // Open via Infra only
            TRef<BlobResourceInterface> blob = infra_->RIO_Open_Volatile(res_path, MIInfraResourceHintType::kShaderSource, BlobResourceAccessFlagBits::kRead);
            if (!blob.Raw()) {
                return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            }
            size_t size = blob->GetSize();
            std::vector<char> buf(size);
            if (size) blob->ReadBlob(0, size, buf.data());

            IDxcBlobEncoding* out_blob = nullptr;
            HRESULT hr = dxc_lib_->CreateBlob(buf.data(), (UINT32)size, CP_UTF8, &out_blob);
            if (FAILED(hr)) return hr;
            *ppIncludeSource = out_blob;
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }

private:
    std::atomic<ULONG> ref_count_{1};
    MyInfra* infra_ {nullptr};
    IDxcUtils* dxc_lib_ {nullptr};
};

struct HLSLCompilerContext {
    IDxcUtils *dxc_lib {nullptr};
    IDxcCompiler3 *dxc_compiler {nullptr};
    IDxcIncludeHandler *include_handler {nullptr};
#ifndef _WIN32
    void* dxc_library_handle {nullptr}; // Linux上的动态库句柄
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

static std::vector<DxcDefine> ConvertDefines(const std::vector<std::string>& defines_strings,
                                             std::vector<std::wstring>& out_names,
                                             std::vector<std::wstring>& out_values) {
    out_names.clear();
    out_values.clear();
    out_names.reserve(defines_strings.size());
    out_values.reserve(defines_strings.size());
    std::vector<DxcDefine> defines;
    defines.reserve(defines_strings.size());
    for (auto & e : defines_strings) {
        std::wstring define_str(Utf8ToWide(e));
        size_t equal_pos = define_str.find(L'=');
        std::wstring define_name, define_value;
        if (equal_pos != std::wstring::npos) {
            // Use substr to avoid iterator difference narrowing warnings
            define_name = define_str.substr(0, equal_pos);
            define_value = define_str.substr(equal_pos + 1);
        } else {
            define_name = define_str; // 没有值
            define_value = L"";       // 空值
        }
        out_names.push_back(std::move(define_name));
        out_values.push_back(std::move(define_value));
        DxcDefine def{};
        def.Name  = out_names.back().c_str();
        def.Value = out_values.back().empty() ? nullptr : out_values.back().c_str(); // DXC 允许 Value 为 nullptr 表示仅定义宏名
        defines.push_back(def);
    }
    return defines;
}

HLSLCompilerContext * MyInfra::GetHLSLCompilerContextForThread(std::thread::id thread_id) {
    std::lock_guard<std::mutex> lock(hlsl_compiler_contexts_mutex_);
    auto it = hlsl_compiler_contexts_.find(thread_id);
    if(it == hlsl_compiler_contexts_.end()) {
        auto ctx = new HLSLCompilerContext;

#ifdef _WIN32
        DxcCreateInstance(CLSID_DxcLibrary, __uuidof(IDxcUtils), (void **)&ctx->dxc_lib);
        DxcCreateInstance(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void **)&ctx->dxc_compiler);
        // Use our Infra-backed include handler
        ctx->include_handler = new InfraIncludeHandler(this, ctx->dxc_lib);

#else
        // Linux实现
        ctx->dxc_library_handle = dlopen("libdxcompiler.so", RTLD_LAZY);
        if (!ctx->dxc_library_handle) {
            std::cerr << "Failed to load DXC library: " << dlerror() << std::endl;
            delete ctx;
            return nullptr;
        }

        auto DxcCreateInstance = (DxcCreateInstanceProc)dlsym(ctx->dxc_library_handle, "DxcCreateInstance");
        if (!DxcCreateInstance) {
            std::cerr << "Failed to get DxcCreateInstance function: " << dlerror() << std::endl;
            dlclose(ctx->dxc_library_handle);
            delete ctx;
            return nullptr;
        }

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
        // Use our Infra-backed include handler
        ctx->include_handler = new InfraIncludeHandler(this, ctx->dxc_lib);
#endif
        // Manual ref management for COM-like interfaces
        // DxcCreateInstance returns objects with refcount = 1, and InfraIncludeHandler starts with refcount = 1.
        // We store the owning references directly and release them once in DestroyHLSLCompilerContexts.
        // Calling AddRef here leaves refcount at 2 and leaks when releasing only once later.
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
#ifdef _WIN32

#endif
#ifndef _WIN32
        if (ctx->dxc_library_handle) {
            dlclose(ctx->dxc_library_handle);
        }
#endif
        delete ctx;
    }
    hlsl_compiler_contexts_.clear();
}

static std::vector<std::wstring> GetImplicitCompileOptions (const wchar_t * shader_path, std::vector<std::string> options, [[maybe_unused]] bool preprocess_only = false) {
    auto w_options = std::vector<std::wstring>(options.size());
    for (size_t i = 0; i < options.size(); i++) {
        w_options[i] = Utf8ToWide(options[i]);
    }

    auto add_option = [&](const std::wstring & option) {
        for(auto & opt : w_options) {
            if(opt == option) {
                return;
            }
        }
        w_options.push_back(option);
    };

    // TODO Migrate the flags to the renderer logic
    if (true) {
        // Instruct dxc to compile adequate SPIRV
        add_option(L"-spirv");
        add_option(L"-fspv-target-env=universal1.5"); // Highest version
        // Compatibility
        add_option(L"-fvk-use-dx-layout"); // Use struct memory layouts specified in DirectX
        add_option(L"-fspv-use-vulkan-memory-model"); // Use Vulkan memory model (see that in Vulkan spec)
        add_option(L"-Ges"); // Strict mode
        add_option(L"-disable-payload-qualifiers"); // Disable DXR 1.1 ray payload qualifiers
#if MI_ENABLE_SHADER_DEBUGGING
        // Debugging flag
        add_option(L"-Zi"); // Generate debug information
#endif
        // Warnings as errors
        add_option(L"-WX");
    }
    // Add a default include path (same as the shader parent directory)
    try {
        std::filesystem::path shader_fs_path = std::filesystem::absolute(std::wstring(shader_path));
        auto parent_path = shader_fs_path.parent_path();
        if (std::filesystem::exists(parent_path)) {
            std::wstring parent_path_w = Utf8ToWide(parent_path.string());
            add_option(L"-I");
            add_option(parent_path_w);
        }
    } catch (...) {
        // ignore
    }
    // Always add the global renderer shader directory for shared includes
    try {
        std::filesystem::path global_shader_dir = std::filesystem::absolute("mi/renderer/shaders");
        if (std::filesystem::exists(global_shader_dir)) {
            std::wstring global_shader_dir_w = Utf8ToWide(global_shader_dir.string());
            add_option(L"-I");
            add_option(global_shader_dir_w);
        }
    } catch (...) {
        // ignore
    }
    return w_options;
}

// 添加预处理并计算哈希的辅助函数
static uint64_t PreprocessAndComputeHash(
        IDxcCompiler3* dxc_compiler,
        IDxcUtils *dxc_utils,
        IDxcIncludeHandler* include_handler,
        DxcBuffer* source,
        const wchar_t* shader_path,
        const wchar_t* entry_point,
        const wchar_t* target_profile,
        const std::vector<DxcDefine>& defines,
        std::vector<const wchar_t*> options_cstr,
        uint32_t options_count) {
    // 进行预处理操作
    IDxcOperationResult *preprocess_result;
    // Make a local copy if we ever want to append internal defines (currently none)
    auto local_defines = defines;
    local_defines.push_back({L"MI_PREPROCESSING", nullptr});
    IDxcCompilerArgs * compiler_args;
    // Preprocess only
    dxc_utils->BuildArguments(
        shader_path, nullptr, nullptr,
        options_cstr.data(), options_count,
        local_defines.data(), (uint32_t)local_defines.size(),
        &compiler_args
    );
    auto preprocess_arg = L"-P";
    compiler_args->AddArguments(&preprocess_arg, 1); // Preprocess only
    dxc_compiler->Compile(
        source,
        compiler_args->GetArguments(),
        compiler_args->GetCount(),
        include_handler,
        IID_PPV_ARGS(&preprocess_result)
    );
    compiler_args->Release();

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
            auto count = error_blob->GetBufferSize() ? error_blob->GetBufferSize() - 1 : 0;
            std::string error_message(static_cast<const char*>(error_blob->GetBufferPointer()), count);
            MI_LOG(MIInfraLogType::kError, "HLSL Preprocessing Error: {}", error_message);
            error_blob->Release();
        } else {
            MI_LOG(MIInfraLogType::kError, "HLSL Preprocessing Failed with unknown error.");
        }
    }

    preprocess_result->Release();
    return hash_value;
}

static uint64_t GetHashForOptionsAndDefines(
        const std::vector<std::string>& options,
        const std::vector<std::string>& defines) {
    uint64_t hash_value = 0;
    for (const auto& e : options) {
        hash_value = XXH64(e.c_str(), e.size() * sizeof(char), hash_value);
    }
    for (const auto& e : defines) {
        hash_value = XXH64(e.c_str(), e.size() * sizeof(char), hash_value);
    }
    return hash_value;
}

std::vector<uint32_t>
MyInfra::CompileHLSLToSPIRV(
        const wchar_t * shader_path,
        std::string entry_point,
        std::string target_profile,
        std::span<const char> hlsl_code,
        std::vector<std::string> defines,
        std::vector<std::string> options,
        std::string & error, std::wstring * out_compile_command,
        uint64_t * out_shader_xxhash64) {

    auto ctx = GetHLSLCompilerContextForThread(std::this_thread::get_id());
    if (!ctx) {
        error = "Failed to initialize DXC compiler";
        return {};
    }

    IDxcUtils *dxc_lib = ctx->dxc_lib;
    IDxcCompiler3 *dxc_compiler = ctx->dxc_compiler;

    IDxcBlobEncoding *hlsl_blob;
    dxc_lib->CreateBlob(hlsl_code.data(), (uint32_t)hlsl_code.size(), CP_UTF8, &hlsl_blob);

    std::wstring entry_point_w = Utf8ToWide(entry_point);
    std::wstring target_profile_w = Utf8ToWide(target_profile);

    if (out_shader_xxhash64) {
        *out_shader_xxhash64 = GetHashForOptionsAndDefines(options, defines);
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
    std::vector<std::wstring> define_names; // 保持生命周期，避免悬垂指针
    std::vector<std::wstring> define_values;
    auto defines_dxc = ConvertDefines(defines, define_names, define_values);
    auto source_buffer = DxcBuffer{
        hlsl_blob->GetBufferPointer(),
        hlsl_blob->GetBufferSize(),
        DXC_CP_ACP
    };
    if (out_shader_xxhash64) {
        // 计算预处理后代码的哈希值
        *out_shader_xxhash64 ^= PreprocessAndComputeHash(
            dxc_compiler,
            dxc_lib,
            ctx->include_handler,
            &source_buffer,
            shader_path,
            entry_point_w.c_str(),
            target_profile_w.c_str(),
            defines_dxc,
            prep_options_cstr,
            (uint32_t)prep_options_cstr.size()
        );
    }

    // 进行实际编译
    try {
        IDxcCompilerArgs * compiler_args;
        dxc_lib->BuildArguments(
            shader_path, entry_point_w.c_str(), target_profile_w.c_str(),
            w_options_cstr.data(), (uint32_t)w_options.size(),
            defines_dxc.data(), (uint32_t)defines_dxc.size(),
            &compiler_args
        );
        IDxcOperationResult* compile_result {};
        dxc_compiler->Compile(
            &source_buffer, compiler_args->GetArguments(), compiler_args->GetCount(),
            ctx->include_handler, IID_PPV_ARGS(&compile_result)
        );
        compiler_args->Release();

        if (compile_result) {
            HRESULT hr;
            compile_result->GetStatus(&hr);
            if (FAILED(hr)) {
                IDxcBlobEncoding* error_blob;
                compile_result->GetErrorBuffer(&error_blob);
                error.resize(error_blob->GetBufferSize() - 1); // -1 to exclude null terminator
                memcpy(error.data(), error_blob->GetBufferPointer(), error_blob->GetBufferSize() - 1);
                error_blob->Release();
                compile_result->Release();
                hlsl_blob->Release();
                return {};
            }


            IDxcBlob* spirv_blob;
            compile_result->GetResult(&spirv_blob);
            std::vector<uint32_t> spirv(spirv_blob->GetBufferSize() / sizeof(uint32_t));
            memcpy(spirv.data(), spirv_blob->GetBufferPointer(), spirv_blob->GetBufferSize());

            spirv_blob->Release();
            compile_result->Release();
            hlsl_blob->Release();
            return spirv;
        }
        else {
            MI_WARN("DXC Compile returned null operation result. (This possibly means a bug within dxc.)");
            hlsl_blob->Release();
            return {};
        }
    }
    catch (...) {
        error = "DXC Compile threw an exception.";
        hlsl_blob->Release();
        return {};
    }
}

uint64_t MyInfra::GetShaderXXHashFromShaderResourcePath(
        const MIResourcePath & res_path,
        std::string entry_point,
        std::string target_profile,
        std::vector<std::string> defines,
        std::vector<std::string> options,
        bool & is_shader_valid) {

    std::wstring entry_point_w = Utf8ToWide(entry_point);
    std::wstring target_profile_w = Utf8ToWide(target_profile);

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
    std::streamoff file_size_off = file.tellg();
    if (file_size_off <= 0) { file.close(); return 0; }
    file.seekg(0, std::ios::beg);
    auto file_size = static_cast<size_t>(file_size_off);

    std::string hlsl_code(file_size, '\0');
    file.read(hlsl_code.data(), static_cast<std::streamsize>(file_size));
    file.close();

    // 获取 DXC 编译上下文
    auto ctx = GetHLSLCompilerContextForThread(std::this_thread::get_id());
    if (!ctx) {
        return 0; // 无法初始化编译器
    }

    IDxcUtils *dxc_lib = ctx->dxc_lib;
    IDxcCompiler3 *dxc_compiler = ctx->dxc_compiler;

    // 创建着色器代码 blob
    IDxcBlobEncoding *hlsl_blob;
    dxc_lib->CreateBlob(hlsl_code.data(), (uint32_t)hlsl_code.size(), DXC_CP_ACP, &hlsl_blob);

    // 准备预处理选项（添加-P选项）
    auto w_options = GetImplicitCompileOptions(shader_path_w.c_str(), options, true);

    auto w_options_cstr = std::vector<const wchar_t *>(w_options.size());
    for (size_t i = 0; i < w_options.size(); i++) {
        w_options_cstr[i] = w_options[i].c_str();
    }
    std::vector<std::wstring> define_names; // 保持生命周期
    std::vector<std::wstring> define_values;
    auto defines_dxc = ConvertDefines(defines, define_names, define_values);

    uint64_t hash_value = GetHashForOptionsAndDefines(options, defines);

    auto source_buffer = DxcBuffer{
        hlsl_blob->GetBufferPointer(),
        hlsl_blob->GetBufferSize(),
        DXC_CP_ACP
    };
    // 计算预处理后代码的哈希值
    hash_value ^= PreprocessAndComputeHash(
        dxc_compiler,
        dxc_lib,
        ctx->include_handler,
        &source_buffer,
        shader_path_w.c_str(),
        entry_point_w.c_str(),
        target_profile_w.c_str(),
        defines_dxc,
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
