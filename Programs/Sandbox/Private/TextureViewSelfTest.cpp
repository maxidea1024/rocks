#include "TextureViewSelfTest.h"
#include "SandboxShaders.h"

#include <Core/Containers/Array.h>
#include <Platform/Memory.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

constexpr uint32 PyramidExtent = hzb::Extent;
constexpr uint32 PyramidMips = hzb::Mips;
constexpr uint32 ReduceGroup = hzb::ReduceGroup;
using hzb::LevelConstants;
// PS가 읽은 값을 색으로 낼 때 나누는 수다(mip 0의 최댓값 63 + 1).
constexpr float ValueScale = 64.0f;
// 셰이더로 읽은 값과 기대값의 허용 차이(8-bit 단위)다.
constexpr int ColorTolerance = 2;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s texture views: %s\n", backendName, what);
    }
}

void PrintLastError(Device& device) noexcept {
    printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
           device.LastError().Data());
}

template <usize Count>
StringView Text(const char (&text)[Count]) noexcept {
    return StringView(text, Count - 1);
}

bool Near(uint8 actual, int expected) noexcept {
    const int delta = static_cast<int>(actual) - expected;
    return delta <= ColorTolerance && delta >= -ColorTolerance;
}

// 셰이더는 FullScreen.rsf·TextureView.rsf다.

// mip level의 CPU 기댓값이다: mip 0은 x + y × 8, 위 mip은 아래 mip 2×2의 최댓값(= 오른쪽 아래
// texel).
float Expected(uint32 level, uint32 x, uint32 y) noexcept {
    const uint32 span = 1u << level;
    const uint32 right = x * span + span - 1;
    const uint32 bottom = y * span + span - 1;
    return static_cast<float>(right + bottom * PyramidExtent);
}

// 1×1 RGBA8 렌더 타깃에 full-screen 삼각형 하나를 그리고 그 픽셀을 읽는다.
struct Canvas {
    Device& device;
    TextureHandle target;
    uint8 pixel[4] = {};

    explicit Canvas(Device& owner) noexcept : device(owner) {
    }

    bool Initialize() noexcept {
        TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.format = PixelFormat::RGBA8Unorm;
        desc.usage = TextureUsage::RenderTarget;
        auto created = device.CreateTexture(desc, {});
        if (created) {
            target = created.Value();
        }

        return created.IsOk();
    }

    // 마지막 Pipeline이 만든 파이프라인의 텍스처 slot이다.
    uint32 textureSlot = MaxResourceSlots;

    Result<PipelineHandle> Pipeline(const shaders::ShaderTypeDesc& pixel, const char* texture,
                                    uint32 constants) noexcept {
        PipelineDesc desc;
        desc.colorFormats[0] = PixelFormat::RGBA8Unorm;
        desc.drawConstantBytes = constants;
        // 다음 Draw가 쓰는 텍스처 slot이다(만든 직후 그린다).
        textureSlot = shaders::TextureSlot(pixel, texture);
        return shaders::CreateGraphicsPipeline(device, shaders::FullScreenVS, pixel, desc);
    }

    // texture를 묶고 그려 pixel에 읽는다. constants가 있으면 그리기별 상수로 올린다.
    bool Draw(PipelineHandle pipeline, TextureHandle texture,
              const LevelConstants* constants) noexcept {
        auto frame = device.BeginOffscreen(target, {});
        if (!frame) {
            return false;
        }

        CommandContext& commands = *frame.Value();
        commands.SetPipeline(pipeline);
        commands.SetTexture(ShaderStage::Pixel, textureSlot, texture);
        bool set = true;
        if (constants != nullptr) {
            set = commands.SetDrawConstants(DrawConstantBytes(*constants)).IsOk();
        }

        commands.Draw(3);
        return device.EndFrame().IsOk() && set &&
               device.ReadPixels(target, Span<uint8>(pixel, sizeof(pixel))).IsOk();
    }

    void Release() noexcept {
        if (!target.IsNull()) {
            device.Retire(target);
        }
    }
};

TextureViewDesc MipView(uint32 mip, ViewAccess access) noexcept {
    TextureViewDesc desc;
    desc.firstMip = mip;
    desc.mipCount = 1;
    desc.access = access;
    return desc;
}

// mip 0을 채우고 mip k → k + 1로 줄인 뒤 모든 mip을 읽어 대조한다. pyramid는 만든 Storage
// 텍스처다.
void RunPyramid(Device& device, Allocator& allocator, TextureHandle pyramid) noexcept {
    auto fill = hzb::CreateFillPipeline(device);
    auto reduce = hzb::CreateReducePipeline(device);
    Check(fill && reduce, "fill and reduce compute pipelines");
    if (!fill || !reduce) {
        PrintLastError(device);
        return;
    }

    TextureHandle reads[PyramidMips] = {};
    TextureHandle writes[PyramidMips] = {};
    bool views = true;
    for (uint32 mip = 0; mip < PyramidMips; ++mip) {
        auto read = device.CreateTextureView(pyramid, MipView(mip, ViewAccess::Read));
        auto write = device.CreateTextureView(pyramid, MipView(mip, ViewAccess::ReadWrite));
        views = views && read && write;
        if (read) {
            reads[mip] = read.Value();
        }

        if (write) {
            writes[mip] = write.Value();
        }
    }

    Check(views, "read and write views of every mip");
    if (!views) {
        PrintLastError(device);
    }

    const DeviceFeatures& features = device.Caps().features;
    const uint64 ignoredBefore = device.IgnoredDraws();
    auto frame = device.BeginPass(PassDesc{});
    if (frame && views) {
        CommandContext& commands = *frame.Value();
        // 쓰기 텍스처를 묶지 않은 dispatch는 무시된다.
        commands.SetPipeline(fill.Value());
        commands.Dispatch(1, 1);
        commands.SetWritableTexture(0, writes[0]);
        commands.Dispatch(1, 1);
        commands.SetPipeline(reduce.Value());
        for (uint32 mip = 0; mip + 1 < PyramidMips; ++mip) {
            const uint32 size = PyramidExtent >> (mip + 1);
            LevelConstants level;
            level.values[0] =
                static_cast<float>(TextureViewBaseMip(features, MipView(mip, ViewAccess::Read)));
            level.values[1] = static_cast<float>(size);
            commands.SetTexture(ShaderStage::Compute,
                                shaders::TextureSlot(shaders::TextureViewReduceCS, "Source"),
                                reads[mip]);
            commands.SetWritableTexture(0, writes[mip + 1]);
            Check(commands.SetDrawConstants(DrawConstantBytes(level)).IsOk(),
                  "reduce draw constants are accepted");
            commands.Dispatch((size + ReduceGroup - 1) / ReduceGroup,
                              (size + ReduceGroup - 1) / ReduceGroup);
        }
    }

    Check(frame && device.EndFrame(), "pyramid pass submits");
    Check(device.IgnoredDraws() - ignoredBefore == 1,
          "a dispatch without its writable texture is ignored");
    Check(hzb::CountMismatchedMips(device, allocator, pyramid) == 0,
          "every mip of the reduced pyramid matches the CPU reference");

    // PS가 mip 범위 view(mip 2부터)를 view 기준 mip으로 읽는다. mip 2 (0, 0)은 27이다.
    Canvas canvas(device);
    auto levelPipeline =
        canvas.Initialize()
            ? canvas.Pipeline(shaders::TextureViewLevelPS, "Source", sizeof(LevelConstants))
            : Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
    TextureViewDesc upper;
    upper.firstMip = 2;
    auto upperView = device.CreateTextureView(pyramid, upper);
    Check(levelPipeline && upperView, "mip range view and level pipeline");
    if (levelPipeline && upperView) {
        LevelConstants level;
        level.values[0] = static_cast<float>(TextureViewBaseMip(features, upper));
        const int expected = static_cast<int>(Expected(2, 0, 0) / ValueScale * 255.0f + 0.5f);
        Check(canvas.Draw(levelPipeline.Value(), upperView.Value(), &level) &&
                  Near(canvas.pixel[0], expected),
              "a pixel shader reads the first mip of a mip range view");
        if (!Near(canvas.pixel[0], expected)) {
            printf("    red %u, expected %d\n", canvas.pixel[0], expected);
        }
    } else {
        PrintLastError(device);
    }

    if (upperView) {
        device.Retire(upperView.Value());
    }

    if (levelPipeline) {
        device.Retire(levelPipeline.Value());
    }

    canvas.Release();
    for (uint32 mip = 0; mip < PyramidMips; ++mip) {
        if (!reads[mip].IsNull()) {
            device.Retire(reads[mip]);
        }

        if (!writes[mip].IsNull()) {
            device.Retire(writes[mip]);
        }
    }

    device.Retire(fill.Value());
    device.Retire(reduce.Value());
}

// 2D 배열 slice 범위 view: 범위를 지원하면 층 1(초록)을 view의 층 0으로 읽는다.
void RunLayerView(Device& device) noexcept {
    TextureDesc desc;
    desc.dimension = TextureDimension::Texture2DArray;
    desc.width = 1;
    desc.height = 1;
    desc.arraySize = 2;
    desc.format = PixelFormat::RGBA8Unorm;
    const uint8 layers[8] = {255, 0, 0, 255, 0, 255, 0, 255};
    auto texture = device.CreateTexture(desc, Span<const uint8>(layers, sizeof(layers)));
    Check(texture.IsOk(), "two layer array texture");
    if (!texture) {
        PrintLastError(device);
        return;
    }

    TextureViewDesc second;
    second.firstSlice = 1;
    second.sliceCount = 1;
    auto view = device.CreateTextureView(texture.Value(), second);
    if (!device.Caps().features.textureViewRanges) {
        Check(view.Error() == ErrorCode::Unsupported,
              "a partial slice view is Unsupported without texture view ranges");
    } else {
        Canvas canvas(device);
        auto pipeline = canvas.Initialize()
                            ? canvas.Pipeline(shaders::TextureViewLayerPS, "Layers", 0)
                            : Result<PipelineHandle>::Failure(ErrorCode::InvalidArgument);
        Check(view && pipeline && canvas.Draw(pipeline.Value(), view.Value(), nullptr) &&
                  Near(canvas.pixel[0], 0) && Near(canvas.pixel[1], 255),
              "a slice range view reads its first layer");
        if (pipeline) {
            device.Retire(pipeline.Value());
        }

        canvas.Release();
    }

    if (view) {
        device.Retire(view.Value());
    }

    device.Retire(texture.Value());
}

} // namespace

namespace hzb {

TextureDesc PyramidDesc() noexcept {
    TextureDesc desc;
    desc.width = Extent;
    desc.height = Extent;
    desc.format = PixelFormat::R32Float;
    desc.usage = TextureUsage::Storage;
    desc.mipLevels = Mips;
    desc.sampler.filter = Filter::Point;
    return desc;
}

Result<PipelineHandle> CreateFillPipeline(Device& device) noexcept {
    ComputePipelineDesc desc;
    desc.bindings.writableTextures[0] = "Target";
    return shaders::CreateComputePipeline(device, shaders::TextureViewFillCS, desc);
}

Result<PipelineHandle> CreateReducePipeline(Device& device) noexcept {
    ComputePipelineDesc desc;
    desc.drawConstantBytes = sizeof(LevelConstants);
    desc.bindings.writableTextures[0] = "Target";
    return shaders::CreateComputePipeline(device, shaders::TextureViewReduceCS, desc);
}

uint32 CountMismatchedMips(Device& device, Allocator& allocator, TextureHandle pyramid) noexcept {
    uint32 mismatched = 0;
    for (uint32 mip = 0; mip < Mips; ++mip) {
        const uint32 size = Extent >> mip;
        Array<uint8> bytes(allocator);
        const bool read = bytes.TryResize(static_cast<usize>(size) * size * sizeof(float)).IsOk() &&
                          device.ReadTexture(pyramid, mip, 0, bytes.View()).IsOk();
        bool same = read;
        if (!read) {
            printf("    mip %u could not be read\n", mip);
        }

        for (uint32 y = 0; same && y < size; ++y) {
            for (uint32 x = 0; same && x < size; ++x) {
                float value = 0.0f;
                platform::MemoryCopy(&value, bytes.Data() + (y * size + x) * sizeof(float),
                                     sizeof(float));
                same = value == Expected(mip, x, y);
                if (!same) {
                    printf("    mip %u (%u, %u) = %f, expected %f\n", mip, x, y,
                           static_cast<double>(value), static_cast<double>(Expected(mip, x, y)));
                }
            }
        }

        mismatched += same ? 0u : 1u;
    }

    return mismatched;
}

} // namespace hzb

uint32 RunTextureViewChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    const uint64 validationBefore = device.ValidationErrorCount();
    const TextureDesc desc = hzb::PyramidDesc();
    const bool capable =
        HasPixelFormatCaps(device.GetPixelFormatCaps(desc.format), RequiredPixelFormatCaps(desc));
    const bool compute = device.Caps().features.computeShaders;
    auto pyramid = device.CreateTexture(desc, {});
    if (!capable) {
        Check(pyramid.Error() == ErrorCode::Unsupported,
              "Storage without Sampled, StorageWrite and Mipmaps is Unsupported");
    } else {
        Check(pyramid.IsOk(), "Storage R32Float mip chain");
        if (!pyramid) {
            PrintLastError(device);
        }
    }

    if (pyramid) {
        // 규칙 위반: 쓰기 view의 mip 둘, 범위 밖.
        TextureViewDesc twoMips;
        twoMips.access = ViewAccess::ReadWrite;
        twoMips.mipCount = 2;
        TextureViewDesc outside;
        outside.firstMip = PyramidMips;
        Check(device.CreateTextureView(pyramid.Value(), twoMips).Error() ==
                      ErrorCode::InvalidArgument &&
                  device.CreateTextureView(pyramid.Value(), outside).Error() ==
                      ErrorCode::InvalidArgument,
              "writable views over two mips and views outside the texture are rejected");
        if (compute) {
            RunPyramid(device, allocator, pyramid.Value());
        }

        device.Retire(pyramid.Value());
    }

    // Sampled 텍스처의 쓰기 view는 규칙 위반이다.
    TextureDesc sampledDesc;
    sampledDesc.width = 1;
    sampledDesc.height = 1;
    sampledDesc.format = PixelFormat::RGBA8Unorm;
    const uint8 white[4] = {255, 255, 255, 255};
    auto sampled = device.CreateTexture(sampledDesc, Span<const uint8>(white, sizeof(white)));
    if (sampled) {
        TextureViewDesc write;
        write.access = ViewAccess::ReadWrite;
        Check(device.CreateTextureView(sampled.Value(), write).Error() ==
                  ErrorCode::InvalidArgument,
              "a writable view of a Sampled texture is rejected");
        device.Retire(sampled.Value());
    }

    RunLayerView(device);
    auto idle = device.WaitIdle();
    Check(idle.IsOk(), "wait idle");
    Check(device.ValidationErrorCount() == validationBefore, "no validation errors");
    printf("  %s texture views: storage pyramid %s, ranges %s, %u failures\n", name,
           capable && compute ? "yes" : "no",
           device.Caps().features.textureViewRanges ? "yes" : "no (whole texture)", failures);
    return failures;
}

} // namespace sandbox
