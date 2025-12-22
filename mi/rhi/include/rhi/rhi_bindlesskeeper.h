/*
 * Created: 2024/9/19
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RHI_BINDLESSKEEPER_H
#define MI_RHI_BINDLESSKEEPER_H
#include "core/infra.h"
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_desc.h"
MI_NAMESPACE_BEGIN

class RHIBindlessSlotKeeperBase : public RefCounted<false>, public NonMovable {
protected:
    RHIBindlessSlotKeeperBase ();
public:
    ~RHIBindlessSlotKeeperBase();

    FORCEINLINE RHIBindlessResourceType GetType () const { return type_; }
    FORCEINLINE uint32_t GetSlot () const { return slot_; }

    // Update the slot with current set resource
    // This is costly. Better batch commits and call the bindless manager manually if you
    // have many slots to update.
    void Commit () const ;
protected:
    friend class RHIBindlessManager;

    RHIResource * Get_Impl () ;
    void Set_Impl (RHIResource * resource) ;
    void Commit_Impl () ;

    RHIBindlessResourceType type_;
    uint32_t slot_;
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
    FORCEINLINE T * Get () {
        return (T*) Get_Impl();
    }
    FORCEINLINE void Set (T * resource) {
        Set_Impl((RHIResource*)resource);
    }
    FORCEINLINE void SetAndCommit (T * resource) {
        Set(resource);
        Commit_Impl();
    }
};

template<>
class RHIBindlessSlotKeeper<RHIBuffer> : public RHIBindlessSlotKeeperBase {
public:
    // Get the resource handle from the slot(s)
    // @param offset (slot + offset) = real_slot. Can not access slots that are not claimed.
    RHIBuffer * Get () {
        return (RHIBuffer*) Get_Impl();
    }
    void Set (RHIBuffer * resource) ;
    void SetAndCommit (RHIBuffer * resource) ;
};

MI_NAMESPACE_END
#endif //MI_RHI_BINDLESSKEEPER_H
