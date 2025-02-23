/*
 * Created: 2025/2/23
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef D3D12_RHI_INTERNAL_H
#define D3D12_RHI_INTERNAL_H

#include "d3d12.h"
#include "dxgi1_6.h
#include "D3D12MemAlloc.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN

class D3D12RHIInternal {
protected:

    HWND window_ = {};
    uint32_t window_width_ = 0;
    uint32_t window_height_ = 0;
    uint32_t max_frames_in_flight_ = 0;

    ID3D12Device *device_ = nullptr;
    IDXGIAdapter1 *adapter_ = nullptr;
    ID3D12Device5 *dxr_device_ = nullptr;
    ID3D12Device2 *mesh_device_ = nullptr;
    ID3D12CommandQueue *command_queue_ = nullptr;
    ID3D12GraphicsCommandList *command_list_ = nullptr;
    ID3D12GraphicsCommandList4 *dxr_command_list_ = nullptr;
    ID3D12GraphicsCommandList6 *mesh_command_list_ = nullptr;
    ID3D12CommandAllocator **command_allocators_ = nullptr;

    HANDLE fence_event_ = {};
    uint32_t fence_index_ = 0;
    ID3D12Fence **fences_ = nullptr;
    uint64_t *fence_values_ = nullptr;

    IDXGISwapChain3 *swap_chain_ = nullptr;
    D3D12MA::Allocator *mem_allocator_ = nullptr;
    ID3D12CommandSignature *dispatch_signature_ = nullptr;
    ID3D12CommandSignature *multi_draw_signature_ = nullptr;
    ID3D12CommandSignature *multi_draw_indexed_signature_ = nullptr;
    ID3D12CommandSignature *dispatch_rays_signature_ = nullptr;
    ID3D12CommandSignature *draw_mesh_signature_ = nullptr;
    std::vector<D3D12_RESOURCE_BARRIER> resource_barriers_;
    ID3D12Resource **back_buffers_ = nullptr;
    D3D12MA::Allocation **back_buffer_allocations_ = nullptr;
    DXGI_FORMAT back_buffer_format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_COLOR_SPACE_TYPE color_space_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    uint32_t *back_buffer_rtvs_ = nullptr;
    bool is_interop_ = false;

    void Initialize ();
};

MI_NAMESPACE_END
#endif //D3D12_RHI_INTERNAL_H
