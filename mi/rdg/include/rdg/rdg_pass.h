/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_PASS_H
#define RDG_PASS_H
#include "rdg_resource.h"
#include "rdg/rdg_fwd.h"
MI_NAMESPACE_BEGIN
struct RDGShaderParamStructAndSizeInfo;

class RenderGraph;

class RDGPass : public NonMovable, public NonCopyable {
public:
    struct RDGTextureUsage {
        RHITextureLayoutType layout;
        RHIGPUAccessFlags access;
        RHIPipelineStageFlags stages;
        RDGTextureRef texture;
    };
    struct RDGBufferUsage {
        RHIGPUAccessFlags access;
        RHIPipelineStageFlags stages;
        RDGBufferRef buffer;
    };
    struct RDGASUsage {
        RHIGPUAccessFlags access;
        RHIPipelineStageFlags stages;
        RHIAccelerationStructure* as;
    };
protected:
    // Can only be allocated by RDG
    RDGPass(
        std::string name,
        int index,
        RDGPassType pass_type,
        RDGPassFlags flags,
        RDGPassLambda && pass
        // const RDGShaderParamStructAndSizeInfo * shader_param_struct_info,
        // const void * shader_param_data,
        // RenderGraph * graph
    );
public:

    // Specify how are you using the texture in the pass. kNone for stgages will be replaced with auto-detected stages.
    // This is a function mostly for convenience. Using AddTexture(texture, layout, ...) for precise controls.
    RDGPass * AddTextureH (RDGTexture * texture, RDGTextureUsageType usage, RHIPipelineStageFlags stages = RHIPipelineStageFlagBits::kNone) ;
    // Describe how are you using the texture in the pass, without any heuristics
    RDGPass * AddTexture (RDGTexture * texture, RHITextureLayoutType layout, RHIGPUAccessFlags access, RHIPipelineStageFlags stages);
    // Specify how are you using the buffer in the pass, with access flags. kNone for stgages will be replaced with auto-detected stages.
    RDGPass * AddBufferH (RDGBuffer * buffer, RHIGPUAccessFlags access, RHIPipelineStageFlags stages = RHIPipelineStageFlagBits::kNone) ;
    // Describe how are you using the buffer in the pass, without any heuristics
    RDGPass * AddBuffer (RDGBuffer * buffer, RHIGPUAccessFlags access, RHIPipelineStageFlags stages) ;
    // Add an acceleration structure to the pass, with access flags. kNone for stages will be replaced with auto-detected stages.
    // NOTE: Unlike other RDG resources, this info is only used for pass dependency analysis and not used for automatic barrier placement.
    // You still have to manually place barriers for acceleration structures inside passes.
    RDGPass * AddASH_NoAutomaticBarrier (RHIAccelerationStructure * as, RHIGPUAccessFlags access, RHIPipelineStageFlags stages = RHIPipelineStageFlagBits::kNone) ;

    FORCEINLINE void SetName (std::string name) {
        name_ = std::move(name);
    }

    FORCEINLINE const std::string & GetName () const {
        return name_;
    }

    ~RDGPass() ;
    friend class RenderGraphBuilder;
    friend class RenderGraph;
    friend class RDGCommandHelper;
    FORCEINLINE RDGPassFlags GetFlags () const {return flags_;}
    FORCEINLINE RDGPassType GetType () const {return type_;}
    FORCEINLINE RHIPipelineStageFlags GetStageFlags () const {
        switch (type_) {
            case RDGPassType::kGraphics:
                return RHIPipelineStageFlagBits::kOrdinaryGraphics;
            case RDGPassType::kCompute:
                return RHIPipelineStageFlagBits::kCompute;
            case RDGPassType::kGeneric:
                // TODO stricter control
                return RHIPipelineStageFlagBits::kAll;
            default:
                assert(false);
                return {};
        }
    }

    FORCEINLINE RenderGraph * GetGraph () const {return graph_;}

protected:

    bool is_pre_compiled_ {};
    void PreCompile () ;

    bool is_compiled_ {};
    // Passes are pre compiled prior to RDG compilation (by the builder).
    // Gather resources accessed by the shader, initialize in/out resources and detailed resource usage
    void Compile () ;

    std::string name_;
    // The index when the pass is joined to the graph
    int index_ {};
    // Flags for the pass
    RDGPassFlags flags_ {};
    RDGPassType type_ {};

    // Only make sense for non-generic passes
    const RDGShaderParamStructAndSizeInfo * shader_param_struct_info_ {};
    const RDGShader * shader_ {};
    // Only make sense for non-generic passes
    const void * shader_param_data_ {};

    struct {
        // Grouping resources by access types
        std::vector<RDGTexture*> out_textures;
        std::vector<RDGBuffer*> out_buffers;
        std::vector<RHIAccelerationStructure*> out_acceleration_structures;
        std::vector<RDGTexture*> in_textures;
        std::vector<RDGBuffer*> in_buffers;
        std::vector<RHIAccelerationStructure*> in_acceleration_structures;

        // Details of each resource access after compilation and alias binning, and reference holding
        std::vector<RDGTextureUsage> textures;
        // Details of each resource access after compilation and alias binning, and reference holding
        std::vector<RDGBufferUsage> buffers;
        // Details of each acceleration structure access after compilation and alias binning
        std::vector<RDGASUsage> acceleration_structures;
    } compiled_; // Generated after compilation


    // Keep references for resources used in the pass prior to compilation.
    std::vector<RDGTextureUsage> used_textures;
    // Keep references for resources used in the pass prior to compilation.
    std::vector<RDGBufferUsage> used_buffers;
    // Keep references for resources used in the pass prior to compilation.
    std::vector<RDGASUsage> used_acceleration_structures;

    // This is filled up by the RDG builder upon spawning the pass
    std::vector<RDGPass*> successive_passes_;

    RDGPassLambda pass_;

    RenderGraph * graph_ {};
};

MI_NAMESPACE_END

#endif //RDG_PASS_H
