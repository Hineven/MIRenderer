/*
 * Created: 2025/3/8
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include "rdg/rdg_param.h"

MI_NAMESPACE_BEGIN

namespace details {
    bool zzFinalizeParams(std::vector<RDGShaderParamInfo> &params) {
        uint32_t curr_position = 0;
        uint32_t vertex_buffer_index = 0;
        uint32_t vertex_attribute_index = 0;
        uint32_t render_target_index = 0;
        std::map<std::string, size_t> name_to_offset;
        name_to_offset.clear();
        bool is_renderpass = false;
        for (auto & e : params) {
            // Ignore non-uniform buffer contents (shader resources, uniform buffer ref)
            if (e.type == RHIParamType::kBasic || e.type == RHIParamType::kStruct) {
                auto alignment = e.GetAlignment();
                curr_position = (curr_position + alignment - 1) & ~(alignment - 1);
                // HLSL buffer-row rule check: if the element lies on the 16-byte boundary, it should be aligned to 16 bytes
                if (e.type == RHIParamType::kBasic) {
                    auto param_size = RHIGetBasicParamSize(e.basic_type);
                    auto start_row = curr_position / 16;
                    auto end_row = (curr_position + param_size - 1) / 16;
                    if (start_row != end_row) {
                        curr_position = (curr_position + 16 - 1) & ~(16 - 1);
                    }
                }
                e.offset = curr_position;
                curr_position += e.size;
                auto it = name_to_offset.find(e.name);
                if (it != name_to_offset.end()) {
                    // FIXME there will be a compile error if i uncomment the LOG line. Why?
                    assert(false);
                    // MI_LOG(MIInfraLogType::kError, "Duplicate param name: {}", e.name);
                    return false;
                }
                name_to_offset[e.name] = e.offset;
            }
            if (e.type == RHIParamType::kVertexBuffer) {
                // Finalize vertex buffer index now
                e.cpp_extra.vertex_buffer_info->index = vertex_buffer_index++;
            }
            if (e.type == RHIParamType::kVertexAttribute) {
                // Finalize vertex attribute index now
                e.cpp_extra.vertex_attribute_info->attribute_index = vertex_attribute_index++;
            }
            if (e.type == RHIParamType::kRenderTarget) {
                // Finalize render target index now
                if (!IsDepthStencilPixelFormat(e.cpp_extra.render_targets_info->format))
                    e.cpp_extra.render_targets_info->target_index = render_target_index++;
                else e.cpp_extra.render_targets_info->target_index = UINT32_MAX; // stands for depth stencil
                is_renderpass = true;
            }
        }
        // Do some simple validation
        bool index_present = false;
        std::vector<bool> used_vertex_buffer;
        used_vertex_buffer.resize(vertex_buffer_index, false);
        for (auto & e : params) {
            if (is_renderpass && e.type != RHIParamType::kRenderTarget) {
                MI_LOG(MIInfraLogType::kError,
                    "Renderpass struct should only contain render target parameters. "
                    "Conventional shader parameter struct should not contain any render target parameters."
                    " (param name {}).", e.name
                );
                return false;
            }
            if (e.type == RHIParamType::kVertexAttribute) {
                if (e.cpp_extra.vertex_attribute_info->buffer_index >= vertex_buffer_index) {
                    MI_LOG(MIInfraLogType::kError,
                        "Invalid vertex attribute buffer index {} is out of range (vertex buffer count {}), attribute name {}).",
                        e.cpp_extra.vertex_attribute_info->buffer_index, vertex_buffer_index, e.name);
                    return false;
                } else {
                    used_vertex_buffer[e.cpp_extra.vertex_attribute_info->buffer_index] = true;
                }
            }
            if (e.type == RHIParamType::kIndexBuffer) {
                if (!index_present) index_present = true;
                else {
                    MI_LOG(MIInfraLogType::kError,
                        "More than one index buffer is provided. (provided {}).", e.name
                    );
                }
            }
        }
        for (int i = 0; i < (int)used_vertex_buffer.size(); i++) {
            if (!used_vertex_buffer[i]) {
                MI_LOG(MIInfraLogType::kWarning,
                    "Vertex buffer index {} is not used by any vertex attribute.", i
                );
            }
        }
        return true;
    }

    void zzFinalizeTopLevelParamsStructInfo(RDGShaderParamStructAndSizeInfo *info) {
        std::vector<RDGShaderParameterLocation> global_uniforms, storage_buffers, uniform_buffers,
            uavs, srvs, samplers, acceleration_structures, vertex_buffers, vertex_attributes,
            render_targets;
        RDGShaderParameterLocation index_buffer, dispatch_command, renderpass;
        std::function<void(const RDGShaderParamStructInfo *info, uint32_t base_cpp_offset, uint32_t base_offset)> Recurse
            = [&](const RDGShaderParamStructInfo *info, uint32_t base_cpp_offset, uint32_t base_offset) {
            for (auto & e : info->cpp_members) {
                uint32_t cpp_offset = e.cpp_offset + base_cpp_offset;
                uint32_t offset = e.offset + base_offset;
                if (e.type == RHIParamType::kStruct) {
                    Recurse(e.cpp_imported_struct_info.cpp_struct_info, cpp_offset, offset);
                } else if (e.type == RHIParamType::kBasic) {
                    global_uniforms.emplace_back(&e, cpp_offset, offset, e.size);
                } else if (e.type == RHIParamType::kStorageBuffer) {
                    storage_buffers.emplace_back(&e, cpp_offset, offset, 0);
                } else if (e.type == RHIParamType::kUniformBuffer) {
                    uniform_buffers.emplace_back(&e, cpp_offset, offset, 0);
                } else if (e.type == RHIParamType::kUAVTexture) {
                    uavs.emplace_back(&e, cpp_offset, offset, 0);
                } else if (e.type == RHIParamType::kSRVTexture) {
                    srvs.emplace_back(&e, cpp_offset, offset, 0);
                } else if (e.type == RHIParamType::kVertexBuffer) {
                    vertex_buffers.emplace_back(&e, cpp_offset, offset, 0);
                } else if (e.type == RHIParamType::kVertexAttribute) {
                    vertex_attributes.emplace_back(&e, cpp_offset, offset, 0);
                } else if (e.type == RHIParamType::kIndexBuffer) {
                    index_buffer = {&e, cpp_offset, offset, 0};
                } else if (e.type == RHIParamType::kRenderTarget) {
                    render_targets.emplace_back(&e, cpp_offset, offset, 0);
                } else if (e.type == RHIParamType::kDispatchCommand) {
                    dispatch_command = {&e, cpp_offset, offset, 0};
                } else if (e.type == RHIParamType::kRenderPass) {
                    renderpass = {&e, cpp_offset, offset, 0};
                } else if (e.type == RHIParamType::kAccelerationStructure){
                    acceleration_structures.emplace_back(&e, cpp_offset, offset, 0);
                } else if (e.type == RHIParamType::kSampler) {
                    samplers.emplace_back(&e, cpp_offset, offset, 0);
                } else {
                    assert(false && "Unimplemented");
                }
            }
        };
        Recurse(info, 0, 0);
        {
            if (!global_uniforms.empty()) {
                info->global_uniforms_ = std::span(new RDGShaderParameterLocation[global_uniforms.size()], global_uniforms.size());
                std::copy(global_uniforms.begin(), global_uniforms.end(), info->global_uniforms_.begin());
            } else info->global_uniforms_ = {};
            if (!storage_buffers.empty()) {
                info->storage_buffers_ = std::span(new RDGShaderParameterLocation[storage_buffers.size()], storage_buffers.size());
                std::copy(storage_buffers.begin(), storage_buffers.end(), info->storage_buffers_.begin());
            } else info->storage_buffers_ = {};
            if (!uniform_buffers.empty()) {
                info->uniform_buffers_ = std::span(new RDGShaderParameterLocation[uniform_buffers.size()], uniform_buffers.size());
                std::copy(uniform_buffers.begin(), uniform_buffers.end(), info->uniform_buffers_.begin());
            } else info->uniform_buffers_ = {};
            if (!uavs.empty()) {
                info->uavs_ = std::span(new RDGShaderParameterLocation[uavs.size()], uavs.size());
                std::copy(uavs.begin(), uavs.end(), info->uavs_.begin());
            } else info->uavs_ = {};
            if (!srvs.empty()) {
                info->srvs_ = std::span(new RDGShaderParameterLocation[srvs.size()], srvs.size());
                std::copy(srvs.begin(), srvs.end(), info->srvs_.begin());
            } else info->srvs_ = {};
            if (!samplers.empty()) {
                info->samplers_ = std::span(new RDGShaderParameterLocation[samplers.size()], samplers.size());
                std::copy(samplers.begin(), samplers.end(), info->samplers_.begin());
            } else info->samplers_ = {};
            if (!acceleration_structures.empty()) {
                info->acceleration_structures_ = std::span(new RDGShaderParameterLocation[acceleration_structures.size()], acceleration_structures.size());
                std::copy(acceleration_structures.begin(), acceleration_structures.end(), info->acceleration_structures_.begin());
            } else info->acceleration_structures_ = {};
            if (!vertex_buffers.empty()) {
                info->vertex_buffers_ = std::span(new RDGShaderParameterLocation[vertex_buffers.size()], vertex_buffers.size());
                std::copy(vertex_buffers.begin(), vertex_buffers.end(), info->vertex_buffers_.begin());
            } else info->vertex_buffers_ = {};
            if (!vertex_attributes.empty()) {
                info->vertex_attributes_ = std::span(new RDGShaderParameterLocation[vertex_attributes.size()], vertex_attributes.size());
                std::copy(vertex_attributes.begin(), vertex_attributes.end(), info->vertex_attributes_.begin());
            } else info->vertex_attributes_ = {};
            if (!render_targets.empty()) {
                info->render_targets_ = std::span(new RDGShaderParameterLocation[render_targets.size()], render_targets.size());
                std::copy(render_targets.begin(), render_targets.end(), info->render_targets_.begin());
            } else info->render_targets_ = {};
            if (index_buffer.info) {
                info->index_buffer_ = index_buffer;
            } else info->index_buffer_ = {};
            if (dispatch_command.info) {
                info->dispatch_command_ = dispatch_command;
            } else info->dispatch_command_ = {};
            if (renderpass.info) {
                info->renderpass_ = renderpass;
            } else info->renderpass_ = {};
        }
        // Some late validations
        {
            for (auto [i, e] : std::views::enumerate(info->vertex_attributes_)) {
                auto component_size = GetVertexAttributeFormatSize(e.info->cpp_extra.vertex_attribute_info->format);
                uint32_t end_offset = e.info->cpp_extra.vertex_attribute_info->offset + component_size;
                auto vbuf_index = e.info->cpp_extra.vertex_attribute_info->buffer_index;
                auto vbuf_stride = info->vertex_buffers_[vbuf_index].info->cpp_extra.vertex_buffer_info->stride;
                if (end_offset > vbuf_stride) {
                    MI_LOG(MIInfraLogType::kWarning,
                        "Vertex attribute {} have end offset {}, which exceeds the "
                        "stride of vertex buffer {}, which is {}.",
                        i, end_offset, vbuf_index, vbuf_stride
                    );
                }
            }
        }
    }

}

MI_NAMESPACE_END
