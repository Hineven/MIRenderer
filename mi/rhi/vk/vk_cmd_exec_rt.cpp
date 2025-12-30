/*
 * Created: 2025/7/5
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <ranges>
#include "vk_rhi.h"
#include "vk_cmd_exec.h"
#include "vk_buffer.h"
#include "vk_pipeline.h"
#include "vk_conversion.h"
#include "vk_as.h"

MI_NAMESPACE_BEGIN

void VulkanCommandExecutor::RHIBuildAccelerationStructure(RHICommandQueueBase *cmd, RHICommandBuildAccelerationStructure *build_acceleration_structure) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & cmdb = state.cmd;

    const auto& build_info = build_acceleration_structure->build_info_;
    auto scratch_buffer = static_cast<VulkanBuffer*>(build_acceleration_structure->scratch_buffer_.buffer);

    // Convert RHI build info to Vulkan build info
    vk::AccelerationStructureBuildGeometryInfoKHR vk_build_info;
    vk_build_info.setType(build_info.type == RHIAccelerationStructureType::kBottomLevel ?
                          vk::AccelerationStructureTypeKHR::eBottomLevel :
                          vk::AccelerationStructureTypeKHR::eTopLevel);

    // Convert build flags
    vk::BuildAccelerationStructureFlagsKHR vk_flags{};
    if (build_info.flags & RHIAccelerationStructureBuildFlagBits::kAllowUpdate) {
        vk_flags |= vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate;
    }
    if (build_info.flags & RHIAccelerationStructureBuildFlagBits::kAllowCompaction) {
        vk_flags |= vk::BuildAccelerationStructureFlagBitsKHR::eAllowCompaction;
    }
    if (build_info.flags & RHIAccelerationStructureBuildFlagBits::kPreferFastTrace) {
        vk_flags |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
    }
    if (build_info.flags & RHIAccelerationStructureBuildFlagBits::kPreferFastBuild) {
        vk_flags |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
    }
    if (build_info.flags & RHIAccelerationStructureBuildFlagBits::kLowMemory) {
        vk_flags |= vk::BuildAccelerationStructureFlagBitsKHR::eLowMemory;
    }
    vk_build_info.setFlags(vk_flags);

    vk_build_info.setMode(build_info.mode == RHIAccelerationStructureBuildMode::kBuild ?
                          vk::BuildAccelerationStructureModeKHR::eBuild :
                          vk::BuildAccelerationStructureModeKHR::eUpdate);

    if (build_info.src_acceleration_structure) {
        auto src_as = static_cast<VulkanAccelerationStructure*>(build_info.src_acceleration_structure);
        vk_build_info.setSrcAccelerationStructure(src_as->GetAccelerationStructure());
    }

    if (build_info.dst_acceleration_structure) {
        auto dst_as = static_cast<VulkanAccelerationStructure*>(build_info.dst_acceleration_structure);
        vk_build_info.setDstAccelerationStructure(dst_as->GetAccelerationStructure());
    }

    // Handle geometry data based on AS type
    vk::AccelerationStructureGeometryKHR * geometries;
    vk::AccelerationStructureBuildRangeInfoKHR * range_infos;

    if (build_info.type == RHIAccelerationStructureType::kBottomLevel) {
        // BLAS - handle geometry data
        geometries =
            state.Allocate<vk::AccelerationStructureGeometryKHR[]>(build_info.geometries.size());
        range_infos =
            state.Allocate<vk::AccelerationStructureBuildRangeInfoKHR[]>(build_info.geometries.size());

        for (const auto [i, geometry] : std::views::enumerate(build_info.geometries)) {
            vk::AccelerationStructureGeometryKHR vk_geometry;
            vk_geometry.setGeometryType(geometry.type == RHIASGeometryType::kTriangles ?
                                        vk::GeometryTypeKHR::eTriangles :
                                        vk::GeometryTypeKHR::eAabbs);

            vk::GeometryFlagsKHR geom_flags{};
            if (geometry.flags & RHIASGeometryFlagBits::kOpaque) {
                geom_flags |= vk::GeometryFlagBitsKHR::eOpaque;
            }
            if (geometry.flags & RHIASGeometryFlagBits::kNoDuplicateAnyHitInvocation) {
                geom_flags |= vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation;
            }
            vk_geometry.setFlags(geom_flags);

            if (geometry.type == RHIASGeometryType::kTriangles) {
                const auto& triangles = geometry.triangles;
                auto vertex_buffer = static_cast<VulkanBuffer*>(triangles.vertex_data.buffer);

                vk::AccelerationStructureGeometryTrianglesDataKHR triangle_data;
                triangle_data.setVertexData(vertex_buffer->GetDeviceAddress() + triangles.vertex_data.offset);
                triangle_data.setVertexStride(triangles.vertex_stride);
                assert(triangles.vertex_format == RHIVertexAttributeFormatType::k3xFp32 &&
                       "Only float3 vertex format is supported for triangles.");
                triangle_data.setVertexFormat(vk::Format::eR32G32B32Sfloat); // Only support float3
                triangle_data.setMaxVertex(triangles.vertex_count - 1);

                if (triangles.index_data.buffer) {
                    auto index_buffer = static_cast<VulkanBuffer*>(triangles.index_data.buffer);
                    triangle_data.setIndexData(index_buffer->GetDeviceAddress() + triangles.index_data.offset);
                    triangle_data.setIndexType(GetVulkanIndexType(triangles.index_type));
                }

                if (triangles.transform_data.buffer) {
                    auto transform_buffer = static_cast<VulkanBuffer*>(triangles.transform_data.buffer);
                    triangle_data.setTransformData(transform_buffer->GetDeviceAddress() + triangles.transform_data.offset);
                }

                vk_geometry.geometry.setTriangles(triangle_data);

                vk::AccelerationStructureBuildRangeInfoKHR range_info;
                range_info.setPrimitiveCount(triangles.index_count > 0 ? triangles.index_count / 3 : triangles.vertex_count / 3);
                range_info.setPrimitiveOffset(0);
                range_info.setFirstVertex(0);
                range_info.setTransformOffset(0);
                range_infos[i] = range_info;
            } else {
                const auto& aabbs = geometry.aabbs;
                auto aabb_buffer = static_cast<VulkanBuffer*>(aabbs.aabb_data.buffer);

                vk::AccelerationStructureGeometryAabbsDataKHR aabb_data;
                aabb_data.setData(aabb_buffer->GetDeviceAddress() + aabbs.aabb_data.offset);
                aabb_data.setStride(aabbs.aabb_stride);

                vk_geometry.geometry.setAabbs(aabb_data);

                vk::AccelerationStructureBuildRangeInfoKHR range_info;
                range_info.setPrimitiveCount(aabbs.aabb_count);
                range_info.setPrimitiveOffset(0);
                range_info.setFirstVertex(0);
                range_info.setTransformOffset(0);
                range_infos[i] = range_info;
            }

            geometries[i] = vk_geometry;
        }
    } else {
        // TLAS - handle instance data
        auto instance_buffer = static_cast<VulkanBuffer*>(build_info.instance_data.buffer);

        vk::AccelerationStructureGeometryKHR vk_geometry;
        vk_geometry.setGeometryType(vk::GeometryTypeKHR::eInstances);

        vk::AccelerationStructureGeometryInstancesDataKHR instance_data {};
        instance_data.setArrayOfPointers(false);
        if (instance_buffer) {
            instance_data.setData(instance_buffer->GetDeviceAddress() + build_info.instance_data.offset);
        }

        vk_geometry.geometry.setInstances(instance_data);
        geometries = state.Allocate<vk::AccelerationStructureGeometryKHR[]>(1);
        geometries[0] = vk_geometry;

        vk::AccelerationStructureBuildRangeInfoKHR range_info;
        range_info.setPrimitiveCount(build_info.instance_count);
        range_info.setPrimitiveOffset(0);
        range_info.setFirstVertex(0);
        range_info.setTransformOffset(0);
        range_infos = state.Allocate<vk::AccelerationStructureBuildRangeInfoKHR[]>(1);
        range_infos[0] = range_info;
    }

    vk_build_info.setPGeometries(geometries);
    vk_build_info.setGeometryCount(
        build_info.type == RHIAccelerationStructureType::kBottomLevel ? (uint32_t)build_info.geometries.size() : 1
    );

    // Set scratch buffer
    vk_build_info.setScratchData(scratch_buffer->GetDeviceAddress() + build_acceleration_structure->scratch_buffer_.offset);

    // Build the acceleration structure
    // TODO 25.12.30: this function make NVIDIA driver comsume about 260KB more memory per call, need to investigate later.
    // current workaround is to reduce the number of calls by checking for dirty transforms.
    cmdb.buildAccelerationStructuresKHR(1, &vk_build_info, &range_infos);
}

void VulkanCommandExecutor::RHIBindRayTracingPipeline(RHICommandQueueBase *cmd, RHICommandBindRayTracingPipeline *bind_ray_tracing_pipeline) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & cmdb = state.cmd;
    auto & point = state.points[(uint32_t)RHIBindPointType::kRayTracing];

    assert(bind_ray_tracing_pipeline->pipeline_->GetType() == RHIPipelineType::kRayTracing);
    auto pipeline = static_cast<VulkanRayTracingPipeline*>(bind_ray_tracing_pipeline->pipeline_);
    if(point.bound_pipeline != pipeline) {
        point.bound_pipeline_dirty = true;
        point.bound_private_descriptor_set = nullptr;
        point.bound_pipeline = pipeline;
    }
}

void VulkanCommandExecutor::RHIBindShaderBindingTable(RHICommandQueueBase *cmd, RHICommandBindShaderBindingTable *bind_shader_binding_table) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & ray_tracing_bind_point = state.points[(uint32_t)RHIBindPointType::kRayTracing];

    // Get the currently bound ray tracing pipeline to obtain SBT strides
    assert(ray_tracing_bind_point.bound_pipeline &&
           ray_tracing_bind_point.bound_pipeline->GetType() == RHIPipelineType::kRayTracing);
    auto pipeline = static_cast<VulkanRayTracingPipeline*>(ray_tracing_bind_point.bound_pipeline);

    // Convert RHI buffer spans to Vulkan shader binding table regions
    vk::StridedDeviceAddressRegionKHR raygen_sbt{};
    vk::StridedDeviceAddressRegionKHR miss_sbt{};
    vk::StridedDeviceAddressRegionKHR hit_sbt{};
    vk::StridedDeviceAddressRegionKHR callable_sbt{};

    if (bind_shader_binding_table->raygen_sbt_.buffer) {
        auto raygen_buffer = static_cast<VulkanBuffer*>(bind_shader_binding_table->raygen_sbt_.buffer);
        raygen_sbt.setDeviceAddress(raygen_buffer->GetDeviceAddress() + bind_shader_binding_table->raygen_sbt_.offset);
        raygen_sbt.setSize(bind_shader_binding_table->raygen_sbt_.size);
        raygen_sbt.setStride(pipeline->GetRaygenSBTStride()); // Get stride from pipeline
    }

    if (bind_shader_binding_table->miss_sbt_.buffer) {
        auto miss_buffer = static_cast<VulkanBuffer*>(bind_shader_binding_table->miss_sbt_.buffer);
        miss_sbt.setDeviceAddress(miss_buffer->GetDeviceAddress() + bind_shader_binding_table->miss_sbt_.offset);
        miss_sbt.setSize(bind_shader_binding_table->miss_sbt_.size);
        miss_sbt.setStride(pipeline->GetMissSBTStride()); // Get stride from pipeline
    }

    if (bind_shader_binding_table->hit_sbt_.buffer) {
        auto hit_buffer = static_cast<VulkanBuffer*>(bind_shader_binding_table->hit_sbt_.buffer);
        hit_sbt.setDeviceAddress(hit_buffer->GetDeviceAddress() + bind_shader_binding_table->hit_sbt_.offset);
        hit_sbt.setSize(bind_shader_binding_table->hit_sbt_.size);
        hit_sbt.setStride(pipeline->GetHitSBTStride()); // Get stride from pipeline
    }

    if (bind_shader_binding_table->callable_sbt_.buffer) {
        auto callable_buffer = static_cast<VulkanBuffer*>(bind_shader_binding_table->callable_sbt_.buffer);
        callable_sbt.setDeviceAddress(callable_buffer->GetDeviceAddress() + bind_shader_binding_table->callable_sbt_.offset);
        callable_sbt.setSize(bind_shader_binding_table->callable_sbt_.size);
        callable_sbt.setStride(pipeline->GetCallableSBTStride()); // Get stride from pipeline
    }

    // Store SBT regions in state for later use in dispatch rays
    state.raygen_sbt = raygen_sbt;
    state.miss_sbt = miss_sbt;
    state.hit_sbt = hit_sbt;
    state.callable_sbt = callable_sbt;
}

void VulkanCommandExecutor::RHIDispatchRays(RHICommandQueueBase *cmd, RHICommandDispatchRays *dispatch_rays) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & cmdb = state.cmd;

    // Flush ray tracing bind point state before dispatching
    FlushBindPointState(cmd, RHIBindPointType::kRayTracing, vk::ShaderStageFlagBits::eRaygenKHR |
                                                            vk::ShaderStageFlagBits::eMissKHR |
                                                            vk::ShaderStageFlagBits::eClosestHitKHR |
                                                            vk::ShaderStageFlagBits::eAnyHitKHR |
                                                            vk::ShaderStageFlagBits::eIntersectionKHR |
                                                            vk::ShaderStageFlagBits::eCallableKHR);

    // Use SBT regions from state (set by RHIBindShaderBindingTable)
    cmdb.traceRaysKHR(state.raygen_sbt, state.miss_sbt, state.hit_sbt, state.callable_sbt,
                      dispatch_rays->width_, dispatch_rays->height_, dispatch_rays->depth_);
}

void VulkanCommandExecutor::RHIDispatchRaysIndirect(RHICommandQueueBase *cmd, RHICommandDispatchRaysIndirect *dispatch_rays_indirect) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & cmdb = state.cmd;

    // Flush ray tracing bind point state before dispatching
    FlushBindPointState(cmd, RHIBindPointType::kRayTracing, vk::ShaderStageFlagBits::eRaygenKHR |
                                                            vk::ShaderStageFlagBits::eMissKHR |
                                                            vk::ShaderStageFlagBits::eClosestHitKHR |
                                                            vk::ShaderStageFlagBits::eAnyHitKHR |
                                                            vk::ShaderStageFlagBits::eIntersectionKHR |
                                                            vk::ShaderStageFlagBits::eCallableKHR);

    auto indirect_buffer = static_cast<VulkanBuffer*>(dispatch_rays_indirect->indirect_buffer_.buffer);
    vk::DeviceAddress indirect_device_address = indirect_buffer->GetDeviceAddress() + dispatch_rays_indirect->indirect_buffer_.offset;

    // Use SBT regions from state (set by RHIBindShaderBindingTable)
    auto raygen_buffer = static_cast<VulkanBuffer*>(dispatch_rays_indirect->raygen_.buffer);
    auto raygen_device_address = raygen_buffer->GetDeviceAddress() + dispatch_rays_indirect->raygen_.offset;
    auto raygen_sbt = vk::StridedDeviceAddressRegionKHR{
        raygen_device_address,
        dispatch_rays_indirect->raygen_.size,
        dispatch_rays_indirect->raygen_.size
    };
    auto miss_buffer = static_cast<VulkanBuffer*>(dispatch_rays_indirect->miss_.buffer);
    auto miss_device_address = miss_buffer->GetDeviceAddress() + dispatch_rays_indirect->miss_.offset;
    auto miss_sbt = vk::StridedDeviceAddressRegionKHR{
        miss_device_address,
        dispatch_rays_indirect->miss_stride_,
        dispatch_rays_indirect->miss_.size
    };
    auto hit_buffer = static_cast<VulkanBuffer*>(dispatch_rays_indirect->hit_.buffer);
    auto hit_device_address = hit_buffer->GetDeviceAddress() + dispatch_rays_indirect->hit_.offset;
    auto hit_sbt = vk::StridedDeviceAddressRegionKHR{
        hit_device_address,
        dispatch_rays_indirect->hit_stride_,
        dispatch_rays_indirect->hit_.size
    };
    cmdb.traceRaysIndirectKHR(
        raygen_sbt,
        miss_sbt,
        hit_sbt,
        {},
        indirect_device_address
    );
}

void VulkanCommandExecutor::RHIDispatchRaysIndirect2(RHICommandQueueBase *cmd, RHICommandDispatchRaysIndirect2 *dispatch_rays_indirect) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & cmdb = state.cmd;

    // Flush ray tracing bind point state before dispatching
    FlushBindPointState(cmd, RHIBindPointType::kRayTracing, vk::ShaderStageFlagBits::eRaygenKHR |
                                                            vk::ShaderStageFlagBits::eMissKHR |
                                                            vk::ShaderStageFlagBits::eClosestHitKHR |
                                                            vk::ShaderStageFlagBits::eAnyHitKHR |
                                                            vk::ShaderStageFlagBits::eIntersectionKHR |
                                                            vk::ShaderStageFlagBits::eCallableKHR);

    auto indirect_buffer = static_cast<VulkanBuffer*>(dispatch_rays_indirect->indirect_buffer_.buffer);
    vk::DeviceAddress indirect_device_address = indirect_buffer->GetDeviceAddress() + dispatch_rays_indirect->indirect_buffer_.offset;

    // Use SBT regions from state (set by RHIBindShaderBindingTable)
    cmdb.traceRaysIndirect2KHR(indirect_device_address);
}

void VulkanCommandExecutor::RHIAcclerationStructureBarriers(RHICommandQueueBase *cmd, RHICommandAccelerationStructureBarrier *barrier) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();

    auto as = barrier->acceleration_structures_;

    auto vk_barriers = state.Allocate<vk::BufferMemoryBarrier2[]>(barrier->num_barriers_);
    for (const auto& [i, e] : std::views::enumerate(std::span(as, barrier->num_barriers_))) {
        vk_barriers[i].srcStageMask = GetVulkanPipelineStageFlags(barrier->src_stages_[i]);
        vk_barriers[i].dstStageMask = GetVulkanPipelineStageFlags(barrier->dst_stages_[i]);
        vk_barriers[i].srcAccessMask = GetVulkanAccessFlags(barrier->src_accesses_[i]);
        vk_barriers[i].dstAccessMask = GetVulkanAccessFlags(barrier->dst_accesses_[i]);
        vk_barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vk_barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        VulkanAccelerationStructure * vk_as = static_cast<VulkanAccelerationStructure*>(e);
        vk_barriers[i].buffer = vk_as->GetBuffer();
        vk_barriers[i].offset = 0;
        vk_barriers[i].size = vk_as->GetSize();
    }
    state.cmd.pipelineBarrier2(vk::DependencyInfo{
        {}, 0, nullptr, barrier->num_barriers_,
        vk_barriers, 0, nullptr
    });
}


MI_NAMESPACE_END