/*
 * Created: 2025/9/25
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <rdg/rdg.h>
#include <rdg/rdg_shader.h>
#include <renderer/mi_renderer.h>

MI_NAMESPACE_BEGIN

BEGIN_SHADER_PARAMETERS(DiffuseIndirectLightingParams)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousScreenProbeRadianceTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeRadianceTexture)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, PreviousTileProbeMRUQueueBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileProbeMRUQueueBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, SceneProbesBuffer)
END_SHADER_PARAMETERS()

class ReprojectScreenProbesShader : public RDGShader {

};

class SpawnScreenProbesShader : public RDGShader {

};

class PatchScreenProbesShader : public RDGShader {

};

class ReconstructScreenProbesShader : public RDGShader {

};

// Trace update rayus...

class UpdateScreenProbesShader : public RDGShader {

};

class FilterScreenProbesShader : public RDGShader {

};

class ProjectScreenProbesShader : public RDGShader {

};

class IntegrateIndirectDiffuseLightingShader : public RDGShader {

};

void Renderer::Render_ComputeIndirectDiffuseLighting(RendererView * view, RenderGraphBuilder & builder) {

}

MI_NAMESPACE_END