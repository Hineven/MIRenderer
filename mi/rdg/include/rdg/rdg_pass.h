/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_PASS_H
#define RDG_PASS_H
#include "rdg_resource.h"
#include "rdg/rdg_base.h"
MI_NAMESPACE_BEGIN
struct RDGShaderParamStructAndSizeInfo;

class RenderGraph;

class RDGPass : public NonMovable, public NonCopyable {
public:
    struct RDGTextureUsage {
        RDGTextureUsageType usage;
        RDGTextureRef texture;
    };
    struct RDGBufferUsage {
        RHIGPUAccessFlags access;
        RDGBufferRef buffer;
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

    RDGPass * AddTexture (RDGTexture * texture, RDGTextureUsageType usage) ;
    RDGPass * AddBuffer (RDGBuffer * buffer, RHIGPUAccessFlags access) ;


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

    bool is_compiled_ {};
    // Passes are compiled prior to RDG compilation (by the builder).
    // Gather resources accessed by the shader, initialize in/out resources and detailed resource usage
    // Also, initialize reference holders to relating resources
    void Compile () ;

    std::string name_;
    // The index when the pass is joined to the graph
    int index_ {};
    // Flags for the pass
    RDGPassFlags flags_ {};
    RDGPassType type_ {};

    // Only make sense for non-generic passes
    const RDGShaderParamStructAndSizeInfo * shader_param_struct_info_ {};
    // Only make sense for non-generic passes
    const void * shader_param_data_ {};

    struct {
        // Grouping resources by access types
        std::vector<RDGTexture*> out_textures;
        std::vector<RDGBuffer*> out_buffers;
        std::vector<RDGTexture*> in_textures;
        std::vector<RDGBuffer*> in_buffers;

        // Details of each resource access, and reference holding
        std::vector<RDGTextureUsage> used_textures;
        // Details of each resource access, and reference holding
        std::vector<RDGBufferUsage> used_buffers;
    } compiled_; // Generated after compilation

    // This is filled up by the RDG builder upon spawning the pass
    std::vector<RDGPass*> successive_passes_;

    RDGPassLambda pass_;

    RenderGraph * graph_ {};
};

MI_NAMESPACE_END

#endif //RDG_PASS_H
