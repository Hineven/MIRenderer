/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_PASS_H
#define RDG_PASS_H
#include "rdg/rdg_base.h"
MI_NAMESPACE_BEGIN
struct RDGShaderParamStructInfo;

class RDGPass : public NonMovable, public NonCopyable {
protected:
    // Can only be allocated by RDG
    RDGPass() = default;
public:
    virtual ~RDGPass() = default;
    virtual void Execute (RenderResourcePool & pool) = 0;
protected:
    const RDGShaderParamStructInfo * shader_param_struct_info_;
    const void * shader_param_data_;
    // It's the graph's task to count resource usage. We'll use plain pointers here.
    std::vector<RDGTexture*> out_textures_;
    std::vector<RDGBuffer*> out_buffers_;
    std::vector<RDGTexture*> in_textures_;
    std::vector<RDGBuffer*> in_buffers_;
    // Private uniform buffer
    RDGBuffer * uniform_buffer_;
    //
    std::vector<const RDGBuffer*> referenced_uniform_buffers_;

    // Gather resources accessed by the shader, initialize in/out resources
    void GatherInOutResources () ;

    std::function<void()> pass_;
};

MI_NAMESPACE_END

#endif //RDG_PASS_H
