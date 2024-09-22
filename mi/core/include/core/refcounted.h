/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_CORE_REFCOUNTED_H
#define MIRENDERER_CORE_REFCOUNTED_H

#include <atomic>
#include <cassert>
#include "core/common.h"
#include "core/meta.h"
#include "platform.h"

MI_NAMESPACE_BEGIN

/**
 * Base class implementing thread-safe reference counting.
 */
template<bool bThreadSafe = true>
class RefCounted
{
public:
    RefCounted() = default;
    virtual ~RefCounted() = default;

    RefCounted(const RefCounted& Rhs) = delete;
    RefCounted& operator=(const RefCounted& Rhs) = delete;

    inline uint32_t IncRef() const
    {
        return (uint32_t)atomic_fetch_add(&ref_count_, 1) + 1;
    }

    inline uint32_t DecRef() const
    {
        int ref_count = atomic_fetch_sub(&ref_count_, 1) - 1;
        if (ref_count == 0)
        {
            delete this;
        }

        return (uint32_t)ref_count;
    }

    uint32_t GetRefCount() const
    {
        return (uint32_t)ref_count_;
    }

private:
    mutable std::conditional_t<bThreadSafe, std::atomic<int>, int> ref_count_ {0};
};

template<typename T>
concept CReferenceCounted =
    !TIsTypeComplete<T>::value || (
    std::is_same_v<decltype(std::declval<T>().IncRef()), uint32_t> &&
    std::is_same_v<decltype(std::declval<T>().DecRef()), uint32_t>);

template<CReferenceCounted T>
class TRef;

template<typename ReferencedType, typename T>
concept CUpcastAvailableRefType =
TIsTemplateInstance<TRef, T>::value && std::is_base_of_v<ReferencedType, typename T::ReferencedType>;


/**
 * A smart pointer to an object which implements IncRef/DecRef.
 *
 * The codes below are copy-pasted from Unreal.
 */
template<CReferenceCounted ReferencedType>
class TRef
{
    typedef ReferencedType* ReferenceType;

public:

    FORCEINLINE TRef():
            ptr_(nullptr)
    { }

    TRef(ReferencedType* in_ptr, bool inc_ref = true) // NOLINT implicit conversion from raw pointers
    {
        ptr_ = in_ptr;
        if(ptr_ && inc_ref)
        {
            ptr_->IncRef();
        }
    }

    TRef(const TRef& Copy)
    {
        ptr_ = Copy.ptr_;
        if(ptr_)
        {
            ptr_->IncRef();
        }
    }

    template<CReferenceCounted CopyReferencedType>
    explicit TRef(const TRef<CopyReferencedType>& Copy)
    {
        ptr_ = static_cast<ReferencedType*>(Copy.Raw());
        if (ptr_)
        {
            ptr_->IncRef();
        }
    }

    // Silent upcast pointers
    template<CUpcastAvailableRefType<ReferencedType> AnotherRefType>
    TRef(const TRef<AnotherRefType>& Copy)
    {
        ptr_ = static_cast<ReferencedType*>(Copy.Raw());
        if (ptr_)
        {
            ptr_->IncRef();
        }
    }
    template<CUpcastAvailableRefType<ReferencedType> AnotherRefType>
    TRef(TRef<AnotherRefType>&& Move)
    {
        ptr_ = static_cast<ReferencedType*>(Move.Raw());
        Move.ptr_ = nullptr;
    }
    // Silent upcast assignment operators
    template<CUpcastAvailableRefType<ReferencedType> AnotherRefType>
    TRef& operator=(const TRef<AnotherRefType>& Copy)
    {
        ptr_ = static_cast<ReferencedType*>(Copy.Raw());
        if (ptr_)
        {
            ptr_->IncRef();
        }
    }
    template<CUpcastAvailableRefType<ReferencedType> AnotherRefType>
    TRef& operator=(TRef<AnotherRefType>&& Move)
    {
        ptr_ = static_cast<ReferencedType*>(Move.Raw());
        Move.ptr_ = nullptr;
    }

    FORCEINLINE TRef(TRef&& Move)  // NOLINT implicit move construction
    {
        ptr_ = Move.ptr_;
        Move.ptr_ = nullptr;
    }

    template<CReferenceCounted MoveReferencedType>
    explicit TRef(TRef<MoveReferencedType>&& Move)
    {
        ptr_ = static_cast<ReferencedType*>(Move.Raw());
        Move.ptr_ = nullptr;
    }

    ~TRef()
    {
        if(ptr_)
        {
            ptr_->DecRef();
        }
    }

    TRef& operator = (ReferencedType* InReference)
    {
        if (ptr_ != InReference)
        {
            // Call IncRef before DecRef, in case the new reference is the same as the old reference.
            ReferencedType* OldReference = ptr_;
            ptr_ = InReference;
            if (ptr_)
            {
                ptr_->IncRef();
            }
            if (OldReference)
            {
                OldReference->DecRef();
            }
        }
        return *this;
    }

    FORCEINLINE TRef& operator = (const TRef& InPtr) // NOLINT self-assignment is rare under such circumstances without hacks
    {
        return *this = InPtr.ptr_; // NOLINT
    }

    template<CReferenceCounted CopyReferencedType>
    FORCEINLINE TRef& operator=(const TRef<CopyReferencedType>& InPtr)
    {
        return *this = InPtr.Raw(); // NOLINT
    }

    TRef& operator = (TRef&& InPtr) // NOLINT self-assignment is rare under such circumstances without hacks
    {
        if (this != &InPtr)
        {
            ReferencedType* OldReference = ptr_;
            ptr_ = InPtr.ptr_;
            InPtr.ptr_ = nullptr;
            if(OldReference)
            {
                OldReference->DecRef();
            }
        }
        return *this;
    }

//    template<CReferenceCounted MoveReferencedType>
//    TRef& operator=(TRef<MoveReferencedType>&& InPtr)
//    {
//        // InPtr is a different type (or we would have called the other operator), so we need not test &InPtr != this
//        ReferencedType* OldReference = ptr_;
//        ptr_ = InPtr.ptr_;
//        InPtr.ptr_ = nullptr;
//        if (OldReference)
//        {
//            OldReference->DecRef();
//        }
//        return *this;
//    }

    FORCEINLINE ReferencedType* operator->() const
    {
        return ptr_;
    }

    FORCEINLINE operator ReferencedType () const // NOLINT implicit conversion
    {
        return ptr_;
    }

// Dangerous to use, commented.

//    FORCEINLINE ReferencedType** GetInitReference ()
//    {
//        *this = nullptr;
//        return &ptr_;
//    }

    FORCEINLINE ReferencedType* Raw () const
    {
        return ptr_;
    }

    FORCEINLINE friend bool IsValidRef(const TRef& InReference)
    {
        return InReference.ptr_ != nullptr;
    }

    [[nodiscard]] FORCEINLINE bool IsValid() const
    {
        return ptr_ != nullptr;
    }

    FORCEINLINE void SafeRelease()
    {
        *this = nullptr;
    }

    uint32_t GetRefCount()
    {
        uint32_t Result = 0;
        if (ptr_)
        {
            Result = ptr_->GetRefCount();
            // cannot use mi_assert cause inftra.h has included this file indirectly
            assert(Result > 0 && "Nah"); // you should never have a zero ref count if there is a live ref counted pointer (*this is live)
        }
        return Result;
    }

    FORCEINLINE void Swap(TRef& InPtr) // this does not change the reference count, and so is faster
    {
        ReferencedType* OldReference = ptr_;
        ptr_ = InPtr.ptr_;
        InPtr.ptr_ = OldReference;
    }


    FORCEINLINE operator bool() const // NOLINT implicit conversion
    {
        return IsValid();
    }

private:

    ReferencedType* ptr_;

    template <CReferenceCounted OtherType>
    friend class TRef;

public:
    FORCEINLINE bool operator==(const TRef& B) const
    {
        return Raw() == B.Raw();
    }

    FORCEINLINE bool operator==(ReferencedType* B) const
    {
        return Raw() == B;
    }
};

MI_NAMESPACE_END

#endif //MIRENDERER_CORE_REFCOUNTED_H
