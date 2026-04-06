#include "viewer_app.h"

#include <array>
#include <fstream>
#include <algorithm>
#include <ranges>
#include "vulkan/vulkan.hpp"
#include <glfw/glfw3.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include "imgui_impl_glfw.h"

#include "rhi/vk/vk_export.h"
#include "rdg/rdg_pool.h"
#include "rhi/rhi_buffer.h"
#include "rhi/rhi_texture.h"
#include "core/util/debug_prof.h"
#include "core/task.h"
#include "renderer/mi_renderer.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_texture.h"
#include "renderer/mi_static_mesh.h"
#include "renderer/mi_cvar.h"

#include "rdg/rdg_shader.h"
#include "util/texture_loader.h"
#include "util/gltf_loader.h"
#include "util/volprims_loader.h"
#include "util/gaussian_radiance_field_loader.h"

#include "3d_viewer.h"
#include "core/util/command_line.h"
#include "viewer_zmq.h"
#include "viewer_export_channel.h"
#include "viewer_commands.h"
#include "viewer_control.h"
#include "renderer/mi_renderer_view.h"
#include "core/pixel_format.h"

MI_NAMESPACE_BEGIN

static bool ReadbackRDGTextureToBytes(
    RDGTexture* rdg_tex,
    PixelFormatType fmt,
    std::vector<std::byte>& out_bytes,
    uint32_t& out_w,
    uint32_t& out_h
);

namespace {

struct PendingViewerFrameExportRequest {
    std::string name;
    ViewerFrameExportChannel channel;
};

enum class ViewerFrameExportSourceType {
    kRendererView,
    kTonemappedColor,
    kTonemappedPathTracing,
};

struct ViewerFrameExportBinding {
    ViewerFrameExportChannel channel;
    ViewerFrameExportSourceType source_type;
    RendererViewFrameExportResource renderer_resource {RendererViewFrameExportResource::kRadiance};
    PixelFormatType format {PixelFormatType::kUnknown};
};

constexpr std::array<ViewerFrameExportBinding, 18> kViewerFrameExportBindings = {{
    {ViewerFrameExportChannel::kRadiance, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kRadiance},
    {ViewerFrameExportChannel::kColor, ViewerFrameExportSourceType::kTonemappedColor, RendererViewFrameExportResource::kRadiance, PixelFormatType::kB8G8R8A8_SRGB},
    {ViewerFrameExportChannel::kOverlay, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kOverlay},
    {ViewerFrameExportChannel::kDepth, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kDepth},
    {ViewerFrameExportChannel::kGrfDepth, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kGrfDepth},
    {ViewerFrameExportChannel::kGrfOpacity, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kGrfOpacity},
    {ViewerFrameExportChannel::kTransmittance, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kTransmittance},
    {ViewerFrameExportChannel::kVisibility, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kVisibility},
    {ViewerFrameExportChannel::kNormal, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kNormal},
    {ViewerFrameExportChannel::kGeometryNormal, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kGeometryNormal},
    {ViewerFrameExportChannel::kAlbedo, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kAlbedo},
    {ViewerFrameExportChannel::kDiffuseDirect, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kDiffuseDirect},
    {ViewerFrameExportChannel::kDiffuseIndirect, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kDiffuseIndirect},
    {ViewerFrameExportChannel::kDenoisedDiffuseDirect, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kDenoisedDiffuseDirect},
    {ViewerFrameExportChannel::kDenoisedDiffuseIndirect, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kDenoisedDiffuseIndirect},
    {ViewerFrameExportChannel::kMotionVector, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kMotionVector},
    {ViewerFrameExportChannel::kPathTracing, ViewerFrameExportSourceType::kRendererView, RendererViewFrameExportResource::kPathTracingFilm},
    {ViewerFrameExportChannel::kTonemappedPathTracing, ViewerFrameExportSourceType::kTonemappedPathTracing, RendererViewFrameExportResource::kPathTracingFilm, PixelFormatType::kB8G8R8A8_SRGB},
}};

const ViewerFrameExportBinding* FindViewerFrameExportBinding(ViewerFrameExportChannel channel) {
    for (const auto& binding : kViewerFrameExportBindings) {
        if (binding.channel == channel) {
            return &binding;
        }
    }
    return nullptr;
}

RendererViewFrameExportDesc GetRequestedFrameExportDesc(const ViewerApp& app, ViewerFrameExportChannel channel) {
    if (!app.view_) {
        return {};
    }
    const auto* binding = FindViewerFrameExportBinding(channel);
    if (!binding) {
        return {};
    }
    if (binding->source_type == ViewerFrameExportSourceType::kRendererView) {
        return app.view_->GetFrameExportResource(binding->renderer_resource);
    }
    if (binding->source_type == ViewerFrameExportSourceType::kTonemappedColor) {
        return {app.tonemapped_color_export_.Raw(), binding->format};
    }
    return {app.tonemapped_path_tracing_export_.Raw(), binding->format};
}

bool HasExportRequestForSourceType(
    const std::vector<PendingViewerFrameExportRequest>& requests,
    ViewerFrameExportSourceType source_type
) {
    return std::ranges::any_of(requests, [source_type](const PendingViewerFrameExportRequest& request) {
        const auto* binding = FindViewerFrameExportBinding(request.channel);
        return binding && binding->source_type == source_type;
    });
}

bool HasPathTracingExportRequest(const std::vector<PendingViewerFrameExportRequest>& requests) {
    return std::ranges::any_of(requests, [](const PendingViewerFrameExportRequest& request) {
        return request.channel == ViewerFrameExportChannel::kPathTracing
            || request.channel == ViewerFrameExportChannel::kTonemappedPathTracing;
    });
}

void PrepareTonemappedExportTexture(
    ViewerApp& app,
    RenderGraphBuilder& builder,
    RDGTexture * source_texture,
    TRef<RDGTexture>& output_texture,
    const char * output_name,
    PostProcessingFlags post_processing_flags
) {
    output_texture = {};
    if (!app.view_ || !source_texture) {
        return;
    }

    output_texture = builder.CreateTexture2D(
        app.view_->film_width_,
        app.view_->film_height_,
        PixelFormatType::kB8G8R8A8_SRGB,
        RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransfer
    );
    output_texture->SetName(output_name);
    output_texture->SetExport();

    auto& renderer = Renderer::Get();
    renderer.DrawTextureToOutput(
        app.view_.get(),
        builder,
        source_texture,
        Renderer::DrawToOutputMappingType::eRadianceToSRGB,
        post_processing_flags,
        output_texture.Raw(),
        RHILoadOpType::kClear
    );
    renderer.DrawTextureToOutput(
        app.view_.get(),
        builder,
        app.view_->overlay_.Raw(),
        Renderer::DrawToOutputMappingType::eLinearToSRGB,
        PostProcessingFlagBits::eNone,
        output_texture.Raw(),
        RHILoadOpType::kLoad
    );
}

void PrepareSpecialFrameExports(
    ViewerApp& app,
    RenderGraphBuilder& builder,
    const std::vector<PendingViewerFrameExportRequest>& requests
) {
    app.tonemapped_color_export_ = {};
    app.tonemapped_path_tracing_export_ = {};
    if (!app.view_) {
        return;
    }

    auto& renderer = Renderer::Get();
    PostProcessingFlags scene_color_flags = PostProcessingFlagBits::eNone;
    if (auto* taa_cvar = CVarRegistry::GetInstance().GetCVar<bool>("r.postprocessing.enable_taa");
        taa_cvar != nullptr && taa_cvar->Get()) {
        scene_color_flags = scene_color_flags | PostProcessingFlagBits::eEnableTAA;
    }

    if (HasExportRequestForSourceType(requests, ViewerFrameExportSourceType::kTonemappedColor)) {
        PrepareTonemappedExportTexture(
            app,
            builder,
            app.view_->radiance_.Raw(),
            app.tonemapped_color_export_,
            "ViewerTonemappedColorExport",
            scene_color_flags
        );
    }

    if (HasPathTracingExportRequest(requests) && !app.view_->did_render_path_tracing_this_frame_) {
        renderer.RenderPathTracingForExport(app.view_.get(), builder);
    }

    if (HasExportRequestForSourceType(requests, ViewerFrameExportSourceType::kTonemappedPathTracing)) {
        auto path_tracing_desc = app.view_->GetFrameExportResource(RendererViewFrameExportResource::kPathTracingFilm);
        PrepareTonemappedExportTexture(
            app,
            builder,
            path_tracing_desc.texture,
            app.tonemapped_path_tracing_export_,
            "ViewerTonemappedPathTracingExport",
            PostProcessingFlagBits::eNone
        );
    }
}

static bool MarkRequestedFrameExport(ViewerApp& app, ViewerFrameExportChannel channel) {
    auto desc = GetRequestedFrameExportDesc(app, channel);
    if (!desc.IsValid()) {
        return false;
    }
    desc.texture->SetExport();
    return true;
}

static bool PackRequestedFrameExport(
    ViewerApp& app,
    const std::string& name,
    ViewerFrameExportChannel channel
) {
    auto desc = GetRequestedFrameExportDesc(app, channel);
    if (!desc.IsValid()) {
        return false;
    }

    std::vector<std::byte> bytes;
    uint32_t w = 0;
    uint32_t h = 0;
    if (!ReadbackRDGTextureToBytes(desc.texture, desc.format, bytes, w, h)) {
        return false;
    }

    ViewerApp::ExportedRenderResult res;
    res.bytes = std::move(bytes);
    res.width = w;
    res.height = h;
    res.format = desc.format;
    res.name = name;
    app.exported_render_results_.push_back(std::move(res));
    return true;
}

} // namespace

// Generic helper: read back an RDG texture into CPU bytes.
// Returns true on success and fills out parameters.
static bool ReadbackRDGTextureToBytes(
    RDGTexture* rdg_tex,
    PixelFormatType fmt,
    std::vector<std::byte>& out_bytes,
    uint32_t& out_w,
    uint32_t& out_h
) {
    if (!rdg_tex) return false;
    auto tex = rdg_tex->GetRHI();
    if (!tex) return false;

    auto& rhi = RHI::Get();
    auto& queue = rhi.GetGraphicsCommandQueue();

    const uint32_t w = tex->GetWidth();
    const uint32_t h = tex->GetHeight();
    const size_t bpp = GetPixelFormatBytesPerPixel(fmt);
    const size_t byte_size = size_t(w) * size_t(h) * bpp;

    auto readback = rhi.CreateBuffer(byte_size, RHIBufferUsageFlagBits::kReadback);

    queue.MemoryBarrier();
    queue.TextureBarrier(tex, RHITextureLayoutType::kTransferSrcOptimal,
        RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll,
        RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kRead);
    queue.CopyTextureToBuffer(tex, readback.Raw());

    // Mark usage for RDG lifetime/alias tracking as in existing patterns
    rdg_tex->Use(
        RHIPipelineStageFlagBits::kTransfer,
        RHIGPUAccessFlagBits::kTransferRead,
        RHITextureLayoutType::kTransferSrcOptimal
    );

    queue.MemoryBarrier();
    queue.WaitForIdle("Readback RDGTexture");

    out_bytes.resize(byte_size);
    void* ptr = readback->Map();
    if (ptr && byte_size > 0) {
        std::memcpy(out_bytes.data(), ptr, byte_size);
    }
    readback->Unmap();

    out_w = w;
    out_h = h;
    return true;
}

static std::string GetCurrentDateTimeString();
static std::string GenerateRandomString(uint32_t len = 4);
static GLFWwindow* StartWindow(const MainLoopStartConfig& cfg);

void ViewerApp::BakingState::ClearBakingState() {
    baking_camera_positions.clear();
    baking_camera_index = 0;
    baking_frame_index = 0;
    is_baking_mode = false;
}

static std::string GetCurrentDateTimeString() {
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &tm);
    return std::string(buffer);
}

static std::string GenerateRandomString(uint32_t len) {
    const char charset[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    const size_t max_index = (sizeof(charset) - 1);
    std::string str;
    str.resize(len);
    for (uint32_t i = 0; i < len; i++) {
        str[i] = charset[rand() % max_index];
    }
    return str;
}

static GLFWwindow* StartWindow(const MainLoopStartConfig& cfg) {
    if (!glfwInit()) {
        mi_assert(false, "Failed to initialize GLFW");
        return nullptr;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    std::string window_name = "MIRenderer";
    if (!cfg.window_name.empty()) window_name = cfg.window_name;
    GLFWwindow* window = glfwCreateWindow(
        cfg.window_width, cfg.window_height, window_name.c_str(),
        nullptr, nullptr
    );
    if (!window) {
        glfwTerminate();
        mi_assert(false, "Failed to create GLFW window");
        return nullptr;
    }

    return window;
}

static std::filesystem::path GetViewerAppConfigPath() {
    // Save next to executable.
    // We avoid Infra FS helpers as requested; using current_path() works when the process
    // is launched from its executable directory (the common case for this app).
    std::error_code ec;
    auto dir = std::filesystem::current_path(ec);
    if (ec) dir = std::filesystem::path(".");
    return dir / "viewer_app_config.json";
}

static void LoadConfig(nlohmann::json &out_config) {
    const auto path = GetViewerAppConfigPath();
    std::ifstream ifs(path);
    if (!ifs) {
        MI_LOG(MIInfraLogType::kInfo, "ViewerApp config not found: {}", path.string());
        out_config = {};
        return;
    }
    try {
        ifs >> out_config;
    } catch (const std::exception &e) {
        MI_WARN("Failed to read ViewerApp config {}: {}", path.string(), e.what());
        out_config = {};
    }
}

static void LoadPinnedCVarsFromConfig(const nlohmann::json &config, std::vector<CVarBase *> &out_pinned) {
    out_pinned.clear();

    if (!config.contains("pinned_cvars") || !config["pinned_cvars"].is_array()) {
        MI_WARN("ViewerApp config malformed: missing pinned_cvars array");
        return;
    }

    auto &reg = CVarRegistry::GetInstance();
    for (const auto &idJson : config["pinned_cvars"]) {
        if (!idJson.is_string()) continue;
        const std::string id = idJson.get<std::string>();
        if (id.empty()) continue;
        if (auto *cvar = reg.GetCVar(id)) {
            out_pinned.push_back(cvar);
        } else {
            MI_WARN("ViewerApp config: pinned cvar '{}' no longer exists", id);
        }
    }

    MI_LOG(MIInfraLogType::kInfo, "Loaded {} pinned cvars", out_pinned.size());
}

static void SaveConfig(const nlohmann::json &config) {
    const auto path = GetViewerAppConfigPath();
    try {
        std::ofstream ofs(path, std::ios::out | std::ios::trunc);
        if (!ofs) {
            MI_WARN("Failed to write ViewerApp config: {}", path.string());
            return;
        }
        ofs << std::setw(4) << config << std::endl;
        MI_LOG(MIInfraLogType::kInfo, "Saved ViewerApp config: {}", path.string());
    } catch (const std::exception &e) {
        MI_WARN("Failed to save ViewerApp config {}: {}", path.string(), e.what());
    }
}

void ViewerApp::Initialize(std::unique_ptr<MIInfraInterface>&& infra, const MainLoopStartConfig& cfg) {
    auto pwd = std::filesystem::current_path();

    TransferInfra(std::move(infra));
    GetInfra().Init();

    // Load persisted UI state.
    nlohmann::json json_config;
    LoadConfig(json_config);
    LoadPinnedCVarsFromConfig(json_config, pinned_cvars_);
    console_.Initialize(json_config);

    RegisterViewerCommands(*this);

    if (GetCurrentThreadType() != ThreadType::kUnknown) {
        mi_assert(false, "MainLoop: somehow the thread calling Start() is known.");
    }
    SetCurrentThreadType(ThreadType::kRenderThread);
    InitializePlatformMainThreadContext();

    glfwInit();

    {
        uint32_t extension_count = 0;
        auto extra_extensions = glfwGetRequiredInstanceExtensions(&extension_count);
        if (extension_count != 0) {
            VulkanRHICreateInfo info {};
            info.extra_instance_extension_count = extension_count;
            info.extra_instance_extensions = extra_extensions;
            RHI::InitializeSingleton(RHIType::kVulkan, &info);
            RHI::Get().ResetPipelineCache(4 * 1024 * 1024);
        } else {
            throw std::runtime_error("Failed to get required instance extensions");
        }
    }

    auto limits = GetInfra().GetResourceLimits();

    if (limits.max_high_performance_thread_count < 2) {
        mi_assert(false, "At least 2 high performance threads are required.");
    }
    int task_graph_hpt_count = std::max(limits.max_high_performance_thread_count - 2, 0);
    int total_task_graph_thread_count = task_graph_hpt_count + limits.max_low_performance_thread_count;
    if (total_task_graph_thread_count < 1) {
        mi_assert(false, "At least 3 threads are required.");
    }

    TaskGraph::InitializeSingleton(0, task_graph_hpt_count);

    auto& shader_lib = RDGShaderLibrary::Get();
    shader_lib.Init();

    MI_LOG(MIInfraLogType::kInfo, "Main loop initialization complete.");

    window_ = StartWindow(cfg);

    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui_ImplGlfw_InitForVulkan(window_, true);
        unsigned char* pixels;
        int width, height;
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        auto& rhi = RHI::Get();
        font_texture_ = rhi.CreateTexture(RHITextureType::k2D,
            {(uint32_t)width, (uint32_t)height, 1},
            PixelFormatType::kR8G8B8A8_UNORM,
            RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransferDst);
        auto staging = rhi.CreateBuffer(width * height * 4, RHIBufferUsageFlagBits::kStaging);
        memcpy(staging->Map(), pixels, width * height * 4);
        auto& cmd = rhi.GetGraphicsCommandQueue();
        cmd.TextureBarrier(font_texture_.Raw(), RHITextureLayoutType::kTransferDstOptimal,
            RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll, RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kWrite);
        cmd.CopyBufferToTexture(staging->GetSpan(), font_texture_.Raw());
        cmd.TextureBarrier(font_texture_.Raw(), RHITextureLayoutType::kShaderReadOnlyOptimal,
            RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll, RHIGPUAccessFlagBits::kWrite, RHIGPUAccessFlagBits::kRead);
        cmd.EnqueueTranslateAndSubmit();
        rhi.WaitForIdle();
        io.Fonts->SetTexID((ImTextureID)font_texture_.Raw());
        io.Fonts->ClearTexData();
    }

    VkSurfaceKHR surface_tmp;
    auto& rhi = RHI::Get();
    auto handles = static_cast<const VulkanRHIHandles*>(rhi.GetUnderlyingGraphicsAPIHandles());
    {
        auto result = glfwCreateWindowSurface(handles->instance, window_, nullptr, &surface_tmp);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create window surface");
        }
    }
    rhi.InitializeSwapChain(&surface_tmp, cfg.window_width, cfg.window_height);

    pool_ = RDGResourcePool::Create();

    resource_allocator_ = Create<DeviceBindlessResourceAllocator>();

    renderable_node_registry_ = Create<RenderableNodeRegistry>();

    Renderer::Get().Init(resource_allocator_.Raw(), pool_.Raw());

    // Initialize ZMQ server.
    ViewerZmqServer::Config zc {};
    zmq_server_ = std::make_unique<ViewerZmqServer>(this, zc);
    zmq_server_->Initialize();

}

void ViewerApp::Destroy() {

    zmq_server_.reset();

    // Persist UI state before tearing subsystems down.
    nlohmann::json json_config;
    json_config["pinned_cvars"] = nlohmann::json::array();
    for (auto *cvar : pinned_cvars_) {
        if (!cvar) continue;
        json_config["pinned_cvars"].push_back(cvar->GetId());
    }

    default_material_.SafeRelease();
    {
        arrow_mesh_x_instance_.SafeRelease();
        arrow_mesh_y_instance_.SafeRelease();
        arrow_mesh_z_instance_.SafeRelease();
        arrow_mesh_x_.SafeRelease();
        arrow_mesh_y_.SafeRelease();
        arrow_mesh_z_.SafeRelease();
        arrow_geometry_.SafeRelease();
    }

    view_.reset();

    loaded_scenes_.clear();

    if (scene_) {
        scene_->ForceFlushDelayedDestruction();
    }

    scene_.reset();

    sky_cube_.SafeRelease();

    Renderer::DestroySingleton();

    assert(pool_.GetRefCount() == 1);
    pool_.SafeRelease();

    renderable_node_registry_.SafeRelease();
    if (resource_allocator_) {
        resource_allocator_->ForceFlushDelayedDestruction();
    }
    resource_allocator_.SafeRelease();

    RDGShaderLibrary::Get().Deinit();

    TaskGraph::DestroySingleton();

    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    font_texture_.SafeRelease();

    RHI::DestroySingleton();

    console_.Destroy(json_config);
    SaveConfig(json_config);

    DestroyPlatformMainThreadContext();
    SetCurrentThreadType(ThreadType::kUnknown);

    GetInfra().Shutdown();
    DestroyInfra();

    glfwTerminate();
}

void ViewerApp::HandleNavigationInput(float delta_time) {
    const float move_speed = 1.f;
    const float mouse_sensitivity = 0.002f;
    float dt = delta_time;
    float move_interval = move_speed * dt;
    glm::vec3 camera_right = glm::normalize(glm::cross(view_->camera_.direction, glm::vec3(0.0f, 1.0f, 0.0f)));
    if (!ImGui::GetIO().WantCaptureKeyboard) {
        if (ImGui::IsKeyDown(ImGuiKey_W))
            view_->camera_.position += view_->camera_.direction * move_interval;
        if (ImGui::IsKeyDown(ImGuiKey_S))
            view_->camera_.position -= view_->camera_.direction * move_interval;
        if (ImGui::IsKeyDown(ImGuiKey_A))
            view_->camera_.position -= camera_right * move_interval;
        if (ImGui::IsKeyDown(ImGuiKey_D))
            view_->camera_.position += camera_right * move_interval;
        if (ImGui::IsKeyDown(ImGuiKey_Space))
            view_->camera_.position += view_->camera_.up * move_interval;
        if (ImGui::IsKeyDown(ImGuiKey_LeftShift))
            view_->camera_.position -= view_->camera_.up * move_interval;
    }

    static double last_mouse_x = 0.0, last_mouse_y = 0.0;
    static bool first_mouse = true;

    double mouse_x, mouse_y;
    glfwGetCursorPos(window_, &mouse_x, &mouse_y);

    if (first_mouse) {
        last_mouse_x = mouse_x;
        last_mouse_y = mouse_y;
        first_mouse = false;
    }

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !input_state_.dragging_ && !ImGui::GetIO().WantCaptureMouse) {
        float delta_x = static_cast<float>(mouse_x - last_mouse_x) * mouse_sensitivity;
        float delta_y = static_cast<float>(mouse_y - last_mouse_y) * mouse_sensitivity;

        glm::mat4 rotate_y = glm::rotate(glm::mat4(1.0f), -delta_x, glm::vec3(0, 1, 0));
        view_->camera_.direction = glm::vec3(rotate_y * glm::vec4(view_->camera_.direction, 0.0f));

        glm::mat4 rotate_x = glm::rotate(glm::mat4(1.0f), -delta_y, camera_right);
        view_->camera_.direction = glm::vec3(rotate_x * glm::vec4(view_->camera_.direction, 0.0f));

        view_->camera_.direction = glm::normalize(view_->camera_.direction);
    }

    last_mouse_x = mouse_x;
    last_mouse_y = mouse_y;
}

void ViewerApp::HandleKeyboardShortcuts(FrameInternalDelayedOps& ops) {
    if (ImGui::GetIO().WantCaptureKeyboard) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
        ops.should_reload_shaders = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F)) {
        auto dir = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/camera_configs/");
        if (!std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
        }
        auto date_time = GetCurrentDateTimeString();
        auto random_str = GenerateRandomString(6);
        auto file_path = dir / (date_time + random_str + ".json");
        nlohmann::json j;
        j["camera_position"] = {view_->camera_.position.x, view_->camera_.position.y, view_->camera_.position.z};
        j["camera_direction"] = {view_->camera_.direction.x, view_->camera_.direction.y, view_->camera_.direction.z};
        j["camera_fov_Y"] = view_->camera_.fov_Y;
        j["camera_near_plane"] = view_->camera_.near_plane;
        j["camera_far_plane"] = view_->camera_.far_plane;
        j["camera_up"] = {view_->camera_.up.x, view_->camera_.up.y, view_->camera_.up.z};
        std::ofstream o(file_path);
        o << std::setw(4) << j << std::endl;
        MI_LOG(MIInfraLogType::kInfo, "Saved camera config to {}.", file_path.string());
    }

    if (ImGui::IsKeyPressed(ImGuiKey_E)) {
        ops.should_export_result = true;
    }
}

void ViewerApp::HandleControlUILogic(FrameInternalDelayedOps& ops, std::vector<RDGTimePeriod> time_periods, float cpu_duration) {
    ViewerControlUI::DrawControlUI(*this, ops, std::move(time_periods), cpu_duration);
}

void ViewerApp::ProcessClickSelect(FrameInternalDelayedOps& ops) {
    auto & io = ImGui::GetIO();
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && io.MouseDelta.x * io.MouseDelta.x + io.MouseDelta.y * io.MouseDelta.y > 0.3f) {
        selection_state_.mouse_moved_since_pressed = true;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !selection_state_.mouse_moved_since_pressed && !io.WantCaptureMouse) {
        if (io.MousePos.x > 0 && io.MousePos.y > 0) {
            ops.should_process_click_select = true;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        selection_state_.mouse_moved_since_pressed = false;
    }
}

void ViewerApp::ProcessDelayedOps(FrameInternalDelayedOps& ops) {

    auto & rhi = RHI::Get();

    if (ops.should_start_baking) {
        ops.should_start_baking = false;
        baking_state_.is_baking_mode = true;
        baking_state_.baking_frame_index = 0;
        baking_state_.baking_camera_index = 0;
        auto dir = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/camera_configs");
        baking_state_.baking_camera_positions.clear();
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() == ".json") {
                std::ifstream i(entry.path());
                nlohmann::json j;
                i >> j;
                Camera cam;
                cam.position = glm::vec3(j["camera_position"][0], j["camera_position"][1], j["camera_position"][2]);
                cam.direction = glm::vec3(j["camera_direction"][0], j["camera_direction"][1], j["camera_direction"][2]);
                cam.fov_Y = j["camera_fov_Y"];
                cam.near_plane = j["camera_near_plane"];
                cam.far_plane = j["camera_far_plane"];
                cam.up = glm::vec3(j["camera_up"][0], j["camera_up"][1], j["camera_up"][2]);
                baking_state_.baking_camera_positions.push_back(cam);
            }
        }
        MI_LOG(MIInfraLogType::kInfo, "Loaded {} baking camera positions.", baking_state_.baking_camera_positions.size());
        if (!baking_state_.baking_camera_positions.empty()) {
            view_->camera_ = baking_state_.baking_camera_positions[0];
        }
    }

    auto ExportViewResourceRaw = [&](const std::filesystem::path& dir, RendererViewFrameExportResource resource, const char* name) {
        auto desc = view_->GetFrameExportResource(resource);
        if (!desc.IsValid()) return;

        std::vector<std::byte> bytes;
        uint32_t w = 0;
        uint32_t h = 0;
        if (!ReadbackRDGTextureToBytes(desc.texture, desc.format, bytes, w, h)) return;

        const auto bpp = GetPixelFormatBytesPerPixel(desc.format);
        auto bin_path = dir / std::format("{}_{}x{}_{}bpp.bin", name, w, h, bpp);
        std::ofstream ofs(bin_path, std::ios::binary);
        if (ofs && !bytes.empty()) {
            ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }

        auto meta_path = dir / std::format("{}_meta.txt", name);
        std::ofstream meta(meta_path, std::ios::out);
        if (meta) {
            meta << std::format("name={}, width={}, height={}, bytes_per_pixel={}, fmt={}\n",
                name, w, h, bpp, ToString(desc.format));
        }
    };
    if (ops.should_export_result) {
        ops.should_export_result = false;

        auto now = std::chrono::system_clock::now();
        auto t_c = std::chrono::system_clock::to_time_t(now);
        std::tm tm_local{};
        localtime_s(&tm_local, &t_c);
        std::string stamp = std::format("{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}",
            tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
            tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);

        auto data_dir = std::filesystem::path(R"(F:\\CLionProjects\\naf_pre\\data\\volprims)");

        std::filesystem::path out_dir = data_dir / std::format("f{}_{}", rhi.GetFrameIndex(), stamp);
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);

        ExportViewResourceRaw(out_dir, RendererViewFrameExportResource::kDepth, "depth");
        ExportViewResourceRaw(out_dir, RendererViewFrameExportResource::kTransmittance, "transmittance");
        ExportViewResourceRaw(out_dir, RendererViewFrameExportResource::kVolumeSampleColor, "vol_sample_color");
        ExportViewResourceRaw(out_dir, RendererViewFrameExportResource::kVolumeSampleLinearDepth, "vol_sample_linear_depth");
        ExportViewResourceRaw(out_dir, RendererViewFrameExportResource::kVolumeDensity, "vol_density");
        ExportViewResourceRaw(out_dir, RendererViewFrameExportResource::kVolumeColor, "vol_color");
        ExportViewResourceRaw(out_dir, RendererViewFrameExportResource::kVolumeDirect, "vol_direct_lighting");
        ExportViewResourceRaw(out_dir, RendererViewFrameExportResource::kVolumeIndirect, "vol_indirect_lighting");

        MI_LOG(MIInfraLogType::kInfo, "Frame data exported to {}", out_dir.string());
        ops.should_export_result = false;
    }

    if (ops.should_export_baking_result) {
        ops.should_export_baking_result = false;
        auto dir = GetInfra().TranslateResPathToFilePath("baking_results/");
        if (!std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
        }
        auto date_time = GetCurrentDateTimeString();
        auto random_str = GenerateRandomString(6);
        auto out_dir = dir / (date_time + random_str);
        std::filesystem::create_directories(out_dir);

        ExportViewResourceRaw(out_dir, RendererViewFrameExportResource::kPathTracingFilm, "baked_radiance");

        if (baking_state_.baking_camera_index <= baking_state_.baking_camera_positions.size()) {
            auto& cam = baking_state_.baking_camera_positions[baking_state_.baking_camera_index];
            view_->camera_ = cam;
            MI_LOG(MIInfraLogType::kInfo, "Switched to baking camera index {}.", baking_state_.baking_camera_index);
        }
    }
    auto & io = ImGui::GetIO();
    if (ops.should_process_click_select) {
        ops.should_process_click_select = false;
        float mouse_x = io.MousePos.x;
        float mouse_y = io.MousePos.y;
        auto visibility_desc = view_->GetFrameExportResource(RendererViewFrameExportResource::kVisibility);
        if (visibility_desc.IsValid()) {
            auto rhi_visibility = visibility_desc.texture->GetRHI();
            auto rhi_fwd_depth = view_->forward_depth_->GetRHI();
            auto& queue = RHI::Get().GetGraphicsCommandQueue();
            auto readback_buffer_visibility = rhi.CreateBuffer(
                rhi_visibility->GetWidth() * rhi_visibility->GetHeight() * sizeof(uint32_t) * 4,
                RHIBufferUsageFlagBits::kReadback
            );
            auto readback_buffer_depth = rhi.CreateBuffer(
                rhi_fwd_depth->GetWidth() * rhi_fwd_depth->GetHeight() * sizeof(float),
                RHIBufferUsageFlagBits::kReadback
            );
            queue.MemoryBarrier();
            queue.TextureBarrier(rhi_visibility, RHITextureLayoutType::kTransferSrcOptimal,
                RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll,
                RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kRead);
            queue.TextureBarrier(rhi_fwd_depth, RHITextureLayoutType::kTransferSrcOptimal,
                RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll,
                RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kRead);
            queue.CopyTextureToBuffer(rhi_visibility, readback_buffer_visibility.Raw());
            queue.CopyTextureToBuffer(rhi_fwd_depth, readback_buffer_depth.Raw());
            visibility_desc.texture->Use(
                RHIPipelineStageFlagBits::kTransfer, RHIGPUAccessFlagBits::kTransferRead,
                RHITextureLayoutType::kTransferSrcOptimal
            );
            view_->forward_depth_->Use(
                RHIPipelineStageFlagBits::kTransfer, RHIGPUAccessFlagBits::kTransferRead,
                RHITextureLayoutType::kTransferSrcOptimal
            );
            queue.MemoryBarrier();
            queue.WaitForIdle("Readback Buffers");
            auto ptr = (glm::uvec4*)readback_buffer_visibility->Map();
            glm::uvec4 pixel = ptr[(int(mouse_y) * rhi_visibility->GetWidth() + int(mouse_x))];
            auto depth_ptr = (float*)readback_buffer_depth->Map();
            input_state_.last_click_forward_depth_ = depth_ptr[(int(mouse_y) * rhi_fwd_depth->GetWidth() + int(mouse_x))];
            readback_buffer_visibility->Unmap();
            readback_buffer_depth->Unmap();
            float uv_x = std::bit_cast<float>(pixel.z);
            float uv_y = std::bit_cast<float>(pixel.w);
            auto descriptor_rank = pixel.x >> 24;
            auto renderable_index = pixel.x & 0xFFFFFF;
            if (renderable_index == 0xFFFFFF) {
                renderable_index = UINT32_MAX;
            }
            auto primitive_index = pixel.y;
            glm::vec2 uv = {uv_x, uv_y};
            if (selection_state_.selected_renderable_index != renderable_index) {
                if (renderable_index == UINT32_MAX) {
                    arrow_mesh_x_instance_->SetVisible(false);
                    arrow_mesh_y_instance_->SetVisible(false);
                    arrow_mesh_z_instance_->SetVisible(false);
                    selection_state_.selected_deferred_renderable_index = UINT32_MAX;
                } else if (renderable_index != arrow_mesh_x_instance_->GetIndex()
                    && renderable_index != arrow_mesh_y_instance_->GetIndex()
                    && renderable_index != arrow_mesh_z_instance_->GetIndex()
                ) {
                    selection_state_.selected_deferred_renderable_index = renderable_index;
                    auto renderable = scene_->GetRenderableByIndex(renderable_index);
                    arrow_mesh_x_instance_->EditTransform().position = renderable->GetTransform().position;
                    arrow_mesh_x_instance_->SetVisible(true);
                    arrow_mesh_y_instance_->EditTransform().position = renderable->GetTransform().position;
                    arrow_mesh_y_instance_->SetVisible(true);
                    arrow_mesh_z_instance_->EditTransform().position = renderable->GetTransform().position;
                    arrow_mesh_z_instance_->SetVisible(true);
                }
            }
            selection_state_.selected_descriptor_rank = descriptor_rank;
            selection_state_.selected_renderable_index = renderable_index;
            selection_state_.selected_primitive_index = primitive_index;
            selection_state_.selected_uv = uv;
            if (selection_state_.selected_renderable_index != UINT32_MAX) {
                MI_LOG(MIInfraLogType::kInfo, "Selected Renderable {}, Primitive {}, Descriptor Rank {}, UV ({}, {})",
                    selection_state_.selected_renderable_index, selection_state_.selected_primitive_index, selection_state_.selected_descriptor_rank, selection_state_.selected_uv.x, selection_state_.selected_uv.y
                );
            }
        }
    }

    if (ops.should_reload_shaders) {
        RHI::Get().WaitForIdle();
        RDGShaderLibrary::Get().RecompileUpdatedCachedShaders();
        ops.should_reload_shaders = false;
        ops.did_reload_shaders = true;
    }
}

void ViewerApp::ProcessAxisDragging() {
    auto io = ImGui::GetIO();
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !io.WantCaptureMouse) {
        int axis = -1;
        if (selection_state_.selected_renderable_index == arrow_mesh_x_instance_->GetIndex()) axis = 0;
        else if (selection_state_.selected_renderable_index == arrow_mesh_y_instance_->GetIndex()) axis = 1;
        else if (selection_state_.selected_renderable_index == arrow_mesh_z_instance_->GetIndex()) axis = 2;
        if (axis != -1) {
            glm::vec2 mouse;
            mouse.x = io.MousePos.x;
            mouse.y = io.MousePos.y;
            if (!input_state_.dragging_ && !io.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // 在箭头上左键点下，此时开始拖拽
                input_state_.drag_mouse_start_pos_ = mouse;
                if (selection_state_.selected_deferred_renderable_index != UINT32_MAX) {
                    auto renderable = scene_->GetRenderableByIndex(selection_state_.selected_deferred_renderable_index);
                    input_state_.drag_start_obj_pos_ = renderable->GetTransform().position;
                }
                input_state_.dragging_ = true;
            }
            if (input_state_.dragging_) {
                glm::vec3 end_world_pos {};
                auto& camera = view_->camera_;
                glm::vec2 ndc2 = {
                    (input_state_.drag_mouse_start_pos_.x / (float)view_->film_width_) * 2.0f - 1.0f,
                    1.0f - (input_state_.drag_mouse_start_pos_.y / (float)view_->film_height_) * 2.0f
                };
                float linear_depth = camera.ReversedZDepthToLinearDepth(input_state_.last_click_forward_depth_);
                float aspect = float(view_->film_width_) / float(view_->film_height_);
                glm::vec3 start_world_pos = camera.RecoverWorldPositionNDC2(ndc2, linear_depth, aspect);
                glm::vec2 curr_ndc2 = {
                    (mouse.x / (float)view_->film_width_) * 2.0f - 1.0f,
                    1.0f - (mouse.y / (float)view_->film_height_) * 2.0f
                };
                glm::vec3 cursor_end_world_pos = camera.RecoverWorldPositionNDC2(curr_ndc2, linear_depth, aspect);
                glm::vec3 delta = cursor_end_world_pos - start_world_pos;
                glm::vec3 delta_projected = {};
                delta_projected[axis] = delta[axis];
                end_world_pos = input_state_.drag_start_obj_pos_ + delta_projected;
                if (selection_state_.selected_deferred_renderable_index != UINT32_MAX) {
                    auto renderable = scene_->GetRenderableByIndex(selection_state_.selected_deferred_renderable_index);
                    renderable->EditTransform().position = end_world_pos;
                    arrow_mesh_x_instance_->EditTransform().position = end_world_pos;
                    arrow_mesh_y_instance_->EditTransform().position = end_world_pos;
                    arrow_mesh_z_instance_->EditTransform().position = end_world_pos;
                }
            }
        } else {
            input_state_.dragging_ = false;
        }
    } else {
        input_state_.dragging_ = false;
    }
}

void ViewerApp::Run(std::unique_ptr<MIInfraInterface>&& infra, const MainLoopStartConfig& cfg) {

    auto & rhi = RHI::Get();

    float cpu_duration = 0.0f;
    std::vector<RDGTimePeriod> rdg_time_periods;
    RDGProfilingContextRef pending_profiling_context;

    std::future<void> previous_frame_future;
    TRef<RHISyncPoint> previous_frame_sync_point = rhi.CreateSyncPoint();

    bool first_frame = true;

    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        auto cpu_tp_start = std::chrono::steady_clock::now();

        FrameInternalDelayedOps ops {};

        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Start of frame operations
        for (auto e : next_frame_operations_) e();
        next_frame_operations_.clear();

        // Major UI and Input Handling
        HandleNavigationInput(cpu_duration);
        HandleKeyboardShortcuts(ops);

        // Combined UI: Console (left) + Rendering/Performance (right)
        {
            ImGui::SetNextWindowSize(ImVec2(1200, 700), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("UI", nullptr, ImGuiWindowFlags_NoCollapse)) {
                float full_w = ImGui::GetContentRegionAvail().x;
                float full_h = ImGui::GetContentRegionAvail().y;
                float spacing = ImGui::GetStyle().ItemSpacing.x;

                // Right column: max 550 by default.
                float right_w = std::min(550.0f, full_w * 0.7f);
                float left_w = std::max(350.0f, full_w - right_w - spacing);

                ImGui::BeginChild("UI_Left", ImVec2(left_w, 0), true);
                auto content_region = ImGui::GetContentRegionAvail();
                console_.DrawImGuiConsoleEmbedded({content_region.x, content_region.y});
                ImGui::EndChild();

                ImGui::SameLine(0.0f, spacing);

                ImGui::BeginChild("UI_Right", ImVec2(right_w, 0), true);
                HandleControlUILogic(ops, rdg_time_periods, cpu_duration);
                ImGui::EndChild();
            }
            ImGui::End();
        }

        // console_.DrawImGuiConsole(); // replaced by embedded console in combined UI window

        if (baking_state_.is_baking_mode) {
            baking_state_.baking_frame_index++;
            if (baking_state_.baking_frame_index == baking_state_.baking_max_num_frames) {
                baking_state_.baking_camera_index++;
                baking_state_.baking_frame_index = 0;
                ops.should_export_baking_result = true;
            }
            if (baking_state_.baking_camera_index >= baking_state_.baking_camera_positions.size()) {
                baking_state_.ClearBakingState();
                ops.should_export_baking_result = false;
                MI_LOG(MIInfraLogType::kInfo, "Baking complete.");
            }
        }

        ProcessClickSelect(ops);

        // Process pending ZMQ messages
        std::vector<PendingViewerFrameExportRequest> frame_export_requests;
        tonemapped_color_export_ = {};
        if (zmq_server_) {
            for (const auto& export_name : zmq_server_->PollEvents()) {
                ViewerFrameExportChannel channel;
                if (!TryParseViewerFrameExportChannel(export_name, channel)) {
                    MI_WARN("Unexpected unsupported export type '{}' reached render loop.", export_name.c_str());
                    continue;
                }
                frame_export_requests.push_back({export_name, channel});
            }
        }
        if (zmq_server_ && zmq_server_->ConsumeReloadShadersRequest()) {
            ops.should_reload_shaders = true;
        }
        bool any_export_requests_pending = false;

        // RDG Execution!
        auto & io = ImGui::GetIO();
        RDGProfilingContextRef current_profiling_context;
        {
            RenderGraphBuilder builder;
            bool should_render_scene = !suspended_ || one_frame_rendering_requested_;
            RenderFrame(builder, view_.get(), should_render_scene);
            // Clear flag
            one_frame_rendering_requested_ = false;

            // Mark export flags.
            if (ops.should_process_click_select && !io.WantCaptureMouse) {
                view_->forward_depth_->SetExport();
                if (auto desc = view_->GetFrameExportResource(RendererViewFrameExportResource::kVisibility); desc.IsValid()) {
                    desc.texture->SetExport();
                }
            }

            if (ops.should_export_result) {
                for (auto resource : {
                    RendererViewFrameExportResource::kDepth,
                    RendererViewFrameExportResource::kTransmittance,
                    RendererViewFrameExportResource::kVolumeSampleColor,
                    RendererViewFrameExportResource::kVolumeSampleLinearDepth,
                    RendererViewFrameExportResource::kVolumeDensity,
                    RendererViewFrameExportResource::kVolumeColor,
                    RendererViewFrameExportResource::kVolumeDirect,
                    RendererViewFrameExportResource::kVolumeIndirect
                }) {
                    if (auto desc = view_->GetFrameExportResource(resource); desc.IsValid()) {
                        desc.texture->SetExport();
                    }
                }
            }

            if (ops.should_export_baking_result) {
                view_->radiance_->SetExport();
            }

            PrepareSpecialFrameExports(*this, builder, frame_export_requests);

            for (const auto& request : frame_export_requests) {
                any_export_requests_pending |= MarkRequestedFrameExport(*this, request.channel);
            }

            std::string frame_name = "Frame " + std::to_string(GetFrameIndexForCurrentThread());
            auto graph = builder.Compile(frame_name);
            {
                graph->Execute(pool_.Raw());
            }

            current_profiling_context = graph->GetProfilingContext();
        }

        if (any_export_requests_pending) {
            // Just wait for this frame to finish and process requests.
            RHI::Get().WaitForIdle();
            // Export!
            if (!exported_render_results_.empty()) {
                MI_WARN("Seems that some exported render results are not consumed...");
                exported_render_results_.clear();
            }

            for (const auto& request : frame_export_requests) {
                if (!PackRequestedFrameExport(*this, request.name, request.channel)) {
                    MI_WARN("Failed to read back '{}' texture", request.name.c_str());
                }
            }
            // Reply to the waiting ZMQ client with the exported results.
            if (zmq_server_) {
                zmq_server_->ReplyExportedFrame();
            }
        }


        ProcessDelayedOps(ops);

        if (zmq_server_ && ops.did_reload_shaders) {
            zmq_server_->ReplyReloadShaders(true);
            ops.did_reload_shaders = false;
        }

        // 必须在 selection 更新后判定拖拽
        ProcessAxisDragging();

        if (rhi.GetFrameIndex() % 1000 == 0) {
            printf("[%llu] Pool memory: %.2f MB\n", rhi.GetFrameIndex(), pool_->GetTotalDeviceMemoryUsage() / 1024.0f / 1024.0f);
#ifndef NDEBUG
            printf("RefCounted object count: %u\n", GetRefCountedObjectCount());
            printf("RHI object count: %llu\n", RHIResource::GetLivingRHIResourceCount());
#endif
            printf("Device allocator allocated memory: %.2f MB\n", resource_allocator_->GetTotalAllocatedDeviceSize() / 1024.0f / 1024.0f);
            fflush(stdout);
        }

        if (first_frame) {
            first_frame = false;
        } else {
            if (previous_frame_future.valid()) {
                previous_frame_future.wait();
            }
            previous_frame_sync_point->Wait();
            previous_frame_sync_point->Reset();
        }

        // Time to recycle renderables and rendering resources marked for deletion from the previous frame!
        scene_->AdvanceFrameForDelayedDestruction();
        resource_allocator_->AdvanceFrameForDelayedDestruction();

        // Try resolve previous frame profiling results (non-blocking) after the previous frame has finished.
        // In case if not ready yet, keep last results.
        if (pending_profiling_context) {
            std::vector<RDGTimePeriod> resolved;
            if (pending_profiling_context->ResolveTimestampPeriods(resolved, rhi, RHI::RHITimestampQueryMode::kNonBlocking)) {
                rdg_time_periods = std::move(resolved);
            }
            pending_profiling_context.SafeRelease();
        }
        // Set current profiling context as pending for next frame.
        pending_profiling_context = current_profiling_context;

        previous_frame_future = rhi.AdvanceFrame(previous_frame_sync_point.Raw());
        fflush(stdout);
        auto cpu_tp_end = std::chrono::steady_clock::now();
        cpu_duration = std::chrono::duration<float>(cpu_tp_end - cpu_tp_start).count();
    }
}

void ViewerApp::EnqueueNextFrameOperations(std::function<void()> func) {
    // Delayed to the beginning of next frame
    next_frame_operations_.push_back(std::move(func));
}

std::vector<ViewerApp::ExportedRenderResult> ViewerApp::GetAndClearExportedFrameResults() {
    return std::move(exported_render_results_);
}

ViewerApp::ViewerStatus ViewerApp::GetStatus() {
    ViewerStatus s{};
    s.is_suspended = suspended_.load();
    auto &rhi = RHI::Get();
    s.frame_index = (uint32_t)rhi.GetFrameIndex();
    if (view_) {
        s.camera = view_->camera_;
    }
    return s;
}

std::vector<ViewerImGuiConsole::ConsoleLogEntry> ViewerApp::GetLatestUniqueLogs(size_t max_count) const {
    return console_.GetLatestUniqueLogs(max_count);
}

bool ViewerApp::LoadGLTFAbsolute(const std::filesystem::path& path, std::vector<uint32_t>* out_renderable_indices) {
    if (!scene_ || !resource_allocator_) return false;
    WaitForSceneMutation();
    FlushSceneDelayedDestruction();
    if (path.empty() || !std::filesystem::exists(path)) {
        MI_WARN("LoadGLTFAbsolute: file not found '{}'.", path.string());
        return false;
    }

    std::vector<TRef<Geometry>> geometries;
    std::vector<TRef<Material>> materials;
    std::vector<TRef<RenderableNode>> nodes;
    std::vector<TRef<StaticMeshInstance>> new_meshes;
    if (!GLTFLoader::LoadGLTF(
        path,
        *resource_allocator_,
        *scene_,
        renderable_node_registry_.Raw(),
        default_material_.Raw(),
        geometries,
        materials,
        new_meshes,
        &nodes)) {
        MI_WARN("LoadGLTFAbsolute: failed to load GLTF '{}'.", path.string());
        return false;
    }

    RegisterLoadedScene(path.filename().string(), nodes);
    auto & r = Renderer::Get();
    auto & rhi = RHI::Get();
    for (auto e : new_meshes) {
        if (e) {
            e->UpdateLights_Async(r.GetDeviceAllocator(), rhi.GetGraphicsCommandQueue());
            if (out_renderable_indices) out_renderable_indices->push_back(e->GetIndex());
        }
    }
    return true;
}

bool ViewerApp::LoadPLYAsGRFAbsolute(const std::filesystem::path& path, std::vector<uint32_t>& out_renderable_indices) {
    if (!scene_ || !resource_allocator_) return false;
    WaitForSceneMutation();
    FlushSceneDelayedDestruction();
    if (path.empty() || !std::filesystem::exists(path)) {
        MI_WARN("LoadPLYAbsolute: file not found '{}'.", path.string());
        return false;
    }

    TRef<GaussianRadianceField> grf;
    if (!GaussianRadianceFieldLoader::LoadPLY(path, *resource_allocator_, grf)) {
        MI_WARN("LoadPLYAbsolute: failed to load PLY '{}'.", path.string());
        return false;
    }
    grf->UpdateOnDevice(resource_allocator_.Raw());
    auto inst = GaussianRadianceFieldInstance::Create(scene_.get(), grf.Raw(), Transform::FromMatrix(glm::mat4(1.0f)));
    if (inst) {
        out_renderable_indices.push_back(inst->GetIndex());
        auto node = renderable_node_registry_->Create(path.filename().string());
        node->SetRenderable(inst.Raw());
        node->UpdateWorldTransform();
        RegisterLoadedScene(path.filename().string(), { node });
    }
    return inst.IsValid();
}

bool ViewerApp::RemoveRenderableNodeByIndex(uint32_t renderable_node_index) {
    if (!scene_) return false;
    mi_assert(false, "Not implemented yet.");
    return true;
}

bool ViewerApp::CleanAllRenderableNodes() {
    if (!scene_) return false;

    WaitForSceneMutation();
    FlushSceneDelayedDestruction();

    loaded_scenes_.clear();

    selection_state_ = {};
    if (arrow_mesh_x_instance_) arrow_mesh_x_instance_->SetVisible(false);
    if (arrow_mesh_y_instance_) arrow_mesh_y_instance_->SetVisible(false);
    if (arrow_mesh_z_instance_) arrow_mesh_z_instance_->SetVisible(false);

    FlushSceneDelayedDestruction();
    return true;
}

void Run3DViewer(std::unique_ptr<MIInfraInterface> &&infra, const MainLoopStartConfig &cfg) {
    ViewerApp app;

    app.Initialize(std::move(infra), cfg);
    app.LoadScene(cfg);

    app.Run(std::move(infra), cfg);

    RHI::Get().WaitForIdle();
    app.Destroy();
}

MI_NAMESPACE_END

