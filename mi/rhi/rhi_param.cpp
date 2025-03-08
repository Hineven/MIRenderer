/*
 * Created: 2025/3/5
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rhi/rhi_param.h"
MI_NAMESPACE_BEGIN

uint32_t RHIParamInfo::GetAlignment () const {
    if (type == RHIParamType::kBasic) {
        return RHIGetBasicParamAlignment(basic_type);
    }
    if (type == RHIParamType::kStruct) {
        return 16;
    }
    return 0;
}

MI_NAMESPACE_END