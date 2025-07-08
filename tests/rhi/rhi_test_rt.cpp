/*
 * Created: 2025/4/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <gtest/gtest.h>
#include "core/infra.h"
#include "infra_impl/infra.h"
#include "rhi/rhi.h"
#include "rhi/rhi_thread.h"
#include "rhi/rhi_shader.h"
#include "rhi/rhi_pipeline.h"
#include "rhi/rhi_texture.h"
#include "rhi/rhi_param.h"
#include "rhi/rhi_bindless.h"
#include "rhi/rhi_bindlesskeeper.h"

#include <exception>
#include <cpptrace/from_current.hpp>

#include "stb_image.h"
#include "stb_image_write.h"

#include <rhi/rhi_buffer.h>

#include "rhi/rhi_as.h"

std::string LoadShaderSourceFromFile (std::filesystem::path source_file) {
    std::ifstream file(source_file, std::ios::in | std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + source_file.string());
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

TEST(RHITest, RHIRayTracedTriangle) {
    using namespace mi;
    CPPTRACE_TRY {
        TransferInfra(std::make_unique<MyInfra>());
        GetInfra().Init();
        // Hack: we need to pretend that we're a render thread to pass the assertions
        SetCurrentThreadType(ThreadType::kRenderThread);
        RHI::InitializeSingleton(RHIType::kVulkan);
        // Sim render thread scope
        {
            std::vector<std::string> options;
            options.push_back("-fspv-target-env=vulkan1.3");
            // options.push_back("-fvk-use-scalar-layout");
            options.push_back("-Zi");

            std::string errmsg;

            auto shader_source = LoadShaderSourceFromFile("rhi/resources/ray_traced_triangle.hlsl");
            auto shader_code_span = std::span(shader_source.data(), shader_source.size());
            auto raygen_bcode = GetInfra().CompileHLSLToSPIRV(
                    L"", "Raygen", "cs_6_3", shader_code_span, options, errmsg);
            if (raygen_bcode.empty()) {
                MI_LOG(MIInfraLogType::kError, "Failed to compile vertex shader: {}", errmsg);
            }
            auto raygen_shader = RHI::Get().CreateShader(
                    RHIShaderFrequencyFlagBits::kRayGen, "Raygen",
                    RHIShaderIRType::kSPIRV, std::span(reinterpret_cast<const std::byte *>(raygen_bcode.data()),
                                                       raygen_bcode.size() * sizeof(uint32_t))
            );
            EXPECT_TRUE(raygen_shader);
            auto closest_hit_bcode = GetInfra().CompileHLSLToSPIRV(
                    L"", "ClosestHit", "hs_6_3", shader_code_span, options, errmsg);
            if (closest_hit_bcode.empty()) {
                MI_LOG(MIInfraLogType::kError, "Failed to compile vertex shader: {}", errmsg);
            }
            auto closest_hit_shader = RHI::Get().CreateShader(
                    RHIShaderFrequencyFlagBits::kRayGen, "ClosestHit",
                    RHIShaderIRType::kSPIRV, std::span(reinterpret_cast<const std::byte *>(closest_hit_bcode.data()),
                                                       closest_hit_bcode.size() * sizeof(uint32_t))
            );
            EXPECT_TRUE(closest_hit_shader);
            auto miss_bcode = GetInfra().CompileHLSLToSPIRV(
        L"", "Miss", "hs_6_3", shader_code_span, options, errmsg);
            if (miss_bcode.empty()) {
                MI_LOG(MIInfraLogType::kError, "Failed to compile vertex shader: {}", errmsg);
            }
            auto miss_shader = RHI::Get().CreateShader(
                    RHIShaderFrequencyFlagBits::kRayGen, "Raygen",
                    RHIShaderIRType::kSPIRV, std::span(reinterpret_cast<const std::byte *>(miss_bcode.data()),
                                                       miss_bcode.size() * sizeof(uint32_t))
            );
            EXPECT_TRUE(miss_shader);

            // Check reflection
            auto pipeline_desc = RHIRayTracingPipelineDesc{
                    .shaders = {raygen_shader.Raw(), closest_hit_shader.Raw(), miss_shader.Raw()},
                    .shader_groups = {
                    RHIRayTracingShaderGroupDesc{
                            .type = RHIRayTracingShaderGroupType::kRayGeneration,
                            .general_shader_index = 0
                        },
                        RHIRayTracingShaderGroupDesc{
                            .type = RHIRayTracingShaderGroupType::kTrianglesHitGroup,
                            .closest_hit_shader_index = 1
                        },
                        RHIRayTracingShaderGroupDesc{
                            .type = RHIRayTracingShaderGroupType::kMiss,
                            .general_shader_index = 2
                        }
                    },
                    .max_recursion_depth = 1,
            };
            auto pipeline = RHI::Get().CreateRayTracingPipeline(pipeline_desc);
            EXPECT_TRUE(pipeline);
            EXPECT_TRUE(pipeline->IsValid());

            // Bindless textures
            auto bindless_texture_0_slot = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHITexture>();
            auto bindless_texture_1_slot = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHITexture>();

            TRef<RHITexture> bindless_texture_0;
            {
                const std::string filename = "rhi/resources/test1.png";
                int width, height, channels;
                auto data = stbi_load(filename.c_str(), &width, &height, &channels, 4);
                EXPECT_TRUE(data);
                auto texture = RHI::Get().CreateTexture(
                        RHITextureType::k2D, RHITextureDimensions{static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                        PixelFormatType::kR8G8B8A8_SRGB,
                        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransferDst
                );
                RHI::Get().GetGraphicsCommandQueue().TextureBarrier(
                    texture.Raw(),
                    RHITextureLayoutType::kTransferDstOptimal,
                    RHIPipelineStageFlagBits::kNone,
                    RHIPipelineStageFlagBits::kTransfer,
                    RHIGPUAccessFlagBits::kNone,
                    RHIGPUAccessFlagBits::kWrite
                );
                EXPECT_TRUE(texture);
                auto staging_buf = RHI::Get().CreateBuffer(
                        width * height * 4,
                        RHIBufferUsageFlagBits::kStaging
                );
                memcpy(staging_buf->Map(), data, width * height * 4);
                RHI::Get().GetGraphicsCommandQueue().CopyBufferToTexture(staging_buf->GetSpan(), texture.Raw());
                RHI::Get().GetGraphicsCommandQueue().TextureBarrier(
                        texture.Raw(), RHITextureLayoutType::kShaderReadOnlyOptimal,
                        RHIPipelineStageFlagBits::kTransfer,
                        RHIPipelineStageFlagBits::kAll,
                        RHIGPUAccessFlagBits::kTransferWrite,
                        RHIGPUAccessFlagBits::kRead
                );
                RHI::Get().GetGraphicsCommandQueue().WaitForIdle();
                bindless_texture_0 = texture;
                bindless_texture_0_slot->SetAndCommit(texture.Raw());
                stbi_image_free(data);
            }
            TRef<RHITexture> bindless_texture_1;
            {
                const std::string filename = "rhi/resources/test2.png";
                int width, height, channels;
                auto data = stbi_load(filename.c_str(), &width, &height, &channels, 4);
                EXPECT_TRUE(data);
                auto texture = RHI::Get().CreateTexture(
                        RHITextureType::k2D, RHITextureDimensions{static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                        PixelFormatType::kR8G8B8A8_SRGB,
                        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransferDst
                );
                EXPECT_TRUE(texture);
                RHI::Get().GetGraphicsCommandQueue().TextureBarrier(
                        texture.Raw(), RHITextureLayoutType::kTransferDstOptimal,
                        RHIPipelineStageFlagBits::kNone,
                        RHIPipelineStageFlagBits::kTransfer,
                        RHIGPUAccessFlagBits::kNone,
                        RHIGPUAccessFlagBits::kTransferWrite
                );
                auto staging_buf = RHI::Get().CreateBuffer(
                        width * height * 4,
                        RHIBufferUsageFlagBits::kStaging
                );
                memcpy(staging_buf->Map(), data, width * height * 4);
                RHI::Get().GetGraphicsCommandQueue().CopyBufferToTexture(staging_buf->GetSpan(), texture.Raw());
                RHI::Get().GetGraphicsCommandQueue().TextureBarrier(
                        texture.Raw(), RHITextureLayoutType::kShaderReadOnlyOptimal,
                        RHIPipelineStageFlagBits::kTransfer,
                        RHIPipelineStageFlagBits::kAll,
                        RHIGPUAccessFlagBits::kTransferWrite,
                        RHIGPUAccessFlagBits::kRead
                );
                RHI::Get().GetGraphicsCommandQueue().WaitForIdle();
                bindless_texture_1 = texture;
                bindless_texture_1_slot->SetAndCommit(texture.Raw());
                stbi_image_free(data);
            }
            TRef<RHIAccelerationStructure> blas, tlas;
            // Acceleration structures
            {
                TRef<RHIBuffer> vertex_buffer;
                TRef<RHIBuffer> index_buffer;
                auto& queue = RHI::Get().GetGraphicsCommandQueue();

                {
                    // 定义一个三角形的顶点和索引数据
                    const std::vector<float> vertices = {
                        1.0f,  1.0f, 0.0f,
                       -1.0f,  1.0f, 0.0f,
                        0.0f, -1.0f, 0.0f
                   };
                    const std::vector<uint32_t> indices = { 0, 1, 2 };

                    const size_t vertex_buffer_size = vertices.size() * sizeof(float);
                    const size_t index_buffer_size = indices.size() * sizeof(uint32_t);

                    // 创建用于上传的临时缓冲区
                    auto staging_buf = RHI::Get().CreateBuffer(
                        vertex_buffer_size + index_buffer_size,
                        RHIBufferUsageFlagBits::kStaging
                    );
                    uint8_t* staging_data = reinterpret_cast<uint8_t*>(staging_buf->Map());

                    vertex_buffer = RHI::Get().CreateBuffer(
                        vertex_buffer_size,
                        RHIBufferUsageFlagBits::kAccelerationStructureBuildInput
                    );

                    index_buffer = RHI::Get().CreateBuffer(
                        index_buffer_size,
                        RHIBufferUsageFlagBits::kAccelerationStructureBuildInput
                    );

                    memcpy(staging_data, vertices.data(), vertex_buffer_size);
                    queue.CopyBuffer(staging_buf->GetSpan(0, vertex_buffer_size), vertex_buffer->GetSpan());

                    memcpy(staging_data + vertex_buffer_size, indices.data(), index_buffer_size);
                    queue.CopyBuffer(staging_buf->GetSpan(vertex_buffer_size, index_buffer_size), index_buffer->GetSpan());

                    queue.BufferBarrier(
                        vertex_buffer->GetSpan(),
                        RHIPipelineStageFlagBits::kTransfer,
                        RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                        RHIGPUAccessFlagBits::kTransferWrite,
                        RHIGPUAccessFlagBits::kAccelerationStructureRead
                    );
                    queue.BufferBarrier(
                        index_buffer->GetSpan(),
                        RHIPipelineStageFlagBits::kTransfer,
                        RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                        RHIGPUAccessFlagBits::kTransferWrite,
                        RHIGPUAccessFlagBits::kAccelerationStructureRead
                    );
                }
                RHIASGeometry geometry {
                    RHIASGeometryType::kTriangles,
                    RHIASGeometryFlagBits::kOpaque
                };
                geometry.triangles = {
                    .vertex_data = vertex_buffer->GetSpan(),
                    .vertex_format = RHIVertexAttributeFormatType::k3xFp32,
                    .vertex_count = 3,
                    .vertex_stride = sizeof(float) * 3,
                    .index_data = index_buffer->GetSpan(),
                    .index_count = 3
                };
                blas = RHI::Get().CreateAccelerationStructure(
                        RHIAccelerationStructureType::kBottomLevel
                );
                auto build_info = RHIAccelerationStructureBuildGeometryInfo{
                    RHIAccelerationStructureType::kBottomLevel,
                    RHIAccelerationStructureBuildFlagBits::kPreferFastTrace,
                    RHIAccelerationStructureBuildMode::kBuild,
                    nullptr,
                    nullptr,
                    {&geometry, 1}
                };
                auto sizes = blas->GetBuildSizes(build_info);
                blas->Create(sizes.acceleration_structure_size);
                auto scratch = RHI::Get().CreateBuffer(
                        sizes.build_scratch_size,
                        RHIBufferUsageFlagBits::kAccelerationStructureScratch
                );
                build_info.dst_acceleration_structure = blas.Raw();
                queue.BuildAccelerationStructure(build_info, scratch->GetSpan());
                tlas = RHI::Get().CreateAccelerationStructure(
                        RHIAccelerationStructureType::kTopLevel
                );
                build_info = RHIAccelerationStructureBuildGeometryInfo{
                    RHIAccelerationStructureType::kBottomLevel,
                    RHIAccelerationStructureBuildFlagBits::kPre ferFastTrace,
                    RHIAccelerationStructureBuildMode::kBuild,
                    nullptr,
                    nullptr,
                    {},
                    {},
                    1
                };
                RHIAccelerationStructureInstance
            }
            // Render texture
            auto output_texture = RHI::Get().CreateTexture(
                    RHITextureType::k2D, RHITextureDimensions{1280, 720}, PixelFormatType::kR8G8B8A8_SRGB,
                    RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransfer
            );
            EXPECT_TRUE(output_texture);

            auto uniform_buf = RHI::Get().CreateBuffer(
                    256,
                    RHIBufferUsageFlagBits::kUniform
            );

            auto &queue = RHI::Get().GetGraphicsCommandQueue();
            queue.TextureBarrier(
                    output_texture.Raw(),
                    RHITextureLayoutType::kTransferDstOptimal,
                    RHIPipelineStageFlagBits::kNone,
                    RHIPipelineStageFlagBits::kTransfer,
                    RHIGPUAccessFlagBits::kNone,
                    RHIGPUAccessFlagBits::kWrite
            );
            queue.ClearTexture(output_texture.Raw(), {0.f, 0.f, 0.f, 0.f});
            queue.TextureBarrier(
                    output_texture.Raw(),
                    RHITextureLayoutType::kColorAttachment,
                    RHIPipelineStageFlagBits::kTransfer,
                    RHIPipelineStageFlagBits::kOrdinaryGraphics,
                    RHIGPUAccessFlagBits::kWrite,
                    RHIGPUAccessFlagBits::kRW
            );
            queue.BindPipeline(pipeline.Raw());
            RHIDrawDesc ds {};
            ds.SetAttachment(0, output_texture.Raw());
            ds.SetClearValue(0, {0.f, 1.f, 0.f, 1.f});

            queue.UpdateDrawState(ds);


            auto staging_buf = RHI::Get().CreateBuffer(
                    1024 * 1024 * 32,
                    RHIBufferUsageFlagBits::kStaging
            );

            auto params = RHIBindPipelineParametersDesc{};
            auto linear_wrap = RHI::Get().GetGlobalSamplers().linear_wrap;
            auto samplers = queue.Allocate<RHIPipelineParameterResourceDesc[]>(1);
            samplers[0].resource = linear_wrap;
            auto sampler_binding = pipeline->ReflectResourceSlot("sampler_linear");
            samplers[0].slot = sampler_binding.slot_index;
            params.samplers = {samplers, 1};
            auto storage_textures = queue.Allocate<RHIPipelineParameterTextureDesc[]>(1);
            storage_textures[0].texture = output_texture.Raw();
            params.uavs = {storage_textures, 1};
            queue.BindPipelineParameters(RHIBindPointType::kRayTracing, params);
            queue.DispatchRays(1280, 720, 1);

            queue.TextureBarrier(
                    output_texture.Raw(),
                    RHITextureLayoutType::kTransferSrcOptimal,
                    RHIPipelineStageFlagBits::kOrdinaryGraphics | RHIPipelineStageFlagBits::kTransfer,
                    RHIPipelineStageFlagBits::kTransfer,
                    RHIGPUAccessFlagBits::kWrite,
                    RHIGPUAccessFlagBits::kRead
            );
            queue.CopyTextureToBuffer(output_texture.Raw(), staging_buf.Raw());
            auto sync = RHI::Get().CreateSyncPoint();
            queue.EnqueueTranslateAndSubmit(sync.Raw());
            sync->Wait();

            RHI::Get().AdvanceFrame();
            // Convert to bitmap
            auto u8_tex = (uint8_t *) staging_buf->Map();
            auto bitmap = new uint32_t[1280 * 720];
            for (int i = 0; i < 1280 * 720; ++i) {
                auto r = static_cast<uint8_t>(u8_tex[i * 4 + 0]);
                auto g = static_cast<uint8_t>(u8_tex[i * 4 + 1]);
                auto b = static_cast<uint8_t>(u8_tex[i * 4 + 2]);
                auto a = static_cast<uint8_t>(u8_tex[i * 4 + 3]);
                // bgra
                bitmap[i] = (a << 24) | (b << 16) | (g << 8) | r;
            }
            // Save to file
            stbi_write_png("rhi_test_rt.png", 1280, 720, 4, bitmap, 1280 * 4);
        }
        RHI::DestroySingleton();
        GetInfra().Shutdown();
        DestroyInfra();
    } CPPTRACE_CATCH (const std::exception &e) {
        cpptrace::from_current_exception().print();
        FAIL() << e.what();
    }
}