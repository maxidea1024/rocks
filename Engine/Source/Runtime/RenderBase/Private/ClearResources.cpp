#include <Graphics/ShaderBindings.h>
#include <Platform/Memory.h>
#include <RenderBase/ClearResources.h>
#include <RenderBase/ComputeDispatch.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/PipelineStatePresets.h>
#include <RenderBase/UtilityShaderTypes.h>

// 자원 clear 도우미다(Docs/Design/RenderBase 11.2절 RR17, 엔진 셰이더 14.4절 4항·14.5절 3항).
namespace rocks::render {

namespace {

using graphics::PixelFormat;
using graphics::TextureDimension;

// 간접 인자 offset 정렬이다.
constexpr uint32 DispatchArgumentsAlignment = 4;
// 스텐실 쓰기 mask(8 bit 전부)다.
constexpr uint8 FullStencilMask = 0xFF;

// storage 형식이 GLES 형식에서 컴파일되는가(셰이더 거르기와 같다).
bool StorageFormatCompiled(const graphics::Device& device, StorageImageFormat format) noexcept {
    return device.Info().backend != graphics::Backend::OpenGL ||
           static_cast<uint32>(format) < GlesStorageImageFormatCount;
}

// UAV clear 파이프라인을 찾거나 만들고 묶는다.
Result<void> BindClearWritable(graphics::Device& device, graphics::CommandContext& commands,
                               ShaderSet& set, PipelineCache& cache, ClearDimension dimension,
                               StorageImageFormat format, bool buffer) noexcept {
    if (!StorageFormatCompiled(device, format)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto entry = set.FindEntry(ClearWritableComputeType.hash, shader::NoVertexInputType,
                               ClearWritablePermutation(dimension, format));
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    ComputePipelineRequest request;
    request.set = &set;
    request.computeEntry = entry.Value();
    request.state.drawConstantBytes = sizeof(ClearWritableConstants);
    if (buffer) {
        request.state.bindings.writableBuffers[0] = ClearTargetName;
    } else {
        request.state.bindings.writableTextures[0] = ClearTargetName;
    }

    auto pipeline = cache.FindOrCreate(request);
    if (!pipeline) {
        return Result<void>::Failure(pipeline.Error());
    }

    commands.SetPipeline(pipeline.Value());
    return Result<void>::Success();
}

// 형식의 값 계열이 부분 clear PS 퍼뮤테이션인가.
bool ClearValueTypeOf(PixelFormat format, ClearValueType& out) noexcept {
    const graphics::PixelFormatInfo& info = graphics::GetPixelFormatInfo(format);
    if (info.kind != graphics::PixelFormatKind::Color) {
        return false;
    }

    switch (info.shaderType) {
    case graphics::PixelShaderType::Float:
        out = ClearValueType::Float;
        return true;
    case graphics::PixelShaderType::Uint:
        out = ClearValueType::Uint;
        return true;
    case graphics::PixelShaderType::Int:
        out = ClearValueType::Sint;
        return true;
    default:
        return false;
    }
}

bool IsDepthFormat(PixelFormat format) noexcept {
    const graphics::PixelFormatKind kind = graphics::GetPixelFormatInfo(format).kind;
    return kind == graphics::PixelFormatKind::Depth ||
           kind == graphics::PixelFormatKind::DepthStencil;
}

} // namespace

ClearValue ClearValue::Float(float r, float g, float b, float a) noexcept {
    const float values[4] = {r, g, b, a};
    ClearValue value;
    platform::MemoryCopy(value.words, values, sizeof(values));
    return value;
}

ClearValue ClearValue::Uint(uint32 r, uint32 g, uint32 b, uint32 a) noexcept {
    ClearValue value;
    value.words[0] = r;
    value.words[1] = g;
    value.words[2] = b;
    value.words[3] = a;
    return value;
}

ClearValue ClearValue::Sint(int32 r, int32 g, int32 b, int32 a) noexcept {
    const int32 values[4] = {r, g, b, a};
    ClearValue value;
    platform::MemoryCopy(value.words, values, sizeof(values));
    return value;
}

Result<void> ClearWritableTexture(graphics::Device& device, graphics::CommandContext& commands,
                                  ShaderSet& set, PipelineCache& cache,
                                  graphics::TextureHandle view, const ClearValue& value,
                                  const WritableTextureRegion& region) noexcept {
    if (region.width == 0 || region.height == 0 || region.depth == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto info = device.GetTextureViewInfo(view);
    if (!info) {
        return Result<void>::Failure(info.Error());
    }

    if (info.Value().access != graphics::ViewAccess::ReadWrite) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ClearDimension dimension = ClearDimension::Texture2D;
    switch (info.Value().dimension) {
    case TextureDimension::Texture2D:
        dimension = ClearDimension::Texture2D;
        break;
    case TextureDimension::Texture2DArray:
        dimension = ClearDimension::Texture2DArray;
        break;
    case TextureDimension::Texture3D:
        dimension = ClearDimension::Texture3D;
        break;
    default:
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (dimension == ClearDimension::Texture2D && (region.z != 0 || region.depth != 1)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto format = StorageImageFormatOf(info.Value().format);
    if (!format) {
        return Result<void>::Failure(format.Error());
    }

    if (auto bound =
            BindClearWritable(device, commands, set, cache, dimension, format.Value(), false);
        !bound) {
        return bound;
    }

    ClearWritableConstants constants;
    constants.value = {value.words[0], value.words[1], value.words[2], value.words[3]};
    constants.minBounds = {region.x, region.y, region.z, 0};
    constants.maxBounds = {region.x + region.width, region.y + region.height,
                           region.z + region.depth, 0};
    commands.SetWritableTexture(0, view);
    if (auto uploaded = commands.SetDrawConstants(graphics::DrawConstantBytes(constants));
        !uploaded) {
        return uploaded;
    }

    const ComputeGroupCount groups =
        dimension == ClearDimension::Texture2D
            ? GroupCountFor(region.width, region.height, ClearTexture2DGroupSide,
                            ClearTexture2DGroupSide)
            : GroupCountFor(region.width, region.height, region.depth, ClearVolumeGroupSide,
                            ClearVolumeGroupSide, ClearVolumeGroupSide);
    if (auto valid = ValidateGroupCount(device.Caps().limits, groups); !valid) {
        return valid;
    }

    commands.Dispatch(groups.x, groups.y, groups.z);
    return Result<void>::Success();
}

Result<void> ClearWritableBuffer(graphics::Device& device, graphics::CommandContext& commands,
                                 ShaderSet& set, PipelineCache& cache,
                                 graphics::BufferViewHandle view, PixelFormat format,
                                 const ClearValue& value, uint32 first, uint32 count) noexcept {
    if (count == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto info = device.GetBufferViewInfo(view);
    if (!info) {
        return Result<void>::Failure(info.Error());
    }

    if (info.Value().kind != graphics::BufferViewKind::Typed ||
        info.Value().access != graphics::ViewAccess::ReadWrite ||
        info.Value().shaderType != graphics::GetPixelFormatInfo(format).shaderType) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto storage = StorageImageFormatOf(format);
    if (!storage) {
        return Result<void>::Failure(storage.Error());
    }

    if (auto bound = BindClearWritable(device, commands, set, cache, ClearDimension::Buffer,
                                       storage.Value(), true);
        !bound) {
        return bound;
    }

    ClearWritableConstants constants;
    constants.value = {value.words[0], value.words[1], value.words[2], value.words[3]};
    constants.minBounds = {first, 0, 0, 0};
    constants.maxBounds = {first + count, 0, 0, 0};
    commands.SetWritableBufferView(0, view);
    if (auto uploaded = commands.SetDrawConstants(graphics::DrawConstantBytes(constants));
        !uploaded) {
        return uploaded;
    }

    const ComputeGroupCount groups = WrapGroupCount(GroupsFor(count, ClearBufferGroupThreads));
    if (auto valid = ValidateGroupCount(device.Caps().limits, groups); !valid) {
        return valid;
    }

    commands.Dispatch(groups.x, groups.y, groups.z);
    return Result<void>::Success();
}

Result<void> ClearRegion(graphics::CommandContext& commands, ShaderSet& set, PipelineCache& cache,
                         const ClearRegionDesc& desc) noexcept {
    const graphics::ScissorRect& rect = desc.rect;
    const bool clearsDepthStencil = desc.clearDepth || desc.clearStencil;
    ClearValueType valueType = ClearValueType::Float;
    const bool colorValid =
        desc.colorFormat == PixelFormat::Unknown || ClearValueTypeOf(desc.colorFormat, valueType);
    const bool depthValid =
        desc.depthFormat == PixelFormat::Unknown || IsDepthFormat(desc.depthFormat);
    if ((!desc.clearColor && !clearsDepthStencil) || rect.width == 0 || rect.height == 0 ||
        rect.x > desc.targetWidth || rect.y > desc.targetHeight ||
        rect.width > desc.targetWidth - rect.x || rect.height > desc.targetHeight - rect.y ||
        !colorValid || !depthValid ||
        (desc.clearColor && desc.colorFormat == PixelFormat::Unknown) ||
        (clearsDepthStencil && desc.depthFormat == PixelFormat::Unknown) ||
        (desc.clearStencil && graphics::GetPixelFormatInfo(desc.depthFormat).stencilBits == 0)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto vertex = set.FindEntry(ClearRegionVertexType.hash);
    auto pixel = desc.clearColor
                     ? set.FindEntry(ClearRegionPixelType.hash, shader::NoVertexInputType,
                                     ClearRegionPermutation(valueType))
                     : set.FindEntry(DepthOnlyPixelType.hash);
    if (!vertex || !pixel) {
        return Result<void>::Failure(!vertex ? vertex.Error() : pixel.Error());
    }

    GraphicsPipelineRequest request;
    request.set = &set;
    request.vertexEntry = vertex.Value();
    request.pixelEntry = pixel.Value();
    graphics::PipelineDesc& state = request.state;
    state.colorTargetCount = desc.colorFormat == PixelFormat::Unknown ? 0u : 1u;
    state.colorFormats[0] = desc.colorFormat;
    SetBlend(state, OpaqueBlend());
    if (!desc.clearColor) {
        state.blend[0].writeMask = graphics::ColorWriteMask::None;
    }

    state.raster = NoCull();
    state.depthFormat = desc.depthFormat;
    state.depthStencil = DepthDisabled();
    if (clearsDepthStencil) {
        state.depthStencil.depthTest = true;
        state.depthStencil.depthWrite = desc.clearDepth;
        state.depthStencil.depthCompare = graphics::CompareFunction::Always;
    }

    if (desc.clearStencil) {
        state.depthStencil.stencilTest = true;
        state.depthStencil.stencilWriteMask = FullStencilMask;
        graphics::StencilFace replace;
        replace.compare = graphics::CompareFunction::Always;
        replace.pass = graphics::StencilOperation::Replace;
        replace.depthFail = graphics::StencilOperation::Replace;
        state.depthStencil.front = replace;
        state.depthStencil.back = replace;
    }

    state.drawConstantBytes = sizeof(ClearRegionConstants);
    auto pipeline = cache.FindOrCreate(request);
    if (!pipeline) {
        return Result<void>::Failure(pipeline.Error());
    }

    ClearRegionConstants constants;
    constants.value = {desc.color.words[0], desc.color.words[1], desc.color.words[2],
                       desc.color.words[3]};
    constants.depth.x = desc.depth;
    commands.SetPipeline(pipeline.Value());
    if (auto uploaded = commands.SetDrawConstants(graphics::DrawConstantBytes(constants));
        !uploaded) {
        return uploaded;
    }

    graphics::Viewport viewport;
    viewport.x = static_cast<float>(rect.x);
    viewport.y = static_cast<float>(rect.y);
    viewport.width = static_cast<float>(rect.width);
    viewport.height = static_cast<float>(rect.height);
    commands.SetViewport(viewport);
    commands.SetScissor(rect);
    commands.SetStencilReference(desc.stencil);
    DrawFullscreenTriangle(commands);
    graphics::Viewport full;
    full.width = static_cast<float>(desc.targetWidth);
    full.height = static_cast<float>(desc.targetHeight);
    commands.SetViewport(full);
    commands.SetScissor(graphics::ScissorRect{0, 0, desc.targetWidth, desc.targetHeight});
    commands.SetStencilReference(0);
    return Result<void>::Success();
}

Result<void>
InitializeDispatchArguments(graphics::Device& device, graphics::CommandContext& commands,
                            ShaderSet& set, PipelineCache& cache, graphics::BufferViewHandle view,
                            uint32 byteOffset, uint32 count,
                            const graphics::DispatchIndirectArguments& arguments) noexcept {
    if (byteOffset % DispatchArgumentsAlignment != 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto info = device.GetBufferViewInfo(view);
    if (!info) {
        return Result<void>::Failure(info.Error());
    }

    if (info.Value().kind != graphics::BufferViewKind::Raw ||
        info.Value().access != graphics::ViewAccess::ReadWrite) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (count == 0) {
        return Result<void>::Success();
    }

    auto entry = set.FindEntry(InitializeDispatchArgumentsComputeType.hash);
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    ComputePipelineRequest request;
    request.set = &set;
    request.computeEntry = entry.Value();
    request.state.drawConstantBytes = sizeof(DispatchArgumentsInitConstants);
    request.state.bindings.writableBuffers[0] = DispatchArgumentsTargetName;
    auto pipeline = cache.FindOrCreate(request);
    if (!pipeline) {
        return Result<void>::Failure(pipeline.Error());
    }

    DispatchArgumentsInitConstants constants;
    constants.arguments = {arguments.groupCountX, arguments.groupCountY, arguments.groupCountZ, 0};
    constants.range = {byteOffset, count, 0, 0};
    const ComputeGroupCount groups = GroupCountFor(count, DispatchArgumentsInitGroupThreads);
    if (auto valid = ValidateGroupCount(device.Caps().limits, groups); !valid) {
        return valid;
    }

    commands.SetPipeline(pipeline.Value());
    commands.SetWritableBufferView(0, view);
    if (auto uploaded = commands.SetDrawConstants(graphics::DrawConstantBytes(constants));
        !uploaded) {
        return uploaded;
    }

    commands.Dispatch(groups.x, groups.y, groups.z);
    return Result<void>::Success();
}

} // namespace rocks::render
