/*
 * Created: 2025/3/2
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_SHADER_H
#define RDG_SHADER_H

#include <string>
#include <functional>
#include "rdg/rdg_base.h"
#include "rdg/rdg_param.h"
MI_NAMESPACE_BEGIN

class RHIGraphicsPipeline;
class RHIComputePipeline;

struct RDGShaderInitializationInfo {
    std::string source_location;
    std::string entry_point;
    RHIPipelineType type;
    std::vector<std::string> default_macros;
};

class RDGShaderRegistrator {
public:
    RDGShaderRegistrator (
        size_t type_hash,
        std::function<RDGShaderInitializationInfo()> get_init_info
    ) ;
};

class RDGShader : public RefCounted<true> {
public:
    RDGShader (RDGShaderInitializationInfo ini) ;
    bool Recompile () ;
//    The following functions should be implemented by sub-classes
//  staitc std::vector<std::string> GetDefaultMacros () ;
    FORCEINLINE bool IsValid () const {return is_valid_;}
    FORCEINLINE RHIPipelineType GetType () const {return type_;}
    FORCEINLINE const std::string & GetSourceLocation () const {return source_location_;}
protected:
    // Resource path (infra)
    std::string source_location_ {};
    std::string entry_point_ {};
    bool is_valid_ {false};
    RHIPipelineType type_ {};
    RHIComputePipeline * compute_pipeline_ {nullptr};
    RHIGraphicsPipeline * graphics_pipeline_ {nullptr};
};



class RDGShaderLibrary : public NonMovable, public NonCopyable {
protected:
    RDGShaderLibrary() = default;
public:
    static RDGShaderLibrary & GetInstance() ;
    template<typename T> RDGShader GetShader (std::vector<std::string> macros = {}) ;
    template<typename T> RDGShader Recompile (std::vector<std::string> macros = {}) ;
    void RegisterShader (size_t type_hash, RDGShader * shader) ;
};

MI_NAMESPACE_END
#endif //RDG_SHADER_H
