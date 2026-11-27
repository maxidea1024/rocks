#include <Graphics/ShaderBindings.h>
#include <RenderBase/ComputeDispatch.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/GenerateMips.h>
#include <RenderBase/UtilityShaderTypes.h>

// 밉 생성 도우미다(Docs/Design/EngineShaders 14.4절 5항·14.5절 2항).
namespace rocks::render {

namespace {

using graphics::TextureDimension;

// 텍스처 하나가 가질 수 있는 mip 수 상한(한 변 2^31까지)이다. view 배열 크기에 쓴다.
constexpr uint32 MaxGeneratedMips = 32;
// compute 그룹 크기(GenerateMips.rsf와 같다)다.
constexpr uint32 ComputeGroupSide2D = 8;
constexpr uint32 ComputeGroupSide3D = 4;

// compute 경로의 차원 퍼뮤테이션 값이다(받지 않는 차원은 false).
bool ComputeDimensionOf(TextureDimension dimension, GenerateMipsComputeDimension& out) noexcept {
    switch (dimension) {
    case TextureDimension::Texture2D:
        out = GenerateMipsComputeDimension::Texture2D;
        return true;
    case TextureDimension::Texture2DArray:
        out = GenerateMipsComputeDimension::Texture2DArray;
        return true;
    case TextureDimension::Texture3D:
        out = GenerateMipsComputeDimension::Texture3D;
        return true;
    default:
        return false;
    }
}

// raster 경로의 원본 차원 퍼뮤테이션 값이다(받지 않는 차원은 false).
bool PixelDimensionOf(TextureDimension dimension, GenerateMipsPixelDimension& out) noexcept {
    switch (dimension) {
    case TextureDimension::Texture2D:
        out = GenerateMipsPixelDimension::Texture2D;
        return true;
    case TextureDimension::Texture2DArray:
        out = GenerateMipsPixelDimension::Texture2DArray;
        return true;
    case TextureDimension::TextureCube:
        out = GenerateMipsPixelDimension::TextureCube;
        return true;
    case TextureDimension::Texture3D:
        out = GenerateMipsPixelDimension::Texture3D;
        return true;
    default:
        return false;
    }
}

// compute 경로의 storage image 형식이다(float 계열, GLES는 core 형식만 — 셰이더 거르기와 같다).
bool ComputeFormatOf(const graphics::Device& device, graphics::PixelFormat format,
                     StorageImageFormat& out) noexcept {
    const Result<StorageImageFormat> storage = StorageImageFormatOf(format);
    if (!storage || !IsFloatStorageImageFormat(storage.Value())) {
        return false;
    }

    if (device.Info().backend == graphics::Backend::OpenGL &&
        static_cast<uint32>(storage.Value()) >= GlesStorageImageFormatCount) {
        return false;
    }

    out = storage.Value();
    return true;
}

// mip 하나의 Read view 설정이다(모든 slice).
graphics::TextureViewDesc SourceView(uint32 mip) noexcept {
    graphics::TextureViewDesc view;
    view.firstMip = mip;
    view.mipCount = 1;
    return view;
}

// view들을 모두 Retire한다.
void RetireViews(graphics::Device& device, graphics::TextureHandle* views, uint32 count) noexcept {
    for (uint32 index = 0; index < count; ++index) {
        if (!views[index].IsNull()) {
            device.Retire(views[index]);
            views[index] = {};
        }
    }
}

// 원본 mip·대상 mip의 상수다.
GenerateMipsConstants MipConstants(const graphics::Device& device,
                                   const graphics::TextureDesc& desc, uint32 destinationMip,
                                   uint32 slice) noexcept {
    const uint32 sourceMip = destinationMip - 1;
    const bool volume = desc.dimension == TextureDimension::Texture3D;
    GenerateMipsConstants constants;
    constants.source.x = graphics::MipExtent(desc.width, sourceMip);
    constants.source.y = graphics::MipExtent(desc.height, sourceMip);
    constants.source.z = volume ? graphics::MipExtent(desc.depth, sourceMip) : 1u;
    constants.source.w =
        graphics::TextureViewBaseMip(device.Caps().features, SourceView(sourceMip));
    constants.destination.x = graphics::MipExtent(desc.width, destinationMip);
    constants.destination.y = graphics::MipExtent(desc.height, destinationMip);
    constants.destination.z = volume ? graphics::MipExtent(desc.depth, destinationMip) : 1u;
    constants.destination.w = slice;
    return constants;
}

Result<void> GenerateCompute(graphics::Device& device, ShaderSet& set, PipelineCache& cache,
                             graphics::TextureHandle texture,
                             const graphics::TextureDesc& desc) noexcept {
    GenerateMipsComputeDimension dimension = GenerateMipsComputeDimension::Texture2D;
    StorageImageFormat format = StorageImageFormat::Rgba8;
    (void)ComputeDimensionOf(desc.dimension, dimension);
    (void)ComputeFormatOf(device, desc.format, format);
    auto entry = set.FindEntry(GenerateMipsComputeType.hash, shader::NoVertexInputType,
                               GenerateMipsComputePermutation(dimension, format));
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    const shader::ShaderBindingLayout* layout = set.BindingLayout(entry.Value());
    auto sourceSlot = layout != nullptr ? graphics::FindTextureSlot(*layout, MipSourceName)
                                        : Result<uint32>::Failure(ErrorCode::NotFound);
    if (!sourceSlot) {
        return Result<void>::Failure(sourceSlot.Error());
    }

    ComputePipelineRequest request;
    request.set = &set;
    request.computeEntry = entry.Value();
    request.state.drawConstantBytes = sizeof(GenerateMipsConstants);
    request.state.bindings.writableTextures[0] = MipTargetName;
    auto pipeline = cache.FindOrCreate(request);
    if (!pipeline) {
        return Result<void>::Failure(pipeline.Error());
    }

    graphics::TextureHandle views[MaxGeneratedMips * 2] = {};
    uint32 viewCount = 0;
    auto frame = device.BeginPass(graphics::PassDesc{});
    if (!frame) {
        return Result<void>::Failure(frame.Error());
    }

    graphics::CommandContext& commands = *frame.Value();
    commands.SetPipeline(pipeline.Value());
    Result<void> result = Result<void>::Success();
    for (uint32 mip = 1; mip < desc.mipLevels && result; ++mip) {
        graphics::TextureViewDesc target;
        target.firstMip = mip;
        target.mipCount = 1;
        target.access = graphics::ViewAccess::ReadWrite;
        auto read = device.CreateTextureView(texture, SourceView(mip - 1));
        auto write = device.CreateTextureView(texture, target);
        if (read) {
            views[viewCount++] = read.Value();
        }

        if (write) {
            views[viewCount++] = write.Value();
        }

        if (!read || !write) {
            result = Result<void>::Failure(!read ? read.Error() : write.Error());
            break;
        }

        const GenerateMipsConstants constants = MipConstants(device, desc, mip, 0);
        commands.SetTexture(graphics::ShaderStage::Compute, sourceSlot.Value(), read.Value());
        commands.SetWritableTexture(0, write.Value());
        result = commands.SetDrawConstants(graphics::DrawConstantBytes(constants));
        const bool volume = desc.dimension == TextureDimension::Texture3D;
        const ComputeGroupCount groups =
            volume ? GroupCountFor(constants.destination.x, constants.destination.y,
                                   constants.destination.z, ComputeGroupSide3D, ComputeGroupSide3D,
                                   ComputeGroupSide3D)
                   : GroupCountFor(constants.destination.x, constants.destination.y, desc.arraySize,
                                   ComputeGroupSide2D, ComputeGroupSide2D, 1);
        if (result && !groups.IsEmpty()) {
            commands.Dispatch(groups.x, groups.y, groups.z);
        }
    }

    auto ended = device.EndFrame();
    RetireViews(device, views, viewCount);
    if (!result) {
        return result;
    }

    return ended ? Result<void>::Success() : Result<void>::Failure(ended.Error());
}

Result<void> GenerateRaster(graphics::Device& device, ShaderSet& set, PipelineCache& cache,
                            graphics::TextureHandle texture,
                            const graphics::TextureDesc& desc) noexcept {
    GenerateMipsPixelDimension dimension = GenerateMipsPixelDimension::Texture2D;
    (void)PixelDimensionOf(desc.dimension, dimension);
    auto entry = set.FindEntry(GenerateMipsPixelType.hash, shader::NoVertexInputType,
                               GenerateMipsPixelPermutation(dimension));
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    const shader::ShaderBindingLayout* layout = set.BindingLayout(entry.Value());
    auto sourceSlot = layout != nullptr ? graphics::FindTextureSlot(*layout, MipSourceName)
                                        : Result<uint32>::Failure(ErrorCode::NotFound);
    if (!sourceSlot) {
        return Result<void>::Failure(sourceSlot.Error());
    }

    const bool cube = desc.dimension == TextureDimension::TextureCube;
    uint32 samplerSlot = 0;
    if (cube) {
        auto found = graphics::FindSamplerSlot(*layout, "MipSourceSampler");
        if (!found) {
            return Result<void>::Failure(found.Error());
        }

        samplerSlot = found.Value();
    }

    auto request = FullscreenPipelineRequest(set, entry.Value(), desc.format);
    if (!request) {
        return Result<void>::Failure(request.Error());
    }

    request.Value().state.drawConstantBytes = sizeof(GenerateMipsConstants);
    auto pipeline = cache.FindOrCreate(request.Value());
    if (!pipeline) {
        return Result<void>::Failure(pipeline.Error());
    }

    // cube 원본은 texel 중심 방향을 point로 표본한다(그 texel 하나를 고른다).
    graphics::SamplerDesc point;
    point.filter = graphics::Filter::Point;
    point.addressU = graphics::AddressMode::Clamp;
    point.addressV = graphics::AddressMode::Clamp;
    point.addressW = graphics::AddressMode::Clamp;
    for (uint32 mip = 1; mip < desc.mipLevels; ++mip) {
        auto read = device.CreateTextureView(texture, SourceView(mip - 1));
        if (!read) {
            return Result<void>::Failure(read.Error());
        }

        const uint32 slices = desc.dimension == TextureDimension::Texture3D
                                  ? graphics::MipExtent(desc.depth, mip)
                                  : graphics::TextureSliceCount(desc);
        Result<void> result = Result<void>::Success();
        for (uint32 slice = 0; slice < slices && result; ++slice) {
            graphics::PassDesc pass;
            pass.colorCount = 1;
            pass.colors[0].texture = texture;
            pass.colors[0].load = graphics::LoadAction::DontCare;
            pass.colors[0].mipLevel = mip;
            pass.colors[0].slice = slice;
            auto frame = device.BeginPass(pass);
            if (!frame) {
                result = Result<void>::Failure(frame.Error());
                break;
            }

            graphics::CommandContext& commands = *frame.Value();
            const GenerateMipsConstants constants = MipConstants(device, desc, mip, slice);
            commands.SetPipeline(pipeline.Value());
            commands.SetTexture(graphics::ShaderStage::Pixel, sourceSlot.Value(), read.Value());
            if (cube) {
                result = commands.SetSampler(graphics::ShaderStage::Pixel, samplerSlot, &point);
            }

            if (result) {
                result = commands.SetDrawConstants(graphics::DrawConstantBytes(constants));
            }

            if (result) {
                DrawFullscreenTriangle(commands);
            }

            auto ended = device.EndFrame();
            if (result && !ended) {
                result = Result<void>::Failure(ended.Error());
            }
        }

        device.Retire(read.Value());
        if (!result) {
            return result;
        }
    }

    return Result<void>::Success();
}

} // namespace

bool CanGenerateMips(const graphics::Device& device, const graphics::TextureDesc& desc,
                     MipGenerationPath path) noexcept {
    if (desc.mipLevels < 2 || desc.mipLevels > MaxGeneratedMips || desc.sampleCount != 1) {
        return false;
    }

    const graphics::PixelFormatCaps caps = device.GetPixelFormatCaps(desc.format);
    if (path == MipGenerationPath::Compute) {
        GenerateMipsComputeDimension dimension = GenerateMipsComputeDimension::Texture2D;
        StorageImageFormat format = StorageImageFormat::Rgba8;
        return graphics::HasTextureUsage(desc.usage, graphics::TextureUsage::Storage) &&
               ComputeDimensionOf(desc.dimension, dimension) &&
               ComputeFormatOf(device, desc.format, format) &&
               graphics::HasPixelFormatCaps(caps, graphics::PixelFormatCaps::Sampled |
                                                      graphics::PixelFormatCaps::StorageWrite);
    }

    GenerateMipsPixelDimension dimension = GenerateMipsPixelDimension::Texture2D;
    return graphics::HasTextureUsage(desc.usage, graphics::TextureUsage::RenderTarget) &&
           PixelDimensionOf(desc.dimension, dimension) &&
           graphics::HasPixelFormatCaps(caps, graphics::PixelFormatCaps::Sampled |
                                                  graphics::PixelFormatCaps::RenderTarget);
}

Result<MipGenerationPath> SelectMipGenerationPath(const graphics::Device& device,
                                                  const graphics::TextureDesc& desc) noexcept {
    if (desc.mipLevels < 2 || desc.sampleCount != 1) {
        return Result<MipGenerationPath>::Failure(ErrorCode::InvalidArgument);
    }

    if (CanGenerateMips(device, desc, MipGenerationPath::Compute)) {
        return Result<MipGenerationPath>::Success(MipGenerationPath::Compute);
    }

    if (CanGenerateMips(device, desc, MipGenerationPath::Raster)) {
        return Result<MipGenerationPath>::Success(MipGenerationPath::Raster);
    }

    return Result<MipGenerationPath>::Failure(ErrorCode::Unsupported);
}

Result<void> GenerateMips(graphics::Device& device, ShaderSet& set, PipelineCache& cache,
                          graphics::TextureHandle texture, const graphics::TextureDesc& desc,
                          MipGenerationPath path) noexcept {
    if (desc.mipLevels < 2) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!CanGenerateMips(device, desc, path)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return path == MipGenerationPath::Compute ? GenerateCompute(device, set, cache, texture, desc)
                                              : GenerateRaster(device, set, cache, texture, desc);
}

Result<void> GenerateMips(graphics::Device& device, ShaderSet& set, PipelineCache& cache,
                          graphics::TextureHandle texture,
                          const graphics::TextureDesc& desc) noexcept {
    auto path = SelectMipGenerationPath(device, desc);
    if (!path) {
        return Result<void>::Failure(path.Error());
    }

    return GenerateMips(device, set, cache, texture, desc, path.Value());
}

} // namespace rocks::render
