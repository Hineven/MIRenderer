/*
 * Created: 2025/2/23
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "rhi/d3d12/d3d12_rhi_internal.h"

MI_NAMESPACE_BEGIN

void D3D12RHIInternal::Initialize () {
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return;
    }
    struct DXGIFactoryReleaser {
        IDXGIFactory1 *factory;
        ~DXGIFactoryReleaser() {
            factory->Release();
        }
    } factory_releaser = { factory };


}

MI_NAMESPACE_END
