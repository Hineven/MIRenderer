/*
 * Created: 2025/10/1
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <../include/renderer/util/scan_sum.h>
#include <rdg/rdg_shader.h>
#include <rdg/rdg_param.h>

#include "rdg/rdg_builder.h"
#include "rdg/rdg_cmd.h"
#include "rdg/rdg_helper.h"

MI_NAMESPACE_BEGIN

struct ScanSumUB {
    uint32_t NumElements; // Number of elements to sort if non-indirect
    uint32_t Padding2;
    uint32_t Padding0;
    uint32_t Padding1;
};

static constexpr auto kIndirectMacro = "SCAN_SUM_INDIRECT";

class ScanSumBlockSumShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(ScanSumBlockSumShaderParameters)
        SHADER_UNIFORM_BUFFER(ScanSumUB, UB)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Values)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWBlockSums)
        // Count is only valid when the shader is an indirect version
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Count)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(ScanSumBlockSumShaderParameters)
    DECLARE_SHADER()

    static std::vector<std::string> GetShaderDefaultMacros() {
        std::string wave_size = std::to_string(RHI::Get().GetDeviceProperties().wave_size);
        return {
            "WAVE_SIZE=" + wave_size,
            "THREADS_PER_GROUP=" + std::to_string(DeviceScanSum::kThreadsPerGroup),
            "ELEMENTS_PER_SEGMENT=" + std::to_string(DeviceScanSum::kElementsPerSegment)
        };
    }
    static std::vector<std::string> GetShaderOptionalMacros() {
        return {kIndirectMacro};
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(ScanSumBlockSumShader, "mi/renderer/shaders/scan_sum/ScanSum.hlsl", "ScanSumBlockSum");

class ScanSumSumBlockSumsShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(ScanSumSumBlockSumsShaderParameters)
        SHADER_UNIFORM_BUFFER(ScanSumUB, UB)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWBlockSums)
        // Count is only valid when the shader is an indirect version
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Count)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(ScanSumSumBlockSumsShaderParameters)
    DECLARE_SHADER()

    static std::vector<std::string> GetShaderDefaultMacros() {
        std::string wave_size = std::to_string(RHI::Get().GetDeviceProperties().wave_size);
        return {
            "WAVE_SIZE=" + wave_size,
            "THREADS_PER_GROUP=" + std::to_string(DeviceScanSum::kThreadsPerGroup),
            "ELEMENTS_PER_SEGMENT=" + std::to_string(DeviceScanSum::kElementsPerSegment)
        };
    }
    static std::vector<std::string> GetShaderOptionalMacros() {
        return {kIndirectMacro};
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(ScanSumSumBlockSumsShader, "mi/renderer/shaders/scan_sum/ScanSum.hlsl", "ScanSumSumBlockSums");

class ScanSumScatterBlockSumsShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(ScanSumScatterBlockSumsShaderParameters)
        SHADER_UNIFORM_BUFFER(ScanSumUB, UB)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Values)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWOutValues)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, BlockSums)
        // Count is only valid when the shader is an indirect version
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Count)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(ScanSumScatterBlockSumsShaderParameters)
    DECLARE_SHADER()

    static std::vector<std::string> GetShaderDefaultMacros() {
        std::string wave_size = std::to_string(RHI::Get().GetDeviceProperties().wave_size);
        return {
            "WAVE_SIZE=" + wave_size,
            "THREADS_PER_GROUP=" + std::to_string(DeviceScanSum::kThreadsPerGroup),
            "ELEMENTS_PER_SEGMENT=" + std::to_string(DeviceScanSum::kElementsPerSegment)
        };
    }
    static std::vector<std::string> GetShaderOptionalMacros() {
        return {kIndirectMacro};
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(ScanSumScatterBlockSumsShader, "mi/renderer/shaders/scan_sum/ScanSum.hlsl", "ScanSumScatterBlockSums");

void DeviceScanSum::AddScanSum32BitsPass(
    RenderGraphBuilder &builder, uint32_t num_elements,
    RDGBuffer *src_values_buffer, RDGBuffer *dst_values_buffer,
    RDGBuffer * count_buffer, const std::string &name
) {
    RDGSectionGuard section(builder, name.empty() ? "ScanSum32Bits" : (name + "_ScanSum32Bits"));

    auto &lib = RDGShaderLibrary::Get();
    auto ini = RDGShaderInitializationInfo{};
    if (count_buffer) ini.optional_macros.push_back(kIndirectMacro);
    auto block_sum_shader = lib.GetShader<ScanSumBlockSumShader>(ini);
    auto sum_block_sums_shader = lib.GetShader<ScanSumSumBlockSumsShader>(ini);
    auto scatter_block_sums_shader = lib.GetShader<ScanSumScatterBlockSumsShader>(ini);

    auto temp_block_sums_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage | RHIBufferUsageFlagBits::kShaderDeviceAddress,
        sizeof(uint32_t) * DivideAndRoundUp(num_elements, kElementsPerSegment)
    );
    temp_block_sums_buffer->SetName("TempBlockSumsBuffer" + name);
    TRef<RDGBuffer> dispatch_command;
    if (count_buffer) {
        dispatch_command = Helpers::SpawnDispatchIndirectCommand1D(builder, count_buffer, kElementsPerSegment);
    }
    auto UB = builder.Allocate<ScanSumUB>();
    UB->NumElements = num_elements;
    // Block scan
    {
        auto params = builder.Allocate<ScanSumBlockSumShader::ScanSumBlockSumShaderParameters>();
        params->UB = UB;
        params->Values = src_values_buffer;
        params->RWBlockSums = temp_block_sums_buffer.Raw();
        params->Count = count_buffer;
        if (count_buffer) {
            Helpers::AddComputeIndirectPass<ScanSumBlockSumShader>(
                builder, block_sum_shader, params, dispatch_command.Raw()
            )->SetName(name + "_BlockSum");
        } else {
            Helpers::AddComputePass<ScanSumBlockSumShader>(
                builder, block_sum_shader, params,
                DivideAndRoundUp(num_elements, kElementsPerSegment)
            )->SetName(name + "_BlockSum");
        }
    }
    // Sum the block sums
    {
        auto params = builder.Allocate<ScanSumSumBlockSumsShader::ScanSumSumBlockSumsShaderParameters>();
        params->UB = UB;
        params->RWBlockSums = temp_block_sums_buffer.Raw();
        params->Count = count_buffer;
        Helpers::AddComputePass<ScanSumSumBlockSumsShader>(
            builder, sum_block_sums_shader, params,
            1
        )->SetName(name + "_SumBlockSums");
    }
    // Scatter the block sums
    {
        auto params = builder.Allocate<ScanSumScatterBlockSumsShader::ScanSumScatterBlockSumsShaderParameters>();
        params->UB = UB;
        params->Values = src_values_buffer;
        params->RWOutValues = dst_values_buffer;
        params->BlockSums = temp_block_sums_buffer.Raw();
        params->Count = count_buffer;
        if (count_buffer) {
            Helpers::AddComputeIndirectPass<ScanSumScatterBlockSumsShader>(
                builder, scatter_block_sums_shader, params, dispatch_command.Raw()
            )->SetName(name + "_ScatterBlockSums");
        } else {
            Helpers::AddComputePass<ScanSumScatterBlockSumsShader>(
                builder, scatter_block_sums_shader, params,
                DivideAndRoundUp(num_elements, kElementsPerSegment)
            )->SetName(name + "_ScatterBlockSums");
        }
    }

}

MI_NAMESPACE_END