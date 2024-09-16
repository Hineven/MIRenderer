/*
 * Created: 2024/9/11
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <infra_impl/infra.h>
#include <iostream>


MI_NAMESPACE_BEGIN

MyBlobResource::~MyBlobResource() noexcept {
    // Wait until all io tasks are finished
    WriteTaskWaitAndAcquire(true);
    file_.close();
}

MyBlobResource::MyBlobResource(MyInfra * infra, const std::filesystem::path & file_path, [[maybe_unused]] MIInfraResourceHintType hint) {
    infra_ = infra;
    file_.open(file_path, std::ios::in | std::ios::out | std::ios::binary);
    if(!file_.good()) {
        infra_->LogMessage(MIInfraLogType::kError, "Failed to open file: " + file_path.string());
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

void MyBlobResource::ReadBlob(size_t pos, size_t size, void *data) {
    ReadTaskWaitAndAcquire();
    file_.seekg(pos);
    file_.read(reinterpret_cast<char *>(data), size);
    ReadTaskRelease();
}

std::future<void> MyBlobResource::Async_ReadBlob(size_t pos, size_t size, void *data) {
    std::packaged_task<void()> task([this, pos, size, data]() {
        ReadBlob(pos, size, data);
    });
    // Create future (before the task can be executed and destructed)
    auto future = task.get_future();
    // Append to fio queue
    {
        std::lock_guard<std::mutex> lock(infra_->fio_queue_mutex_);
        infra_->fio_tasks_.emplace(std::move(task));
    }
    // Notify fio threads
    infra_->fio_task_semaphore_.release();
    // Return future
    return future;
}

void MyBlobResource::WriteBlob(size_t pos, size_t size, const void *data) {
    WriteTaskWaitAndAcquire(false);
    file_.seekp(pos);
    file_.write(reinterpret_cast<const char *>(data), size);
    file_size_dirty_ = true;
    WriteTaskRelease();
}

std::future<void> MyBlobResource::Async_WriteBlob(size_t pos, size_t size, const void *data) {
    std::packaged_task<void()> task([this, pos, size, data]() {
        WriteBlob(pos, size, data);
    });
    // Create future (before the task can be executed and destructed)
    auto future = task.get_future();
    // Append to fio queue
    {
        std::lock_guard<std::mutex> lock(infra_->fio_queue_mutex_);
        infra_->fio_tasks_.emplace(std::move(task));
    }
    // Notify fio threads
    infra_->fio_task_semaphore_.release();
    // Return future
    return future;
}

size_t MyBlobResource::GetSize() {
    ReadTaskWaitAndAcquire();
    if(file_size_dirty_) {
        file_.seekg(0, std::ios::end);
        file_size_ = file_.tellg();
        file_size_dirty_ = false;
    }
    return file_size_;
}

bool MyBlobResource::ReadTaskWaitAndAcquire() {
    if(file_closing_) return false;
    rw_mutex_.lock_shared();
    num_active_r_tasks_++;
    return true;
}

void MyBlobResource::ReadTaskRelease() {
    num_active_r_tasks_--;
    rw_mutex_.unlock_shared();
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
    while(!fio_stop_) {
        fio_task_semaphore_.acquire();
        // Incoming task or stop signal
        if(fio_stop_) {
            break;
        }
        std::packaged_task<void()> task;
        {
            // Pop task from queue
            std::lock_guard<std::mutex> lock(fio_queue_mutex_);
            if(!fio_tasks_.empty()) {
                task = std::move(fio_tasks_.front());
                fio_tasks_.pop();
            }
        }
        // Process task
        task();
        // Task destruction.
    }
}

void MyInfra::KickOffFIOThreads() {
    fio_stop_ = false;
    for(auto & fio_thread : fio_threads_) {
        fio_thread = std::make_unique<std::thread>(&MyInfra::FIO_ThreadMain, this);
    }
}

void MyInfra::StopAndBlockWaitFIOThreads() {
    fio_stop_ = true;
    // Notify all threads to check for stop signal
    fio_task_semaphore_.release(std::size(fio_threads_));
    for(auto & fio_thread : fio_threads_) {
        fio_thread->join();
    }
}

std::filesystem::path MyInfra::TranslateResPathToFilePath(const MIResourcePath &res_path) {
    return resource_directory_ / res_path;
}

TRef<BlobResourceInterface>
MyInfra::RIO_Open(const MIResourcePath &res_path, MIInfraResourceHintType hint, BlobResourceAccessFlags access) {
    auto file_path = TranslateResPathToFilePath(res_path);
    if(access & BlobResourceAccessFlagBits::kWrite) {
        if(!std::filesystem::exists(file_path)) {
            // Create directory if not exists
            std::filesystem::create_directories(file_path.parent_path());
            // Try to create file if not exists
            std::ofstream file(file_path);
            if(!file.good()) {
                MI_LOG(MIInfraLogType::kError,
                       "Failed to create file: {}, err: {}",
                       file_path.string().c_str(), errno);
                return nullptr;
            }
        }
    }
    auto * res = new MyBlobResource(this, file_path, hint);
    return res;
}

bool MyInfra::RIO_Exists(const MIResourcePath &res_path) {
    return std::filesystem::exists(TranslateResPathToFilePath(res_path));
}

bool MyInfra::RIO_Delete(const MIResourcePath &res_path) {
    return std::filesystem::remove(TranslateResPathToFilePath(res_path));
}

MIInfraLimits MyInfra::GetResourceLimits() {
    MIInfraLimits limits;
    limits.max_high_performance_thread_count = std::thread::hardware_concurrency();
    limits.max_low_performance_thread_count = 0;
    return limits;
}
MI_NAMESPACE_END