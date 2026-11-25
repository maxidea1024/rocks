#include "FormatSelfTest.h"
#include "SandboxShaders.h"

#include <Core/Containers/Array.h>
#include <Platform/Memory.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// 왕복 텍스처의 크기다(BC 블록 4·비디오 2의 배수).
constexpr uint32 Extent = 8;
// view 샘플 시험의 렌더 타깃 크기다(선형 RGBA8).
constexpr uint32 TargetSize = 4;
// 셰이더로 읽은 값과 기대값의 허용 차이(8-bit 단위)다.
constexpr int ColorTolerance = 2;

uint32 failures = 0;
const char* backendName = "";
// OpenGL ES는 색 렌더 타깃이 아닌 형식·블록 압축을 읽지 못한다(ReadTexture가 Unsupported,
// Docs/Design/PixelFormat 14.2절). 그 backend에서만 Unsupported를 허용하고 수를 센다.
bool readLimited = false;
uint32 unreadable = 0;

void Check(bool condition, PixelFormat format, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s %s: %s\n", backendName, PixelFormatName(format), what);
    }
}

// 셰이더는 FullScreen.rsf와 Format.rsf다(출력 계열별 상수 색, 깊이·스텐실 plane 읽기).

// 결정적 바이트 열이다(seed마다 다르다).
void FillBytes(Span<uint8> bytes, uint32 seed) noexcept {
    uint32 state = seed * 2654435761u + 12345u;
    for (usize index = 0; index < bytes.Size(); ++index) {
        state = state * 1664525u + 1013904223u;
        bytes[index] = static_cast<uint8>(state >> 24);
    }
}

// plane의 깊이 형식이다(깊이 plane이 아니면 Unknown).
PixelFormat DepthPlaneFormat(PixelFormat format, uint32 plane) noexcept {
    const PixelFormatInfo& info = GetPixelFormatInfo(format);
    if (info.kind == PixelFormatKind::Depth && plane == 0) {
        return format;
    }
    if (info.kind == PixelFormatKind::DepthStencil && plane == 0) {
        return info.planes[0].format;
    }

    return PixelFormat::Unknown;
}

// mip·plane 하나의 위치다(초기 데이터 배치: mip 0부터, mip 안은 plane 순서).
struct Slice {
    usize offset = 0;
    usize bytes = 0;
    uint32 mip = 0;
    uint32 plane = 0;
};

uint32 Slices(PixelFormat format, uint32 mips, Slice (&out)[8]) noexcept {
    uint32 count = 0;
    usize offset = 0;
    for (uint32 mip = 0; mip < mips; ++mip) {
        for (uint32 plane = 0; plane < PixelFormatPlaneCount(format); ++plane) {
            const usize bytes = static_cast<usize>(PixelFormatPlaneImageBytes(
                format, plane, MipExtent(Extent, mip), MipExtent(Extent, mip)));
            out[count++] = {offset, bytes, mip, plane};
            offset += bytes;
        }
    }

    return count;
}

// 작은 float 하나(exponentBits·mantissaBits, 비트 offset)를 유한한 정규 수 또는 0으로 바꾼다.
uint32 FiniteSmallFloat(uint32 word, uint32 offset, uint32 exponentBits,
                        uint32 mantissaBits) noexcept {
    const uint32 exponentMask = ((1u << exponentBits) - 1u) << (offset + mantissaBits);
    const uint32 mantissaMask = ((1u << mantissaBits) - 1u) << offset;
    const uint32 exponent = (word & exponentMask) >> (offset + mantissaBits);
    if (exponent == (1u << exponentBits) - 1u) {
        word &= ~(1u << (offset + mantissaBits));
    } else if (exponent == 0) {
        word &= ~mantissaMask;
    }

    return word;
}

// float·snorm 표본을 모든 backend가 비트 그대로 왕복하는 값으로 바꾼다. OpenGL ES readback은
// framebuffer 읽기 조합(RGBA/FLOAT·RGBA/BYTE)을 거치므로 NaN·무한대·비정규 수와 SNORM 최솟값(-1을
// 두 번 나타내는 -128·-32768)은 보존되지 않는다.
void SanitizeValues(PixelFormat format, Span<uint8> data) noexcept {
    const PixelFormatInfo& info = GetPixelFormatInfo(format);
    if (info.kind != PixelFormatKind::Color) {
        return;
    }
    if (format == PixelFormat::RG11B10Ufloat) {
        for (usize at = 0; at + 4 <= data.Size(); at += 4) {
            uint32 word = 0;
            platform::MemoryCopy(&word, data.Data() + at, 4);
            word = FiniteSmallFloat(word, 0, 5, 6);
            word = FiniteSmallFloat(word, 11, 5, 6);
            word = FiniteSmallFloat(word, 22, 5, 5);
            platform::MemoryCopy(data.Data() + at, &word, 4);
        }
        return;
    }
    uint32 channels = 0;
    for (uint32 bit = 0; bit < 4; ++bit) {
        channels += (info.channels >> bit) & 1u;
    }
    const uint32 width = channels == 0 ? 0 : info.blockBytes / channels;
    if (info.componentType == PixelComponentType::Float && (width == 2 || width == 4)) {
        for (usize at = 0; at + width <= data.Size(); at += width) {
            uint32 word = 0;
            platform::MemoryCopy(&word, data.Data() + at, width);
            word = width == 2 ? FiniteSmallFloat(word, 0, 5, 10) : FiniteSmallFloat(word, 0, 8, 23);
            platform::MemoryCopy(data.Data() + at, &word, width);
        }
    } else if (info.componentType == PixelComponentType::Snorm && (width == 1 || width == 2)) {
        for (usize at = 0; at + width <= data.Size(); at += width) {
            // 최솟값은 최상위 byte 0x80, 나머지 0이다(little-endian).
            const bool minimum = data[at + width - 1] == 0x80u && (width == 1 || data[at] == 0);
            if (minimum) {
                data[at] = static_cast<uint8>(data[at] | 1u);
            }
        }
    }
}

// 깊이 plane은 장치가 보존하는 값으로 바꾼다: D24는 상위 8비트를 0으로(정의되지 않는 비트), D32F는
// [0, 1]의 유한값으로. P010은 16비트 표본의 하위 6비트를 0으로 둔다(Vulkan G10X6 형식은 그 비트를
// 정의하지 않는다). 색 형식은 SanitizeValues를 따른다.
void SanitizeDepth(PixelFormat format, uint32 mips, Span<uint8> data) noexcept {
    SanitizeValues(format, data);
    if (format == PixelFormat::P010) {
        for (usize index = 0; index + 2 <= data.Size(); index += 2) {
            data[index] = static_cast<uint8>(data[index] & 0xC0u);
        }

        return;
    }

    Slice slices[8];
    const uint32 count = Slices(format, mips, slices);
    for (uint32 index = 0; index < count; ++index) {
        const PixelFormat depth = DepthPlaneFormat(format, slices[index].plane);
        uint8* bytes = data.Data() + slices[index].offset;
        for (usize texel = 0; texel + 4 <= slices[index].bytes; texel += 4) {
            if (depth == PixelFormat::D24Unorm) {
                bytes[texel + 3] = 0;
            } else if (depth == PixelFormat::D32Float) {
                const float value = static_cast<float>((texel / 4) % 1024) / 1023.0f;
                platform::MemoryCopy(bytes + texel, &value, 4);
            }
        }
    }
}

// 읽은 plane이 기대와 같은지다. D24 깊이 plane은 하위 24비트만 비교한다.
bool SameBytes(PixelFormat format, uint32 plane, const uint8* expected, const uint8* actual,
               usize bytes) noexcept {
    const bool d24 = DepthPlaneFormat(format, plane) == PixelFormat::D24Unorm;
    for (usize index = 0; index < bytes; ++index) {
        if (d24 && index % 4 == 3) {
            continue;
        }
        if (expected[index] != actual[index]) {
            printf("    first difference at byte %llu: expected %u, read %u\n",
                   static_cast<unsigned long long>(index), expected[index], actual[index]);
            return false;
        }
    }

    return true;
}

// 모든 mip·plane을 읽어 data와 비교한다. ReadTexture가 그 형식을 읽지 못하면(OpenGL ES만 허용)
// false다.
bool CompareAll(Device& device, TextureHandle texture, PixelFormat format, uint32 mips,
                Span<const uint8> data, Allocator& allocator, const char* what) noexcept {
    Slice slices[8];
    const uint32 count = Slices(format, mips, slices);
    Array<uint8> read(allocator);
    for (uint32 index = 0; index < count; ++index) {
        const Slice& slice = slices[index];
        if (!read.TryResize(slice.bytes)) {
            Check(false, format, "readback buffer allocation");
            return true;
        }
        auto readback = device.ReadTexture(texture, slice.mip, slice.plane, read.View());
        if (!readback && readLimited && readback.Error() == ErrorCode::Unsupported) {
            return false;
        }
        Check(readback.IsOk(), format, "ReadTexture succeeds");
        if (readback) {
            Check(SameBytes(format, slice.plane, data.Data() + slice.offset, read.Data(),
                            slice.bytes),
                  format, what);
        }
    }

    return true;
}

// 업로드·readback 왕복과 UpdateTexture 왕복이다(PF2·PF6).
void RunRoundTrip(Device& device, PixelFormat format, Allocator& allocator) noexcept {
    const PixelFormatCaps caps = device.GetPixelFormatCaps(format);
    if (!HasPixelFormatCaps(caps, PixelFormatCaps::Sampled)) {
        return;
    }
    const uint32 mips =
        HasPixelFormatCaps(caps, PixelFormatCaps::Mipmaps) && PixelFormatPlaneCount(format) == 1
            ? 2
            : 1;
    Array<uint8> data(allocator);
    if (!data.TryResize(static_cast<usize>(MipChainBytes(format, Extent, Extent, 1, mips)))) {
        Check(false, format, "initial data allocation");
        return;
    }
    FillBytes(data.View(), PixelFormatNumber(format));
    SanitizeDepth(format, mips, data.View());
    TextureDesc desc;
    desc.width = Extent;
    desc.height = Extent;
    desc.format = format;
    desc.mipLevels = mips;
    auto texture = device.CreateTexture(desc, Span<const uint8>(data.Data(), data.Size()));
    Check(texture.IsOk(), format, "sampled texture with initial data is created");
    if (!texture) {
        printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
               device.LastError().Data());
        return;
    }

    const bool readable = CompareAll(device, texture.Value(), format, mips, data.View(), allocator,
                                     "initial data reads back unchanged");
    unreadable += readable ? 0u : 1u;
    // mip 0 갱신(UpdateTexture)도 같은 배치로 올라간다.
    Array<uint8> update(allocator);
    if (update.TryResize(static_cast<usize>(PixelFormatImageBytes(format, Extent, Extent)))) {
        FillBytes(update.View(), PixelFormatNumber(format) + 97);
        SanitizeDepth(format, 1, update.View());
        auto updated = device.UpdateTexture(texture.Value(), 0,
                                            Span<const uint8>(update.Data(), update.Size()));
        Check(updated.IsOk(), format, "UpdateTexture succeeds");
        if (updated && readable) {
            CompareAll(device, texture.Value(), format, 1, update.View(), allocator,
                       "updated mip 0 reads back unchanged");
        }
    }
    device.Retire(texture.Value());
}

// 생성 결과가 능력과 맞는지다. 능력이 없으면 Unsupported여야 한다.
void ExpectCreation(bool created, ErrorCode error, bool expected, PixelFormat format,
                    const char* what) noexcept {
    Check(created == expected, format, what);
    if (!created && !expected) {
        Check(error == ErrorCode::Unsupported, format, "missing capability fails as Unsupported");
    }
}

// 형식 × 용도의 "능력 있음 ⇔ 생성 성공"이다(PF3).
void RunCapsMatchCreation(Device& device, PixelFormat format, Allocator& allocator) noexcept {
    const PixelFormatCaps caps = device.GetPixelFormatCaps(format);
    TextureDesc desc;
    desc.width = Extent;
    desc.height = Extent;
    desc.format = format;

    desc.usage = TextureUsage::Sampled;
    Array<uint8> data(allocator);
    const usize bytes = static_cast<usize>(PixelFormatImageBytes(format, Extent, Extent));
    if (data.TryResize(bytes)) {
        FillBytes(data.View(), 7);
        SanitizeDepth(format, 1, data.View());
        auto sampled = device.CreateTexture(desc, Span<const uint8>(data.Data(), data.Size()));
        ExpectCreation(sampled.IsOk(), sampled.IsOk() ? ErrorCode::None : sampled.Error(),
                       HasPixelFormatCaps(caps, RequiredPixelFormatCaps(desc)), format,
                       "Sampled creation matches the Sampled capability");
        if (sampled) {
            device.Retire(sampled.Value());
        }
    }

    desc.usage = TextureUsage::RenderTarget;
    auto target = device.CreateTexture(desc, {});
    ExpectCreation(target.IsOk(), target.IsOk() ? ErrorCode::None : target.Error(),
                   HasPixelFormatCaps(caps, RequiredPixelFormatCaps(desc)), format,
                   "RenderTarget creation matches the RenderTarget capability");
    if (target) {
        device.Retire(target.Value());
    }

    desc.usage = TextureUsage::DepthStencil;
    auto depth = device.CreateTexture(desc, {});
    ExpectCreation(depth.IsOk(), depth.IsOk() ? ErrorCode::None : depth.Error(),
                   HasPixelFormatCaps(caps, RequiredPixelFormatCaps(desc)), format,
                   "DepthStencil creation matches the DepthStencil capability");
    if (depth) {
        device.Retire(depth.Value());
    }

    // 파이프라인 색 형식: 셰이더 출력 계열을 형식에 맞춘다(타입이 다르면 검증 계층 경고).
    const PixelShaderType shader = GetPixelFormatInfo(format).shaderType;
    const shaders::ShaderTypeDesc* pixel = &shaders::FormatFloatColorPS;
    if (shader == PixelShaderType::Int) {
        pixel = &shaders::FormatIntColorPS;
    } else if (shader == PixelShaderType::Uint || shader == PixelShaderType::Uint64) {
        pixel = &shaders::FormatUintColorPS;
    }
    PipelineDesc pipelineDesc;
    pipelineDesc.colorFormats[0] = format;
    auto pipeline =
        shaders::CreateGraphicsPipeline(device, shaders::FullScreenVS, *pixel, pipelineDesc);
    const bool expected = HasPixelFormatCaps(caps, PixelFormatCaps::RenderTarget);
    ExpectCreation(pipeline.IsOk(), pipeline.IsOk() ? ErrorCode::None : pipeline.Error(), expected,
                   format, "pipeline color format matches the RenderTarget capability");
    if (pipeline) {
        device.Retire(pipeline.Value());
    }
}

// view 샘플 시험의 장치 자원이다.
struct ViewBench {
    Device* device = nullptr;
    TextureHandle target;
    PipelineHandle floatLoad;
    PipelineHandle stencilLoad;
    uint8 pixels[TargetSize * TargetSize * 4] = {};

    // view를 묶어 그린 뒤 (1, 1) 픽셀을 읽는다.
    bool Render(TextureHandle view, PipelineHandle pipeline, uint8 (&color)[4]) noexcept {
        auto frame = device->BeginOffscreen(target, {});
        if (!frame) {
            return false;
        }
        frame.Value()->SetPipeline(pipeline);
        frame.Value()->SetTexture(view);
        frame.Value()->Draw(3);
        if (!device->EndFrame() ||
            !device->ReadPixels(target, Span<uint8>(pixels, sizeof(pixels)))) {
            return false;
        }
        platform::MemoryCopy(color, pixels + (TargetSize + 1) * 4, 4);
        return true;
    }
};

bool Near(uint8 actual, int expected) noexcept {
    const int delta = static_cast<int>(actual) - expected;
    return delta <= ColorTolerance && delta >= -ColorTolerance;
}

// 모든 texel이 같은 값인 텍스처를 만든다(plane별 texel 바이트 반복).
Result<TextureHandle> Uniform(Device& device, PixelFormat format, const uint8* planeTexels[2],
                              const uint32 texelBytes[2], Allocator& allocator) noexcept {
    Array<uint8> data(allocator);
    const usize bytes = static_cast<usize>(PixelFormatImageBytes(format, Extent, Extent));
    if (!data.TryResize(bytes)) {
        return Result<TextureHandle>::Failure(ErrorCode::OutOfMemory);
    }
    usize offset = 0;
    for (uint32 plane = 0; plane < PixelFormatPlaneCount(format); ++plane) {
        const usize planeBytes =
            static_cast<usize>(PixelFormatPlaneImageBytes(format, plane, Extent, Extent));
        for (usize at = 0; at < planeBytes; at += texelBytes[plane]) {
            platform::MemoryCopy(data.Data() + offset + at, planeTexels[plane], texelBytes[plane]);
        }
        offset += planeBytes;
    }
    TextureDesc desc;
    desc.width = Extent;
    desc.height = Extent;
    desc.format = format;
    return device.CreateTexture(desc, Span<const uint8>(data.Data(), data.Size()));
}

// sRGB 짝 view: 선형 RGBA8 128을 sRGB로 읽으면 선형 값 0.2158(55)이다. 원 텍스처를 놓아도 view가
// 자원을 붙든다.
void RunSrgbView(ViewBench& bench, Allocator& allocator) noexcept {
    Device& device = *bench.device;
    const PixelFormat format = PixelFormat::RGBA8Unorm;
    const uint8 texel[4] = {128, 128, 128, 255};
    const uint8* texels[2] = {texel, nullptr};
    const uint32 sizes[2] = {4, 0};
    auto texture = Uniform(device, format, texels, sizes, allocator);
    Check(texture.IsOk(), format, "uniform texture is created");
    if (!texture) {
        return;
    }
    auto wrong = device.CreateTextureView(texture.Value(), {PixelFormat::BGRA8Unorm, 0, {}});
    Check(!wrong && wrong.Error() == ErrorCode::InvalidArgument, format,
          "view with a non-pair format is InvalidArgument");
    auto view = device.CreateTextureView(texture.Value(), {PixelFormat::RGBA8UnormSrgb, 0, {}});
    const bool expected = HasPixelFormatCaps(device.GetPixelFormatCaps(PixelFormat::RGBA8UnormSrgb),
                                             PixelFormatCaps::Sampled);
    // OpenGL ES core에는 형식을 바꾸는 texture view가 없어 sRGB 짝 view는 Unsupported다.
    const bool reinterpretLimited = readLimited && !view && view.Error() == ErrorCode::Unsupported;
    Check(view.IsOk() == expected || reinterpretLimited, format,
          "sRGB pair view creation matches the capability");
    uint8 color[4] = {};
    Check(bench.Render(texture.Value(), bench.floatLoad, color) && Near(color[0], 128), format,
          "linear view reads the stored value");
    if (view) {
        device.Retire(texture.Value());
        Check(bench.Render(view.Value(), bench.floatLoad, color) && Near(color[0], 55), format,
              "sRGB pair view decodes to linear after the source is retired");
        device.Retire(view.Value());
    } else {
        device.Retire(texture.Value());
    }
}

// 깊이·스텐실 view: 깊이 plane은 float(0.5 → 128), 스텐실 plane은 uint G 채널(0x5A)이다.
void RunDepthStencilViews(ViewBench& bench, PixelFormat format, Allocator& allocator) noexcept {
    Device& device = *bench.device;
    if (!HasPixelFormatCaps(device.GetPixelFormatCaps(format), PixelFormatCaps::Sampled)) {
        return;
    }
    uint8 depth[4] = {};
    if (GetPixelFormatInfo(format).planes[0].format == PixelFormat::D24Unorm) {
        const uint32 half = 0x800000;
        platform::MemoryCopy(depth, &half, 4);
    } else {
        const float half = 0.5f;
        platform::MemoryCopy(depth, &half, 4);
    }
    const uint8 stencil[1] = {0x5A};
    const uint8* texels[2] = {depth, stencil};
    const uint32 sizes[2] = {4, 1};
    auto texture = Uniform(device, format, texels, sizes, allocator);
    Check(texture.IsOk(), format, "depth stencil texture with data is created");
    if (!texture) {
        return;
    }
    auto depthView = device.CreateTextureView(texture.Value(), {});
    auto stencilView = device.CreateTextureView(texture.Value(), {PixelFormat::Unknown, 1, {}});
    Check(depthView.IsOk() && stencilView.IsOk(), format,
          "depth and stencil plane views are created");
    uint8 color[4] = {};
    if (depthView) {
        Check(bench.Render(depthView.Value(), bench.floatLoad, color) && Near(color[0], 128),
              format, "depth plane view reads the depth value");
        device.Retire(depthView.Value());
    }
    if (stencilView) {
        Check(bench.Render(stencilView.Value(), bench.stencilLoad, color) && Near(color[1], 0x5A),
              format, "stencil plane view reads the stencil value in G");
        device.Retire(stencilView.Value());
    }
    device.Retire(texture.Value());
}

// 비디오 view: Y plane과 UV plane을 따로 읽는다(PF6).
void RunVideoViews(ViewBench& bench, PixelFormat format, Allocator& allocator) noexcept {
    Device& device = *bench.device;
    if (!HasPixelFormatCaps(device.GetPixelFormatCaps(format), PixelFormatCaps::Sampled)) {
        return;
    }
    const bool wide = format == PixelFormat::P010;
    // 8비트: Y 0x40, CbCr (0x80, 0xC0). P010은 같은 값을 16비트 상위에 둔다.
    const uint8 luma8[1] = {0x40};
    const uint8 chroma8[2] = {0x80, 0xC0};
    const uint8 luma16[2] = {0x00, 0x40};
    const uint8 chroma16[4] = {0x00, 0x80, 0x00, 0xC0};
    const uint8* texels[2] = {wide ? luma16 : luma8, wide ? chroma16 : chroma8};
    const uint32 sizes[2] = {wide ? 2u : 1u, wide ? 4u : 2u};
    auto texture = Uniform(device, format, texels, sizes, allocator);
    Check(texture.IsOk(), format, "video texture with both planes is created");
    if (!texture) {
        return;
    }
    auto outside = device.CreateTextureView(texture.Value(), {PixelFormat::Unknown, 2, {}});
    Check(!outside && outside.Error() == ErrorCode::InvalidArgument, format,
          "plane 2 view is InvalidArgument");
    auto luma = device.CreateTextureView(texture.Value(), {});
    auto chroma = device.CreateTextureView(texture.Value(), {PixelFormat::Unknown, 1, {}});
    Check(luma.IsOk() && chroma.IsOk(), format, "Y and UV plane views are created");
    uint8 color[4] = {};
    if (luma) {
        Check(bench.Render(luma.Value(), bench.floatLoad, color) && Near(color[0], 0x40), format,
              "Y plane view reads luma");
        device.Retire(luma.Value());
    }
    if (chroma) {
        Check(bench.Render(chroma.Value(), bench.floatLoad, color) && Near(color[0], 0x80) &&
                  Near(color[1], 0xC0),
              format, "UV plane view reads chroma");
        device.Retire(chroma.Value());
    }
    device.Retire(texture.Value());
}

void RunViewChecks(Device& device, Allocator& allocator) noexcept {
    ViewBench bench;
    bench.device = &device;
    TextureDesc targetDesc;
    targetDesc.width = TargetSize;
    targetDesc.height = TargetSize;
    targetDesc.format = PixelFormat::RGBA8Unorm;
    targetDesc.usage = TextureUsage::RenderTarget;
    auto target = device.CreateTexture(targetDesc, {});
    PipelineDesc pipelineDesc;
    pipelineDesc.colorFormats[0] = PixelFormat::RGBA8Unorm;
    pipelineDesc.bindings.texture = "Depth";
    auto floatLoad = shaders::CreateGraphicsPipeline(device, shaders::FullScreenVS,
                                                     shaders::FormatLoadFloatPS, pipelineDesc);
    pipelineDesc.bindings.texture = "Stencil";
    auto stencilLoad = shaders::CreateGraphicsPipeline(device, shaders::FullScreenVS,
                                                       shaders::FormatLoadStencilPS, pipelineDesc);
    Check(target.IsOk() && floatLoad.IsOk() && stencilLoad.IsOk(), PixelFormat::RGBA8Unorm,
          "view bench resources are created");
    if (!target || !floatLoad || !stencilLoad) {
        printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
               device.LastError().Data());
        return;
    }
    bench.target = target.Value();
    bench.floatLoad = floatLoad.Value();
    bench.stencilLoad = stencilLoad.Value();
    RunSrgbView(bench, allocator);
    RunDepthStencilViews(bench, PixelFormat::D24UnormS8Uint, allocator);
    RunDepthStencilViews(bench, PixelFormat::D32FloatS8Uint, allocator);
    RunVideoViews(bench, PixelFormat::NV12, allocator);
    RunVideoViews(bench, PixelFormat::P010, allocator);
    device.Retire(bench.floatLoad);
    device.Retire(bench.stencilLoad);
    device.Retire(bench.target);
}

// RGBA8 텍스처 하나로 ReadTexture·CreateTextureView 지원을 확인한다.
void Probe(Device& device, Allocator& allocator, bool& readable, bool& views) noexcept {
    const PixelFormat format = PixelFormat::RGBA8Unorm;
    const uint8 texel[4] = {1, 2, 3, 4};
    const uint8* texels[2] = {texel, nullptr};
    const uint32 sizes[2] = {4, 0};
    auto texture = Uniform(device, format, texels, sizes, allocator);
    Check(texture.IsOk(), format, "probe texture is created");
    if (!texture) {
        return;
    }
    Array<uint8> read(allocator);
    if (read.TryResize(static_cast<usize>(PixelFormatImageBytes(format, Extent, Extent)))) {
        auto readback = device.ReadTexture(texture.Value(), 0, 0, read.View());
        readable = readback.IsOk();
        Check(readable || readback.Error() == ErrorCode::Unsupported, format,
              "ReadTexture succeeds or is Unsupported");
    }
    auto view = device.CreateTextureView(texture.Value(), {});
    views = view.IsOk();
    Check(views || view.Error() == ErrorCode::Unsupported, format,
          "CreateTextureView succeeds or is Unsupported");
    if (view) {
        device.Retire(view.Value());
    }
    device.Retire(texture.Value());
}

} // namespace

uint32 RunFormatChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    unreadable = 0;
    backendName = name;
    readLimited = device.Info().backend == Backend::OpenGL;
    // ReadTexture·CreateTextureView를 지원하는 backend에서만 왕복·view 시험을 한다. 지원하지 않으면
    // Unsupported여야 한다(새 virtual의 기본 구현).
    bool readable = false;
    bool views = false;
    Probe(device, allocator, readable, views);
    uint32 sampled = 0;
    uint32 renderTargets = 0;
    uint32 depthTargets = 0;
    uint32 unsupported = 0;
    for (uint32 number = 1; number <= MaxPixelFormatNumber; ++number) {
        const auto format = static_cast<PixelFormat>(number);
        if (!IsValidPixelFormat(format)) {
            continue;
        }
        const PixelFormatCaps caps = device.GetPixelFormatCaps(format);
        sampled += HasPixelFormatCaps(caps, PixelFormatCaps::Sampled) ? 1u : 0u;
        renderTargets += HasPixelFormatCaps(caps, PixelFormatCaps::RenderTarget) ? 1u : 0u;
        depthTargets += HasPixelFormatCaps(caps, PixelFormatCaps::DepthStencil) ? 1u : 0u;
        unsupported += caps == PixelFormatCaps::None ? 1u : 0u;
        RunCapsMatchCreation(device, format, allocator);
        if (readable) {
            RunRoundTrip(device, format, allocator);
        }
    }
    if (views) {
        RunViewChecks(device, allocator);
    }
    auto flushed = device.WaitIdle();
    Check(flushed.IsOk(), PixelFormat::Unknown, "WaitIdle after format checks");
    printf("  %s pixel formats: %u sampled, %u render target, %u depth stencil, %u unsupported, "
           "readback %s (%u formats unreadable), views %s, %u failure%s\n",
           name, sampled, renderTargets, depthTargets, unsupported, readable ? "yes" : "no",
           unreadable, views ? "yes" : "no", failures, failures == 1 ? "" : "s");
    return failures;
}

} // namespace sandbox
