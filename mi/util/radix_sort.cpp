/*
 * Created: 2025/5/30
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <util/radix_sort.h>
#include <rdg/rdg_shader.h>
#include <rdg/rdg_param.h>

#include "rdg/rdg_builder.h"
#include "rdg/rdg_cmd.h"
#include "rdg/rdg_helper.h"

MI_NAMESPACE_BEGIN
struct RadixSortUB {
    uint32_t NumElements; // Number of elements to sort if non-indirect
    uint32_t BitShift; // Bit shift for the current pass
    uint32_t Padding0;
    uint32_t Padding1;
};

static constexpr auto kIndirectMacro = "RADIX_SORT_INDIRECT";

class RadixSortScanShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(RadixSortScanShaderParameters)
        SHADER_UNIFORM_BUFFER(RadixSortUB, UB)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Keys)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Values)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWBins)
        // Count is only valid when the shader is an indirect version
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Count)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(RadixSortScanShaderParameters)
    DECLARE_SHADER()

    static std::vector<std::string> GetShaderDefaultMacros() {
        std::string wave_size = std::to_string(RHI::Get().GetDeviceProperties().wave_size);
        return {"WAVE_SIZE=" + wave_size};
    }
    static std::vector<std::string> GetShaderOptionalMacros() {
        return {kIndirectMacro};
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(RadixSortScanShader, "mi/util/shaders/radix_sort/RadixSort.hlsl", "RadixSortScan");

class RadixSortSumShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(RadixSortSumShaderParameters)
        SHADER_UNIFORM_BUFFER(RadixSortUB, UB)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWBins)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Count)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(RadixSortSumShaderParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros() {
        std::string wave_size = std::to_string(RHI::Get().GetDeviceProperties().wave_size);
        return {"WAVE_SIZE=" + wave_size};
    }
    static std::vector<std::string> GetShaderOptionalMacros() {
        return {kIndirectMacro};
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(RadixSortSumShader, "mi/util/shaders/radix_sort/RadixSort.hlsl", "RadixSortSum");

class RadixSortSumBinsShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(RadixSortSumBinsShaderParameters)
        SHADER_UNIFORM_BUFFER(RadixSortUB, UB)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Bins)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWSumBins)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Count)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(RadixSortSumBinsShaderParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros() {
        std::string wave_size = std::to_string(RHI::Get().GetDeviceProperties().wave_size);
        return {"WAVE_SIZE=" + wave_size};
    }
    static std::vector<std::string> GetShaderOptionalMacros() {
        return {kIndirectMacro};
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(RadixSortSumBinsShader, "mi/util/shaders/radix_sort/RadixSort.hlsl", "RadixSortSumBins");

class RadixSortScatterShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(RadixSortScatterShaderParameters)
        SHADER_UNIFORM_BUFFER(RadixSortUB, UB)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Keys)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Values)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWOutKeys)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWOutValues)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, SumBins)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Bins)
        // Count is only valid when the shader is an indirect version
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Count)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(RadixSortScatterShaderParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros() {
        std::string wave_size = std::to_string(RHI::Get().GetDeviceProperties().wave_size);
        return {"WAVE_SIZE=" + wave_size};
    }
    static std::vector<std::string> GetShaderOptionalMacros() {
        return {kIndirectMacro};
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(RadixSortScatterShader, "mi/util/shaders/radix_sort/RadixSort.hlsl", "RadixSortScatter");

void RadixSort::AddRadixSort32BitsPass(
    RenderGraphBuilder &builder, uint32_t num_elements, RDGBuffer *src_keys_buffer, RDGBuffer *dst_keys_buffer,
    RDGBuffer *src_values_buffer, RDGBuffer *dst_values_buffer, RDGBuffer * count_buffer, const std::string &name
) {
    auto &lib = RDGShaderLibrary::Get();
    auto ini = RDGShaderInitializationInfo{};
    if (count_buffer) ini.macros.push_back(kIndirectMacro);
    auto scan_shader = lib.GetShader<RadixSortScanShader>(ini);
    auto sum_shader = lib.GetShader<RadixSortSumShader>(ini);
    auto sum_bins_shader = lib.GetShader<RadixSortSumBinsShader>(ini);
    auto scatter_shader = lib.GetShader<RadixSortScatterShader>(ini);

    auto temp_keys_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t)
    );
    temp_keys_buffer->SetName("RadixSortTempKeysBuffer");
    auto temp_values_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t)
    );
    temp_values_buffer->SetName("RadixSortTempValuesBuffer");

    auto curr_dst_keys_buffer = temp_keys_buffer.Raw();
    auto curr_dst_values_buffer = temp_values_buffer.Raw();

    auto DivideAndRoundUp = [](uint32_t value, uint32_t divisor) {
        return (value + divisor - 1) / divisor;
    };

    TRef<RDGBuffer> dispatch_command;
    if (count_buffer) {
        dispatch_command = Helpers::SpawnDispatchIndirectCommand1D(builder, count_buffer, kElementsPerSegment);
    }
    for (int i = 0; i < 32; i += 8) {
        auto radix_sort_pass_name = name.empty() ? (std::string("RadixSort_Pass_") + std::to_string(i)) : (name + "_Pass_" + std::to_string(i));
        auto UB = builder.Allocate<RadixSortUB>();
        UB->NumElements = num_elements;
        UB->BitShift = i;
        auto max_num_groups = DivideAndRoundUp(num_elements, kElementsPerSegment);
        auto bins = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, max_num_groups * kBinsPerPass * sizeof(uint32_t));
        bins->SetName("BinsBuffer_" + std::to_string(i));
        auto sum_bins = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, kBinsPerPass * sizeof(uint32_t));
        sum_bins->SetName("SumBinsBuffer_" + std::to_string(i));
        // Scan
        {
            auto params = builder.Allocate<RadixSortScanShader::ShaderParameters>();
            params->UB = UB;
            params->Keys = src_keys_buffer;
            params->Values = src_values_buffer;
            params->RWBins = bins.Raw();
            params->Count = count_buffer;
            builder.AddPass<RadixSortScanShader>(
                {}, params,
                [scan_shader, params, max_num_groups, count_buffer, cmd = dispatch_command.Raw()](RDGPass *pass, RHICommandQueueGraphics &queue) {
                    if (!count_buffer) {
                        RDGCommandHelper::Dispatch<RadixSortScanShader>(queue, pass, scan_shader, params, max_num_groups, 1, 1);
                    } else {
                        RDGCommandHelper::DispatchIndirect<RadixSortScanShader>(queue, pass, scan_shader, params, cmd);
                    }
                }
            )->AddBuffer(dispatch_command.Raw(), RHIGPUAccessFlagBits::kRead)->SetName(radix_sort_pass_name);
        }
        // Sum (using kBinsPerPass groups)
        {
            auto params = builder.Allocate<RadixSortSumShader::ShaderParameters>();
            params->UB = UB;
            params->RWBins = bins.Raw();
            params->Count = count_buffer;
            builder.AddPass<RadixSortSumShader>(
            {}, params,
                [sum_shader, params](RDGPass *pass, RHICommandQueueGraphics &queue) {
                    RDGCommandHelper::Dispatch<RadixSortSumShader>(queue, pass, sum_shader, params, kBinsPerPass, 1, 1);
                }
            )->SetName(radix_sort_pass_name);
        }
        // Sum the sums
        {
            auto params = builder.Allocate<RadixSortSumBinsShader::ShaderParameters>();
            params->UB = UB;
            params->Bins = bins.Raw();
            params->RWSumBins = sum_bins.Raw();
            params->Count = count_buffer;
            builder.AddPass<RadixSortSumBinsShader>(
            {}, params,
                [sum_bins_shader, params](RDGPass *pass, RHICommandQueueGraphics &queue) {
                    RDGCommandHelper::Dispatch<RadixSortSumBinsShader>(queue, pass, sum_bins_shader, params, 1, 1, 1);
                }
            )->SetName(radix_sort_pass_name);
        }
        // Scatter
        {
            auto params = builder.Allocate<RadixSortScatterShader::ShaderParameters>();
            params->UB = UB;
            params->Keys = src_keys_buffer;
            params->Values = src_values_buffer;
            params->RWOutKeys = curr_dst_keys_buffer;
            params->RWOutValues = curr_dst_values_buffer;
            params->Bins = bins.Raw();
            params->SumBins = sum_bins.Raw();
            params->Count = count_buffer;
            builder.AddPass<RadixSortScatterShader>(
                {}, params,
                [scatter_shader, params, max_num_groups, count_buffer, cmd = dispatch_command.Raw()](RDGPass *pass, RHICommandQueueGraphics &queue) {
                    if (!count_buffer) {
                        RDGCommandHelper::Dispatch<RadixSortScatterShader>(queue, pass, scatter_shader, params, (uint32_t)max_num_groups, 1, 1);
                    } else {
                        RDGCommandHelper::DispatchIndirect<RadixSortScatterShader>(queue, pass, scatter_shader, params, cmd);
                    }
                }
            )->AddBuffer(dispatch_command.Raw(), RHIGPUAccessFlagBits::kRead)->SetName(radix_sort_pass_name);
        }
        if (i != 0) {
            std::swap(src_keys_buffer, curr_dst_keys_buffer);
            std::swap(src_values_buffer, curr_dst_values_buffer);
        } else {
            src_keys_buffer = curr_dst_keys_buffer;
            src_values_buffer = curr_dst_values_buffer;
            curr_dst_keys_buffer = dst_keys_buffer;
            curr_dst_values_buffer = dst_values_buffer;
        }
    }
}


MI_NAMESPACE_END