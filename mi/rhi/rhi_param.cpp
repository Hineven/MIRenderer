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

void RHIParamStructInfo::InitializeLayoutHash() {
    uint32_t hash = 0;
    for (auto & e : members) {
        hash = CRC32(e.name.c_str(), e.name.size(), hash);
        hash = CRC32(&e.size, sizeof(e.size), hash);
        hash = CRC32(&e.offset, sizeof(e.offset), hash);
        hash = CRC32(&e.type, sizeof(e.type), hash);
        hash = CRC32(&e.basic_type, sizeof(e.basic_type), hash);
        if (e.struct_info) {
            hash = CRC32(&e.struct_info->layout_hash, sizeof(e.struct_info->layout_hash), hash);
        }
    }
    layout_hash = hash;
}


MI_NAMESPACE_END