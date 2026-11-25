#include <RenderBase/EngineShaderTypes.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/PipelineStatePresets.h>

// 전체화면 pass 도우미다(Docs/Design/RenderBase 11.4절 3항).
namespace rocks::render {

Result<GraphicsPipelineRequest>
FullscreenPipelineRequest(ShaderSet& set, uint32 pixelEntry,
                          graphics::PixelFormat colorFormat) noexcept {
    auto vertex = set.FindEntry(FullscreenTriangleVertexType.hash);
    if (!vertex) {
        return Result<GraphicsPipelineRequest>::Failure(vertex.Error());
    }

    if (pixelEntry >= set.EntryCount()) {
        return Result<GraphicsPipelineRequest>::Failure(ErrorCode::InvalidArgument);
    }

    GraphicsPipelineRequest request;
    request.set = &set;
    request.vertexEntry = vertex.Value();
    request.pixelEntry = pixelEntry;
    request.state.colorTargetCount = 1;
    request.state.colorFormats[0] = colorFormat;
    SetBlend(request.state, OpaqueBlend());
    request.state.raster = NoCull();
    request.state.depthStencil = DepthDisabled();
    return Result<GraphicsPipelineRequest>::Success(request);
}

} // namespace rocks::render
