/*
 * Created: 2024/9/19
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RHI_BINDLESSKEEPER_H
#define MI_RHI_BINDLESSKEEPER_H
#include "rhi/rhi.h"
#include "core/infra.h"
MI_NAMESPACE_BEGIN

class RHIBindlessSlotKeeperBase : public NonCopyable, public NonMovable {
protected:
    RHIBindlessSlotKeeperBase ();
public:
    ~RHIBindlessSlotKeeperBase();

    FORCEINLINE RHIBindlessResourceType GetType () const { return type_; }
    FORCEINLINE uint32_t GetSlot () const { return slot_; }

    FORCEINLINE uint32_t IncRef() {
        return ++ref_count_;
    }

    FORCEINLINE uint32_t DecRef() {
        ref_count_--;
        if (ref_count_ == 0) {
            // Self destruct
            GetInfra().Delete(this);
        }
        return ref_count_;
    }

    FORCEINLINE uint32_t GetRefCount() const {
        return ref_count_;
    }

    FORCEINLINE void CommitChanges (uint32_t offset = 0, int size = -1) const ;
protected:
    friend class RHIBindlessManager;

    RHIResource * Get_Impl (uint32_t offset) ;
    void Set_Impl (RHIResource * resource, uint32_t offset) ;
    void Commit_Impl (uint32_t offset) ;

    RHIBindlessResourceType type_;
    uint32_t slot_;
    uint32_t num_slots_;

    int ref_count_;
};

template<typename T>
class RHIBindlessSlotKeeper;

template<typename T>
using RHIBindlessSlotRef = TRef<RHIBindlessSlotKeeper<T>>;


template<typename T>
class RHIBindlessSlotKeeper : public RHIBindlessSlotKeeperBase {
public:
    // Get the resource handle from the slot(s)
    // @param offset (slot + offset) = real_slot. Can not access slots that are not claimed.
    T * Get (uint32_t offset = 0) {
        mi_assert(offset < num_slots_, "Offset exceeds the number of slots");
        return (T*) Get_Impl(offset);
    }
    void Set (T * resource, uint32_t offset = 0) {
        mi_assert(offset < num_slots_, "Offset exceeds the number of slots");
        Set_Impl((RHIResource*)resource, offset);
    }
    void SetAndCommit (T * resource, uint32_t offset = 0) {
        Set(resource, offset);
        Commit_Impl(offset);
    }
};

template<>
class RHIBindlessSlotKeeper<RHIBuffer> : public RHIBindlessSlotKeeperBase {
public:
    // Get the resource handle from the slot(s)
    // @param offset (slot + offset) = real_slot. Can not access slots that are not claimed.
    RHIBuffer * Get (uint32_t offset = 0) {
        mi_assert(offset < (int)num_slots_, "Offset exceeds the number of slots");
        return (RHIBuffer*) Get_Impl(offset);
    }
    void Set (RHIBuffer * resource, uint32_t offset = 0) ;
    void SetAndCommit (RHIBuffer * resource, uint32_t offset = 0) ;
};

template<>
class RHIBindlessSlotKeeper<RHIBufferSpan> : public RHIBindlessSlotKeeperBase {
public:
    // Get the resource handle from the slot(s)
    // @param offset (slot + offset) = real_slot.
    RHIBufferSpan Get (uint32_t offset = 0) ;
    void Set (RHIBufferSpan resource, uint32_t offset = 0) ;
    void SetAndCommit (RHIBufferSpan resource, uint32_t offset = 0) ;
};

MI_NAMESPACE_END
#endif //MI_RHI_BINDLESSKEEPER_H
