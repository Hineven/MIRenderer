/*
 * Created: 2024/9/11
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_INFRA_H
#define MI_INFRA_H

#include <queue>
#include <semaphore>
#include <fstream>

#include <core/infra.h>
#include <shared_mutex>
#include <span>
#include <map>

MI_NAMESPACE_BEGIN

constexpr uint32_t kMaxFIOThreads = 4;

// Simply uses OS FS as resource system
class MyBlobResource : public BlobResourceInterface {
public:

    const void *ReadBlobZeroCopy(size_t pos, size_t size) override;
    std::future<const void *> Async_ReadBlobZeroCopy(size_t pos, size_t size) override;

    void ReadBlob(size_t pos, size_t size, void *data) override;

    std::future<void> Async_ReadBlob(size_t pos, size_t size, void *data) override;

    size_t GetSize() override;

    void WriteBlob(size_t pos, size_t size, const void *data) override;

    std::future<void> Async_WriteBlob(size_t pos, size_t size, const void *data) override;

    ~MyBlobResource() override;

    friend class MyInfra;
protected:
    MyBlobResource(MyInfra * infra_, const std::filesystem::path &file_path, MIInfraResourceHintType hint) ;

    MyInfra * infra_ {nullptr};

    // Real file stream
    std::fstream file_;
    // Make sure this value is always consistent with the file size upon reading
    volatile size_t file_size_ {};
    // Visible to all threads, used to indicate that the file size is dirty and needs to be updated
    volatile bool file_size_dirty_ {};

    // Read/Write control.
    // Multiple read tasks can be executed concurrently, while only one write task can be executed at a time.
    std::atomic<uint32_t> num_active_r_tasks_ {0};
    std::atomic<uint32_t> num_active_w_tasks_ {0};

    // True if the file is being closed, ie, WriteTaskWaitAndAcquire(true) is called.
    std::atomic<bool> file_closing_ {false};

    std::shared_mutex rw_mutex_;

    // Try to acquire the read lock, if failed, wait until the write lock is released
    // Called by io tasks in fs threads
    bool ReadTaskWaitAndAcquire();
    // Release the read lock. Called by io tasks in fs threads
    void ReadTaskRelease();
    // Try to acquire the write lock, if failed, wait until all read locks are released
    // Called by io tasks in fs threads.
    // If close_request is true, the write task will close the file and is prioritized over other write tasks.
    // Return true if the write lock is acquired, false if the file is closed.
    bool WriteTaskWaitAndAcquire(bool close_request = true);
    // Release the write lock. Called by io tasks in fs threads
    // Called by io tasks in fs threads
    void WriteTaskRelease();


};

struct HLSLCompilerContext;

// A simple implementation for the infrastructure interface.
// Windows, Vulkan 1.3, NVIDIA
class MyInfra : public MIInfraInterface {
public:
    MyInfra(bool find_resource_directory = false, std::string resource_directory = "") ;

    std::filesystem::path GetResourceDirectory() override;

    std::filesystem::path GetTempDirectory() override;

    MIInfraLimits GetResourceLimits () override;

    void Init () override;
    void Shutdown () override;

    TRef<BlobResourceInterface>
    RIO_Open(const MIResourcePath &res_path, MIInfraResourceHintType hint, BlobResourceAccessFlags access) override;

    bool RIO_Exists(const MIResourcePath &res_path) override;

    bool RIO_Delete(const MIResourcePath &res_path) override;

    std::optional<std::unique_ptr<std::thread>>
    LaunchThread(ThreadPerformanceType perf_type, std::function<void()> thread_func) override;

    uint32_t GenerateSeed() override;

    float GetTimeSinceStart() override;

    void ProfileStart(const std::string &name) override;

    void ProfileEnd(const std::string &name) override;

    void AddProfileTime(const std::string &name, float time) override;

    std::vector<uint32_t> CompileHLSLToSPIRV (
            const wchar_t *  shader_path,
            std::string entry_point,
            // dxc style profile
            std::string target_profile,
            std::span<const char> hlsl_code,
            std::vector<std::string> defines,
            std::vector<std::string> options,
            std::string & error,
            std::wstring * out_compile_command = nullptr,
            uint64_t * out_hash = nullptr
    ) override;

    uint64_t GetShaderXXHashFromShaderResourcePath (
        const MIResourcePath & res_path,
        std::vector<std::string> defines,
        std::vector<std::string> options,
        bool & is_shader_valid
    ) override;

    void LogMessage(MIInfraLogType level, const std::string &message) override;

    void OnFrameBegin() override;

    void OnFrameRHISubmit() override;

    friend class MyBlobResource;
protected:

    void FIO_ThreadMain ();

    void KickOffFIOThreads();
    void StopAndBlockWaitFIOThreads();

    HLSLCompilerContext *  GetHLSLCompilerContextForThread(std::thread::id thread_id);
    void DestroyHLSLCompilerContexts ();

    std::filesystem::path TranslateResPathToFilePath (const MIResourcePath & res_path);

    // Configurations
    MIInfraLimits limits_;

    // Directories
    std::filesystem::path resource_directory_;
    std::filesystem::path temp_directory_;

    // FIO
    // The flag is used to stop the file io threads. Make it atomic to ensure mem visibility to all other threads
    // upon modification.
    std::atomic<bool> fio_stop_ {false};
    // The semaphore is incremented when a new task is added to the queue
    std::counting_semaphore<> fio_task_semaphore_ {0};
    // The mutex is used to protect the queue
    std::mutex fio_queue_mutex_;
    // The thread that processes the file io tasks
    std::unique_ptr<std::thread> fio_threads_[kMaxFIOThreads];
    // The queue of file io tasks
    std::queue<std::packaged_task<void()>> fio_tasks_;

    // Seeds
    std::atomic<uint32_t> seed_identifier_ {0};

    // Timing
    std::chrono::time_point<std::chrono::steady_clock> start_time_;

    // Compiler
    std::map<std::thread::id, HLSLCompilerContext *> hlsl_compiler_contexts_;
    std::mutex hlsl_compiler_contexts_mutex_; // Protect compiler contexts map (shader hot-reload multi-thread safety)
};

MI_NAMESPACE_END

#endif //MI_INFRA_H
