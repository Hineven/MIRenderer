/*
 * Created: 2024/9/11
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <iostream>
#include <string.h>
#include <infra_impl/infra.h>

#include "core/thr.h"


MI_NAMESPACE_BEGIN
#define RU "(Are you forgetting to release all BlobRes references before Infra destruction?) "

MyBlobResource::~MyBlobResource() noexcept {
    DoClose();
}

MyBlobResource::MyBlobResource(MyInfra * infra, const std::filesystem::path & file_path, [[maybe_unused]] MIInfraResourceHintType hint) {
    infra_ = infra;
    file_.open(file_path, std::ios::in | std::ios::out | std::ios::binary);
    if(!file_.good()) {
        char buffer[128];
        strerror_s(buffer, 128, errno);
        infra_->LogMessage(MIInfraLogType::kInfo, "Failed to open file: " + file_path.string()
            + ", err: " + buffer);
    } else {
        file_.seekg(0, std::ios::end);
        file_size_ = file_.tellg();
        file_.seekg(0);
    }
}

const void *MyBlobResource::ReadBlobZeroCopy([[maybe_unused]] size_t pos, [[maybe_unused]] size_t size) {
    return nullptr;
}

std::future<const void *> MyBlobResource::Async_ReadBlobZeroCopy([[maybe_unused]] size_t pos, [[maybe_unused]] size_t size) {
    return {};
}

void MyBlobResource::DoClose() {
    file_closing_ = true;
    rw_mutex_.lock();
    file_.close();
    rw_mutex_.unlock();
}

void MyBlobResource::DoReadBlob(size_t pos, size_t size, void *data) {
    if(!ReadTaskWaitAndAcquire()) return;
    file_.seekg(static_cast<std::streamoff>(pos));
    file_.read(reinterpret_cast<char *>(data), static_cast<std::streamsize>(size));
    ReadTaskRelease();
}

void MyBlobResource::DoWriteBlob(size_t pos, size_t size, const void *data) {
    if(!WriteTaskWaitAndAcquire(false)) return;
    file_.seekp(static_cast<std::streamoff>(pos));
    file_.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    file_size_dirty_ = true;
    WriteTaskRelease();
}

size_t MyBlobResource::DoGetSize() {
    if(!ReadTaskWaitAndAcquire()) return 0;
    if(file_size_dirty_) {
        file_.seekg(0, std::ios::end);
        file_size_ = file_.tellg();
        file_size_dirty_ = false;
    }
    auto size = file_size_;
    ReadTaskRelease();
    return size;
}

void MyBlobResource::ReadBlob(size_t pos, size_t size, void *data) {
    DoReadBlob(pos, size, data);
}

std::future<void> MyBlobResource::Async_ReadBlob(size_t pos, size_t size, void *data) {
    TRef self(this); // keep alive during async
    std::packaged_task<void()> task([self, pos, size, data]() {
        self->DoReadBlob(pos, size, data);
    });
    auto future = task.get_future();
    {
        std::lock_guard<std::mutex> lock(infra_->fio_queue_mutex_);
        if (!infra_->fio_stop_) {
            infra_->fio_tasks_.emplace(std::move(task));
        } else {
            // If FIO thread is stopping, do not enqueue new tasks.
            MI_LOG(MIInfraLogType::kWarning, RU "Attempted to read from blob resource while FIO thread is stopping.");
            return future;
        }
    }
    infra_->fio_task_semaphore_.release();
    return future;
}

void MyBlobResource::WriteBlob(size_t pos, size_t size, const void *data) {
    DoWriteBlob(pos, size, data);
}

std::future<void> MyBlobResource::Async_WriteBlob(size_t pos, size_t size, const void *data) {
    TRef self(this); // keep alive during async
    std::packaged_task<void()> task([self, pos, size, data]() {
        self->DoWriteBlob(pos, size, data);
    });
    auto future = task.get_future();
    {
        std::lock_guard<std::mutex> lock(infra_->fio_queue_mutex_);
        if (infra_->fio_stop_) {
            // If FIO thread is stopping, do not enqueue new tasks.
            MI_LOG(MIInfraLogType::kWarning, RU "Attempted to write to blob resource while FIO thread is stopping.");
            return future;
        }
        infra_->fio_tasks_.emplace(std::move(task));
    }
    infra_->fio_task_semaphore_.release();
    return future;
}

size_t MyBlobResource::GetSize() {
    return DoGetSize();
}

bool MyBlobResource::ReadTaskWaitAndAcquire() {
    if(file_closing_) return false;
    // Two threads can not read at the same time (because seekg() stuff)
    rw_mutex_.lock();
    num_active_r_tasks_++;
    return true;
}

void MyBlobResource::ReadTaskRelease() {
    num_active_r_tasks_--;
    // Two threads can not read at the same time (because seekg() stuff)
    rw_mutex_.unlock();
}

bool MyBlobResource::WriteTaskWaitAndAcquire(bool close_request) {
    if(close_request) {
        assert(!file_closing_);
        rw_mutex_.lock();
        return true;
    }
    if(file_closing_) return false;
    rw_mutex_.lock();
    num_active_w_tasks_++;
    return true;
}

void MyBlobResource::WriteTaskRelease() {
    num_active_w_tasks_--;
    rw_mutex_.unlock();
}

void MyInfra::FIO_ThreadMain () {
    if (GetCurrentThreadType() != ThreadType::kUnknown) {
        MI_LOG(MIInfraLogType::kError, "FIO Thread started on a thread already registered as a different type.");
        return ;
    }
    SetCurrentThreadType(ThreadType::kFIOThread);
    InitializePlatformBackgroundThreadContext_Worker();
    while(true) {
        fio_task_semaphore_.acquire();
        std::packaged_task<void()> task;
        {
            // Pop task from queue
            std::lock_guard<std::mutex> lock(fio_queue_mutex_);
            if(!fio_tasks_.empty()) {
                task = std::move(fio_tasks_.front());
                fio_tasks_.pop();
            } else {
                // Check for stop signal
                if(fio_stop_) break;
            }
        }
        // Process task
        if (task.valid()) task();
        // Task destruction.
    }
    DestroyPlatformBackgroundThreadContext_Worker();
    SetCurrentThreadType(ThreadType::kUnknown);
    MI_INFO("FIO Thread exited.");
}

void MyInfra::KickOffFIOThreads() {
    fio_stop_ = false;
    fio_thread_ = std::make_unique<std::thread>(&MyInfra::FIO_ThreadMain, this);
}

void MyInfra::StopAndBlockWaitFIOThreads() {
    {
        std::lock_guard<std::mutex> lock(fio_queue_mutex_);
        fio_stop_ = true;
        // Wake the FIO thread to let it drain remaining tasks and exit
        fio_task_semaphore_.release(1);
    }
    fio_thread_->join();
}

std::filesystem::path MyInfra::TranslateResPathToFilePath(const MIResourcePath &res_path) {
    return (resource_directory_ / res_path).generic_string();
}

TRef<BlobResourceInterface>
MyInfra::RIO_Open(const MIResourcePath &res_path, MIInfraResourceHintType hint, BlobResourceAccessFlags access) {
    auto file_path = TranslateResPathToFilePath(res_path);

    // Ensure single instance per resource path.
    std::lock_guard<std::mutex> lock(resource_cache_mutex_);
    {
        auto it = resource_cache_.find(res_path);
        if (it != resource_cache_.end()) {
            return it->second;
        }
    }

    if(access & BlobResourceAccessFlagBits::kWrite) {
        if(!std::filesystem::exists(file_path)) {
            // Create directory if not exists
            std::filesystem::create_directories(file_path.parent_path());
            // Try to create file if not exists
            std::ofstream file(file_path);
            if(!file.good()) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Failed to create file: {}, err: {}",
                       file_path.string(), errno);
                return nullptr;
            }
        }
    }
    auto * res = new MyBlobResource(this, file_path, hint);
    printf("Opening: %s\n", file_path.string().c_str());
    if (!res->file_.good()) {
        delete res;
        return nullptr;
    }

    auto [it, inserted] = resource_cache_.emplace(res_path, TRef<BlobResourceInterface>(res));

    return it->second;

}

bool MyInfra::RIO_Exists(const MIResourcePath &res_path) {
    return std::filesystem::exists(TranslateResPathToFilePath(res_path));
}

bool MyInfra::RIO_Delete(const MIResourcePath &res_path) {
    {
        std::lock_guard<std::mutex> lock(resource_cache_mutex_);
        resource_cache_.erase(res_path);
    }
    return std::filesystem::remove(TranslateResPathToFilePath(res_path));
}

MIInfraLimits MyInfra::GetResourceLimits() {
    MIInfraLimits limits;
    limits.max_high_performance_thread_count = static_cast<int>(std::thread::hardware_concurrency());
    limits.max_low_performance_thread_count = 0;
    return limits;
}
MI_NAMESPACE_END
