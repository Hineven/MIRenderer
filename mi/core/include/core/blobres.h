/*
 * Created: 2024/9/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_BLOBRES_H
#define MIRENDERER_BLOBRES_H

#include <future>
#include "core/common.h"
#include "core/refcounted.h"
#include "core/base.h"

MI_NAMESPACE_BEGIN

// Implement the blob resource interface to produce a functioning infra.
class BlobResourceInterface : public RefCounted<> {
public:
    // All the functions require thread-safe implementations
    // @return may return nullptr if the blob resource is not zero-copy readable.
    // Thread safe.
    virtual const void * ReadBlobZeroCopy (size_t pos, size_t size) = 0;
    // Thread safe.
    virtual std::future<const void*> Async_ReadBlobZeroCopy(size_t pos, size_t size) = 0;
    // Thread safe.
    virtual void   ReadBlob (size_t pos, size_t size, void * data) = 0;
    // Thread safe.
    virtual std::future<void> Async_ReadBlob(size_t pos, size_t size, void * data) = 0;
    // Thread safe.
    virtual size_t GetSize () = 0;
    // Thread safe.
    virtual void   WriteBlob (size_t pos, size_t size, const void * data) = 0;
    // Thread safe.
    virtual std::future<void> Async_WriteBlob(size_t pos, size_t size, const void * data) = 0;

protected:
    virtual ~BlobResourceInterface () = default; // prevent user deletion; allow base RefCounted to delete
    BlobResourceInterface () = default;
};

MI_NAMESPACE_END
#endif //MIRENDERER_BLOBRES_H
