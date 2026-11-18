#include <Graphics/DeviceTypes.h>

// 파이프라인 고정 기능 상태의 장치 무관 규칙이다(RenderBase 4.4절 GX3).
namespace rocks::graphics {

namespace {

bool ValidStencilFace(const StencilFace& face) noexcept {
    const auto valid = [](StencilOperation operation) noexcept {
        return operation <= StencilOperation::DecrementWrap;
    };
    return face.compare <= CompareFunction::Always && valid(face.fail) && valid(face.depthFail) &&
           valid(face.pass);
}

bool ValidBlend(const ColorTargetBlend& blend) noexcept {
    const auto factor = [](BlendFactor value) noexcept {
        return value <= BlendFactor::SourceAlphaSaturate;
    };
    return factor(blend.sourceColor) && factor(blend.destinationColor) &&
           factor(blend.sourceAlpha) && factor(blend.destinationAlpha) &&
           blend.colorOperation <= BlendOperation::Max &&
           blend.alphaOperation <= BlendOperation::Max &&
           (blend.writeMask & ColorWriteMask::All) == blend.writeMask;
}

bool SameBlend(const ColorTargetBlend& left, const ColorTargetBlend& right) noexcept {
    return left.enable == right.enable && left.sourceColor == right.sourceColor &&
           left.destinationColor == right.destinationColor &&
           left.colorOperation == right.colorOperation && left.sourceAlpha == right.sourceAlpha &&
           left.destinationAlpha == right.destinationAlpha &&
           left.alphaOperation == right.alphaOperation && left.writeMask == right.writeMask;
}

} // namespace

Result<void> ValidatePipelineDesc(const PipelineDesc& desc) noexcept {
    const auto invalid = []() noexcept {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    };
    if (desc.colorTargetCount > MaxColorTargets || desc.topology > PrimitiveTopology::PointList ||
        desc.raster.cullMode > CullMode::Front) {
        return invalid();
    }

    // 형식이 그 용도(렌더 타깃·깊이)를 받는지는 장치 능력이 정한다(backend가 Unsupported로 공개).
    for (uint32 index = 0; index < desc.colorTargetCount; ++index) {
        if (!IsValidPixelFormat(desc.colorFormats[index]) || !ValidBlend(desc.blend[index])) {
            return invalid();
        }
    }

    const bool hasDepth = desc.depthFormat != PixelFormat::Unknown;
    if (hasDepth && !IsValidPixelFormat(desc.depthFormat)) {
        return invalid();
    }

    const DepthStencilState& state = desc.depthStencil;
    if ((desc.colorTargetCount == 0 && !hasDepth) ||
        ((state.depthTest || state.depthWrite) && !hasDepth) ||
        (state.stencilTest && GetPixelFormatInfo(desc.depthFormat).stencilBits == 0) ||
        state.depthCompare > CompareFunction::Always || !ValidStencilFace(state.front) ||
        !ValidStencilFace(state.back)) {
        return invalid();
    }

    return Result<void>::Success();
}

Result<void> CheckPipelineFeatures(const DeviceCaps& caps, const PipelineDesc& desc) noexcept {
    const DeviceFeatures& features = caps.features;
    if (desc.colorTargetCount > caps.limits.colorAttachments ||
        (desc.raster.depthClamp && !features.depthClamp) ||
        (desc.raster.depthBiasClamp != 0.0f && !features.depthBiasClamp) ||
        (desc.alphaToCoverage && !features.alphaToCoverage)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (!features.independentBlend) {
        for (uint32 index = 1; index < desc.colorTargetCount; ++index) {
            if (!SameBlend(desc.blend[index], desc.blend[0])) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }
        }
    }

    return Result<void>::Success();
}

// 파이프라인이 깊이·스텐실을 쓰는지다(읽기 전용 깊이 attachment에서 그리기를 거르는 데 쓴다).
bool PipelineWritesDepthStencil(const PipelineDesc& desc) noexcept {
    const DepthStencilState& state = desc.depthStencil;
    const auto writes = [](const StencilFace& face) noexcept {
        return face.fail != StencilOperation::Keep || face.depthFail != StencilOperation::Keep ||
               face.pass != StencilOperation::Keep;
    };
    return (state.depthTest && state.depthWrite) ||
           (state.stencilTest && state.stencilWriteMask != 0 &&
            (writes(state.front) || writes(state.back)));
}

} // namespace rocks::graphics
