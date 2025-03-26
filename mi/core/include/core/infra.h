/*
 * Created: 2024/7/3
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_CORE_INFTRA_H
#define MIRENDERER_CORE_INFTRA_H

#include <future>
#include <filesystem>
#include <span>
#include <format>
#include "core/common.h"
#include "types.h"
#include "blobres.h"

MI_NAMESPACE_BEGIN

enum class MIInfraResourceHintType {
    // The resource is plain binary, no prior knowledge.
    kBlob,
    // The resource is a shader source, and the renderer will use it as a shader.
    kShaderSource,
    // The resource is a texture, and the renderer will use it as a texture.
    kTexture,
    // The resource is a buffer for geometry computation (vertex/index buffers, etc)
    kGeometryBuffer
};

enum class MIInfraLogType {
    kInfo,
    kWarning,
    kError
};

class ThreadRunnable;

// Use plain strings for resource paths (for now).
typedef std::string MIResourcePath;

struct MIInfraLimits {
    // The maximum memory usage allowed for the renderer. 0 for unlimited.
    size_t max_memory_usage {};
    // The maximum VRAM usage allowed for the renderer. 0 for unlimited.
    size_t max_vram_usage {};
    // The maximum number of threads allowed for the renderer. -1: number of logical cores.
    int max_high_performance_thread_count {-1};
    // The maximum number of low-performance threads allowed for the renderer. Default to 0.
    int max_low_performance_thread_count {};

    // Optional: the index of the GPU to use. -1 for any.
    int gpu_index {-1};
};

// Interface for external infrastructure.
// The entire renderer is built on top of this interface, and the renderer will not access the system directly.
// This must be implemented by the users, and provide its instance to launch the renderer.
// Some of the implementations should be asynchronous to avoid blocking the renderer.
class MIInfraInterface {
public:
    // Return the ranges of computation resources that the renderer is allowed to use
    // The renderer will not request resources exceeding these limits (if it's just working as intended).
    virtual MIInfraLimits GetResourceLimits () = 0;

    // Called from the render thread when initializing the infrastructure.
    // This function is executed prior to any other functions within the render thread.
    virtual void Init() = 0;
    // Called from the render thread while shutting down the infrastructure.
    // This function is executed after all other functions within the render thread.
    virtual void Shutdown() = 0;

    // Resource operations. (File ops)
    // MI will access the file system directly, but will use this interface to load/save files.
    // These operations should usually be done in dedicated threads for maximum performance

    // The renderer request a persistent blob resource to load from the infrastructure.
    // Thread safety: required
    virtual TRef<BlobResourceInterface> RIO_Open (const MIResourcePath & res_path, MIInfraResourceHintType hint, BlobResourceAccessFlags access = BlobResourceAccessFlagBits::kRead) = 0;

    // The renderer request the infrastructure to check if a resource exists.
    // @return true if the resource exists.
    // Thread safety: required
    virtual bool RIO_Exists (const MIResourcePath & res_path) = 0;

    // The renderer request the infrastructure to delete a resource.
    // @return true if the resource is successfully deleted.
    // Thread safety: required
    virtual bool RIO_Delete (const MIResourcePath & res_path) = 0;

    // The renderer request the infrastructure to launch and keep a thread.
    // @return true if the thread is successfully launched.
    // Thread safety: required
    virtual std::optional<std::unique_ptr<std::thread>> LaunchThread (ThreadPerformanceType perf_type, std::function<void()> thread_func) = 0;

    // The renderer request a random seed from the infrastructure.
    // This function will only be called a few times, so performance is not a concern.
    // @return a random seed.
    // Thread safety: required
    virtual uint32_t GenerateSeed () = 0;

    // Get the current time since the infrastructure was initialized in seconds.
    // Precise time gives better counting accuracy, but not required.
    virtual float GetTimeSinceStart () = 0;

    // Profiling interface
    // Start a profiling section.
    virtual void ProfileStart (const std::string & name) = 0;
    // End a profiling section.
    virtual void ProfileEnd (const std::string & name) = 0;

    // Add a time to a profile section.
    virtual void AddProfileTime (const std::string & name, float time) = 0;

    // Compile a HLSL shader to SPIR-V.
    // Blocks until the compilation is finished. (This should usually be done parallelly)
    // @return a temporary blob containing the SPIR-V binary.
    virtual std::vector<uint32_t> CompileHLSLToSPIRV (
            const wchar_t * shader_path,
            std::string entry_point,
            std::string target_profile,
            std::span<const char> hlsl_code,
            std::vector<std::string> options,
            std::string & error
    ) = 0;


    // Logging interface
    virtual void               LogMessage (MIInfraLogType level, const std::string & message) = 0;

    // Hooks
    // Called from the render thread when a new frame begins.
    inline virtual void               OnFrameBegin() {};
    // Called from the render thread when the render commands have been recorded and before RHI submission.
    inline virtual void               OnFrameRHISubmit () {};

    virtual ~MIInfraInterface() = default;
};

// Get the globally unique provided infrastructure instance for the renderer.
MIInfraInterface & GetInfra () ;
// Transferring the ownership of the infra to the renderer after external construction.
// Init() is called on the infrastructure by the render thread when it starts.
void TransferInfra (std::unique_ptr<MIInfraInterface> && infra) ;
// Destroy the infrastructure instance.
// This function is called from the render thread when the renderer is shutting down.
// GetInfra().Shutdown() is called prior to this function.
void DestroyInfra () ;

#define MI_LOG(level, fmt, ...) ::MI_NAMESPACE::GetInfra().LogMessage(level, std::format("[{0}:{1}] {2}", __FILE__, __LINE__, std::format(fmt, ##__VA_ARGS__)))

#ifndef NDEBUG
#define mi_assert(cond, fmt, ...) do{if (!(cond)) { MI_LOG(::MI_NAMESPACE::MIInfraLogType::kError, fmt, ##__VA_ARGS__); throw std::exception("assertion failure.");}}while(false)
#define mi_warning(cond, fmt, ...) do{if (!(cond)) { MI_LOG(::MI_NAMESPACE::MIInfraLogType::kWarning, fmt, ##__VA_ARGS__); }}while(false)
#else
#define mi_assert(cond, msg)
#define mi_warning(cond, msg)
#endif

template<typename T>
class DeleteOnInfra {
public:
    inline void operator()(T * ptr) const {
        ptr->~T();
        GetInfra().Free(ptr);
    }
};

MI_NAMESPACE_END

#endif //MIRENDERER_CORE_INFTRA_H
