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

void RHIParamStructInfo::InitializeLayoutHash () {
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

uint32_t RHIParamStructInfo::ComputeSize() const {
    uint32_t current_position = 0;
    for (auto & e : members) {
        auto alignment = e.GetAlignment();
        uint32_t next_position = (current_position + alignment - 1) & ~(alignment - 1);
        bool crossing_border = (next_position / 16) != (current_position / 16);
        // HLSL buffer-row rule check: if the element lies on the 16-byte boundary, it should be aligned to 16 bytes
        if (crossing_border) alignment = 16;
        current_position = (current_position + alignment - 1) & ~(alignment - 1);
        current_position += e.size;
    }
    return current_position;
}


MI_NAMESPACE_END