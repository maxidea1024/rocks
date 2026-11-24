#include "TextureSelfTest.h"
#include "SandboxShaders.h"

#include <Core/Containers/Array.h>
#include <Platform/Memory.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;

// 셰이더로 읽은 값과 기대값의 허용 차이(8-bit 단위)다.
constexpr int ColorTolerance = 2;
// 차원 시험 텍스처의 한 변이다(mip 1은 1×1이 아니게 4).
constexpr uint32 Extent = 4;

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s textures: %s\n", backendName, what);
    }
}

void PrintLastError(Device& device) noexcept {
    printf("    %.*s\n", static_cast<int>(device.LastError().ByteCount()),
           device.LastError().Data());
}

bool Near(uint8 actual, int expected) noexcept {
    const int delta = static_cast<int>(actual) - expected;
    return delta <= ColorTolerance && delta >= -ColorTolerance;
}

bool NearColor(const uint8* actual, int red, int green, int blue, int alpha) noexcept {
    return Near(actual[0], red) && Near(actual[1], green) && Near(actual[2], blue) &&
           Near(actual[3], alpha);
}

// 결정적 바이트 열이다(seed마다 다르다).
void FillBytes(Span<uint8> bytes, uint32 seed) noexcept {
    uint32 state = seed * 2654435761u + 12345u;
    for (usize index = 0; index < bytes.Size(); ++index) {
        state = state * 1664525u + 1013904223u;
        bytes[index] = static_cast<uint8>(state >> 24);
    }
}

// --- 셰이더 ---
// 정점 버퍼 없이 화면을 덮는 삼각형이다. 픽셀 셰이더는 대상의 열(x)로 읽을 자리를 고른다.

// 셰이더는 FullScreen.rsf·Texture.rsf다(텍스처 이름은 파이프라인 이름 표 bindings.texture).

// 대상 하나와 파이프라인 하나로 텍스처를 그려 대상 픽셀을 읽는다(대상은 columns × 1 RGBA8Unorm).
struct Bench {
    Device* device = nullptr;
    TextureHandle target;
    PipelineHandle pipeline;
    uint32 columns = 0;
    uint8 pixels[16 * 4] = {};

    bool Initialize(Device& owner, uint32 width, const shaders::ShaderTypeDesc& pixel,
                    const char* texture) noexcept {
        device = &owner;
        columns = width;
        TextureDesc desc;
        desc.width = width;
        desc.height = 1;
        desc.format = PixelFormat::RGBA8Unorm;
        desc.usage = TextureUsage::RenderTarget;
        auto created = owner.CreateTexture(desc, {});
        PipelineDesc pipelineDesc;
        pipelineDesc.bindings.texture = texture;
        pipelineDesc.colorFormats[0] = PixelFormat::RGBA8Unorm;
        auto made =
            shaders::CreateGraphicsPipeline(owner, shaders::FullScreenVS, pixel, pipelineDesc);
        if (created) {
            target = created.Value();
        }
        if (made) {
            pipeline = made.Value();
        }
        if (!created || !made) {
            PrintLastError(owner);
        }
        return created.IsOk() && made.IsOk();
    }

    // texture를 묶어 그리고 대상 전체를 pixels에 읽는다. before가 비어 있지 않으면 그리기 전에 그
    // 전이 목록을 낸다.
    bool Render(TextureHandle texture, Span<const ResourceTransition> before = {}) noexcept {
        auto frame = device->BeginOffscreen(target, {});
        if (!frame) {
            return false;
        }
        if (!before.IsEmpty()) {
            frame.Value()->Transition(before);
        }
        frame.Value()->SetPipeline(pipeline);
        frame.Value()->SetTexture(texture);
        frame.Value()->Draw(3);
        return device->EndFrame().IsOk() &&
               device->ReadPixels(target, Span<uint8>(pixels, columns * 4)).IsOk();
    }

    const uint8* Pixel(uint32 column) const noexcept {
        return pixels + column * 4;
    }

    void Release() noexcept {
        if (device == nullptr) {
            return;
        }
        if (!pipeline.IsNull()) {
            device->Retire(pipeline);
        }
        if (!target.IsNull()) {
            device->Retire(target);
        }
    }
};

// 차원 시험 RGBA8 텍스처 설정이다.
TextureDesc ColorDesc(TextureDimension dimension, uint32 arraySize, uint32 depth,
                      uint32 mipLevels) noexcept {
    TextureDesc desc;
    desc.dimension = dimension;
    desc.width = Extent;
    desc.height = Extent;
    desc.depth = depth;
    desc.arraySize = arraySize;
    desc.format = PixelFormat::RGBA8Unorm;
    desc.mipLevels = mipLevels;
    desc.sampler.filter = Filter::Point;
    desc.sampler.addressU = AddressMode::Clamp;
    desc.sampler.addressV = AddressMode::Clamp;
    desc.sampler.addressW = AddressMode::Clamp;
    return desc;
}

// 모든 mip을 ReadTexture로 읽어 data(초기 픽셀 배치)와 비교하고, mip 1을 UpdateTexture로 바꾼 뒤
// 다시 읽는다.
void RunRoundTrip(Device& device, TextureHandle texture, const TextureDesc& desc,
                  Span<const uint8> data, const char* label, Allocator& allocator) noexcept {
    char what[160];
    usize offset = 0;
    for (uint32 mip = 0; mip < desc.mipLevels; ++mip) {
        const usize bytes = static_cast<usize>(TextureMipBytes(desc, mip));
        Array<uint8> read(allocator);
        if (!read.TryResize(bytes)) {
            Check(false, "readback allocation");
            return;
        }
        auto readback = device.ReadTexture(texture, mip, 0, read.View());
        snprintf(what, sizeof(what), "%s mip %u reads back the uploaded bytes", label, mip);
        Check(readback.IsOk() &&
                  platform::MemoryCompare(read.Data(), data.Data() + offset, bytes) == 0,
              what);
        if (!readback) {
            PrintLastError(device);
        }
        offset += bytes;
    }

    if (desc.mipLevels < 2) {
        return;
    }
    const usize bytes = static_cast<usize>(TextureMipBytes(desc, 1));
    Array<uint8> update(allocator);
    Array<uint8> read(allocator);
    if (!update.TryResize(bytes) || !read.TryResize(bytes)) {
        Check(false, "update allocation");
        return;
    }
    FillBytes(update.View(), 91);
    auto updated = device.UpdateTexture(texture, 1, update.View());
    auto wrong = device.UpdateTexture(texture, 1, update.View().Slice(0, bytes - 4));
    auto readback = device.ReadTexture(texture, 1, 0, read.View());
    snprintf(what, sizeof(what), "%s mip 1 update (all slices) reads back", label);
    Check(updated.IsOk() && readback.IsOk() &&
              platform::MemoryCompare(read.Data(), update.Data(), bytes) == 0,
          what);
    snprintf(what, sizeof(what), "%s update of one slice size is InvalidArgument", label);
    Check(!wrong && wrong.Error() == ErrorCode::InvalidArgument, what);
}

// 2D 배열: 왕복, layer load, slice 범위 전이 뒤 그리기.
void RunArray(Device& device, Allocator& allocator) noexcept {
    constexpr uint32 Layers = 3;
    const TextureDesc desc = ColorDesc(TextureDimension::Texture2DArray, Layers, 1, 2);
    Array<uint8> data(allocator);
    if (!data.TryResize(static_cast<usize>(TextureDataBytes(desc)))) {
        Check(false, "array data allocation");
        return;
    }
    FillBytes(data.View(), 3);
    // mip 0의 layer마다 texel (1, 1)을 알려진 색으로 둔다(셰이더 시험).
    const usize image = static_cast<usize>(PixelFormatImageBytes(desc.format, Extent, Extent));
    for (uint32 layer = 0; layer < Layers; ++layer) {
        uint8* texel = data.Data() + layer * image + (Extent + 1) * 4;
        texel[0] = static_cast<uint8>(40 + layer * 60);
        texel[1] = static_cast<uint8>(200 - layer * 50);
        texel[2] = 17;
        texel[3] = 255;
    }
    auto texture = device.CreateTexture(desc, data.View());
    Check(texture.IsOk(), "2D array texture is created");
    if (!texture) {
        PrintLastError(device);
        return;
    }
    RunRoundTrip(device, texture.Value(), desc, data.View(), "2D array", allocator);

    // mip 1은 위 갱신으로 바뀌었으므로 mip 0만 셰이더로 본다.
    Bench bench;
    if (bench.Initialize(device, Layers, shaders::TextureArrayPS, "Layers")) {
        Check(bench.Render(texture.Value()), "2D array draw");
        for (uint32 layer = 0; layer < Layers; ++layer) {
            Check(NearColor(bench.Pixel(layer), 40 + static_cast<int>(layer) * 60,
                            200 - static_cast<int>(layer) * 50, 17, 255),
                  "2D array layer load reads that layer");
        }

        // layer 1만 복사 원본으로 옮겼다가(slice 범위 전이) 셰이더 읽기로 되돌려도 같은 값이다.
        const uint64 validationBefore = device.ValidationErrorCount();
        const ResourceTransition transitions[2] = {{texture.Value(),
                                                    {},
                                                    {0, RemainingSubresources, 1, 1},
                                                    ResourceAccess::SrvGraphics,
                                                    ResourceAccess::CopySource},
                                                   {texture.Value(),
                                                    {},
                                                    {0, RemainingSubresources, 1, 1},
                                                    ResourceAccess::CopySource,
                                                    ResourceAccess::SrvGraphics}};
        Check(bench.Render(texture.Value(), Span<const ResourceTransition>(transitions, 2)) &&
                  NearColor(bench.Pixel(1), 100, 150, 17, 255),
              "2D array draw after a single-slice transition round trip");
        Check(device.ValidationErrorCount() == validationBefore,
              "single-slice transitions keep the tracked state of the other slices");
        bench.Release();
    }
    device.Retire(texture.Value());
}

// 3D: 왕복(깊이도 mip마다 준다), 깊이 면 load, W축 주소 모드.
void RunVolume(Device& device, Allocator& allocator) noexcept {
    if (!HasPixelFormatCaps(device.GetPixelFormatCaps(PixelFormat::RGBA8Unorm),
                            PixelFormatCaps::Texture3D)) {
        TextureDesc desc = ColorDesc(TextureDimension::Texture3D, 1, Extent, 1);
        Array<uint8> data(allocator);
        if (data.TryResize(static_cast<usize>(TextureDataBytes(desc)))) {
            auto texture = device.CreateTexture(desc, data.View());
            Check(!texture && texture.Error() == ErrorCode::Unsupported,
                  "3D without the Texture3D capability is Unsupported");
        }
        return;
    }

    const TextureDesc desc = ColorDesc(TextureDimension::Texture3D, 1, Extent, 3);
    Array<uint8> data(allocator);
    if (!data.TryResize(static_cast<usize>(TextureDataBytes(desc)))) {
        Check(false, "volume data allocation");
        return;
    }
    FillBytes(data.View(), 5);
    const usize image = static_cast<usize>(PixelFormatImageBytes(desc.format, Extent, Extent));
    for (uint32 z = 0; z < Extent; ++z) {
        uint8* texel = data.Data() + z * image + (Extent + 1) * 4;
        texel[0] = static_cast<uint8>(30 + z * 50);
        texel[1] = 90;
        texel[2] = static_cast<uint8>(250 - z * 40);
        texel[3] = 255;
    }
    auto texture = device.CreateTexture(desc, data.View());
    Check(texture.IsOk(), "3D texture is created");
    if (!texture) {
        PrintLastError(device);
        return;
    }
    RunRoundTrip(device, texture.Value(), desc, data.View(), "3D", allocator);
    Bench bench;
    if (bench.Initialize(device, Extent, shaders::TextureVolumePS, "Volume")) {
        Check(bench.Render(texture.Value()), "3D draw");
        for (uint32 z = 0; z < Extent; ++z) {
            Check(NearColor(bench.Pixel(z), 30 + static_cast<int>(z) * 50, 90,
                            250 - static_cast<int>(z) * 40, 255),
                  "3D load reads that depth slice");
        }
        bench.Release();
    }
    device.Retire(texture.Value());

    // W축: 1×1×2(면 0 빨강, 면 1 초록)를 w = 1.25로 표본한다.
    TextureDesc slab = ColorDesc(TextureDimension::Texture3D, 1, 2, 1);
    slab.width = 1;
    slab.height = 1;
    const uint8 texels[8] = {255, 0, 0, 255, 0, 255, 0, 255};
    Bench address;
    if (!address.Initialize(device, 1, shaders::TextureAddressWPS, "Wrapped")) {
        Check(false, "W address bench resources are created");
        return;
    }
    const AddressMode modes[2] = {AddressMode::Wrap, AddressMode::Clamp};
    for (const AddressMode mode : modes) {
        slab.sampler.addressW = mode;
        auto made = device.CreateTexture(slab, Span<const uint8>(texels, sizeof(texels)));
        Check(made.IsOk(), "W address texture is created");
        if (!made) {
            continue;
        }
        const bool wrap = mode == AddressMode::Wrap;
        Check(address.Render(made.Value()) &&
                  NearColor(address.Pixel(0), wrap ? 255 : 0, wrap ? 0 : 255, 0, 255),
              wrap ? "W Wrap reads depth slice 0 at w = 1.25"
                   : "W Clamp reads depth slice 1 at w = 1.25");
        device.Retire(made.Value());
    }
    address.Release();
}

// cube 면 texel 값이다: R = 면 × 40 + 10, G·B = 열·행 × 100 + 50.
void FillCubeFaces(uint8* faces, uint32 cubes) noexcept {
    for (uint32 cube = 0; cube < cubes; ++cube) {
        for (uint32 face = 0; face < CubeFaceCount; ++face) {
            uint8* image = faces + (cube * CubeFaceCount + face) * 2 * 2 * 4;
            for (uint32 row = 0; row < 2; ++row) {
                for (uint32 column = 0; column < 2; ++column) {
                    uint8* texel = image + (row * 2 + column) * 4;
                    texel[0] = static_cast<uint8>(face * 40 + 10);
                    texel[1] = static_cast<uint8>(column * 100 + 50);
                    texel[2] = static_cast<uint8>(row * 100 + 50 + cube * 30);
                    texel[3] = 255;
                }
            }
        }
    }
}

// cube: 왕복(mip 2개), 면 순서와 면 안 방향.
void RunCube(Device& device, Allocator& allocator) noexcept {
    const TextureDesc desc = ColorDesc(TextureDimension::TextureCube, 1, 1, 2);
    Array<uint8> data(allocator);
    if (!data.TryResize(static_cast<usize>(TextureDataBytes(desc)))) {
        Check(false, "cube data allocation");
        return;
    }
    FillBytes(data.View(), 9);
    auto texture = device.CreateTexture(desc, data.View());
    Check(texture.IsOk(), "cube texture is created");
    if (!texture) {
        PrintLastError(device);
        return;
    }
    RunRoundTrip(device, texture.Value(), desc, data.View(), "cube", allocator);
    device.Retire(texture.Value());

    // 2×2 면으로 방향을 본다(점 필터, 면 순서는 UE·D3D CubeFace).
    TextureDesc small = desc;
    small.width = 2;
    small.height = 2;
    small.mipLevels = 1;
    uint8 faces[CubeFaceCount * 2 * 2 * 4] = {};
    FillCubeFaces(faces, 1);
    auto cube = device.CreateTexture(small, Span<const uint8>(faces, sizeof(faces)));
    Check(cube.IsOk(), "2x2 cube texture is created");
    Bench bench;
    if (cube && bench.Initialize(device, 12, shaders::TextureCubePS, "Cube")) {
        Check(bench.Render(cube.Value()), "cube draw");
        for (uint32 face = 0; face < CubeFaceCount; ++face) {
            const int red = static_cast<int>(face) * 40 + 10;
            Check(NearColor(bench.Pixel(face * 2), red, 50, 50, 255),
                  "cube direction toward texel (0, 0) reads that face's first texel");
            Check(NearColor(bench.Pixel(face * 2 + 1), red, 150, 150, 255),
                  "cube direction toward texel (1, 1) reads that face's last texel");
        }
        bench.Release();
    }
    if (cube) {
        device.Retire(cube.Value());
    }
}

// cube 배열: 기능이 없으면 Unsupported, 있으면 왕복과 cube·면 선택.
void RunCubeArray(Device& device, Allocator& allocator) noexcept {
    constexpr uint32 Cubes = 2;
    TextureDesc desc = ColorDesc(TextureDimension::TextureCubeArray, Cubes, 1, 2);
    Array<uint8> data(allocator);
    if (!data.TryResize(static_cast<usize>(TextureDataBytes(desc)))) {
        Check(false, "cube array data allocation");
        return;
    }
    FillBytes(data.View(), 11);
    auto texture = device.CreateTexture(desc, data.View());
    if (!device.Caps().features.textureCubeArray) {
        Check(!texture && texture.Error() == ErrorCode::Unsupported,
              "cube array without the textureCubeArray feature is Unsupported");
        return;
    }
    Check(texture.IsOk(), "cube array texture is created");
    if (!texture) {
        PrintLastError(device);
        return;
    }
    RunRoundTrip(device, texture.Value(), desc, data.View(), "cube array", allocator);
    device.Retire(texture.Value());

    desc.width = 2;
    desc.height = 2;
    desc.mipLevels = 1;
    uint8 faces[Cubes * CubeFaceCount * 2 * 2 * 4] = {};
    FillCubeFaces(faces, Cubes);
    auto cubes = device.CreateTexture(desc, Span<const uint8>(faces, sizeof(faces)));
    Check(cubes.IsOk(), "2x2 cube array texture is created");
    Bench bench;
    if (cubes &&
        bench.Initialize(device, Cubes * CubeFaceCount, shaders::TextureCubeArrayPS, "Cubes")) {
        Check(bench.Render(cubes.Value()), "cube array draw");
        for (uint32 cube = 0; cube < Cubes; ++cube) {
            for (uint32 face = 0; face < CubeFaceCount; ++face) {
                Check(NearColor(bench.Pixel(cube * CubeFaceCount + face),
                                static_cast<int>(face) * 40 + 10, 50,
                                50 + static_cast<int>(cube) * 30, 255),
                      "cube array reads the face of the selected cube");
            }
        }
        bench.Release();
    }
    if (cubes) {
        device.Retire(cubes.Value());
    }
}

// 4×4 균일 텍스처다.
Result<TextureHandle> UniformTexture(Device& device, PixelFormat format, const uint8* texel,
                                     uint32 texelBytes, const ChannelSwizzle& swizzle) noexcept {
    TextureDesc desc;
    desc.width = Extent;
    desc.height = Extent;
    desc.format = format;
    desc.swizzle = swizzle;
    desc.sampler.filter = Filter::Point;
    uint8 data[Extent * Extent * 4] = {};
    for (uint32 index = 0; index < Extent * Extent; ++index) {
        platform::MemoryCopy(data + index * texelBytes, texel, texelBytes);
    }
    return device.CreateTexture(desc, Span<const uint8>(data, Extent * Extent * texelBytes));
}

// view swizzle: 텍스처 desc·view desc의 성분 대응, A8 흉내와의 합성.
void RunSwizzle(Device& device) noexcept {
    const bool supported = device.Caps().features.textureViewSwizzle;
    const uint8 texel[4] = {10, 20, 30, 40};
    const ChannelSwizzle mixed{SwizzleSource::Blue, SwizzleSource::Zero, SwizzleSource::One,
                               SwizzleSource::Red};
    auto swizzled = UniformTexture(device, PixelFormat::RGBA8Unorm, texel, 4, mixed);
    if (!supported) {
        Check(!swizzled && swizzled.Error() == ErrorCode::Unsupported,
              "texture swizzle without view swizzle support is Unsupported");
        auto plain = UniformTexture(device, PixelFormat::RGBA8Unorm, texel, 4, {});
        Check(plain.IsOk(), "identity swizzle texture is created");
        if (plain) {
            TextureViewDesc view;
            view.swizzle.red = SwizzleSource::Alpha;
            auto rejected = device.CreateTextureView(plain.Value(), view);
            Check(!rejected && rejected.Error() == ErrorCode::Unsupported,
                  "view swizzle without view swizzle support is Unsupported");
            device.Retire(plain.Value());
        }
        return;
    }

    Check(swizzled.IsOk(), "swizzled texture is created");
    Bench bench;
    if (!swizzled || !bench.Initialize(device, 1, shaders::TextureLoadPS, "Source")) {
        if (swizzled) {
            device.Retire(swizzled.Value());
        }
        return;
    }
    Check(bench.Render(swizzled.Value()) && NearColor(bench.Pixel(0), 30, 0, 255, 10),
          "texture swizzle (B, 0, 1, R) reads (30, 0, 255, 10)");

    // view는 원 텍스처의 swizzle을 이어받지 않고 자기 대응을 쓴다. 원본과 번갈아 그려 서로 덮지
    // 않는지 본다(OpenGL ES는 성분 대응이 공유 텍스처 객체 상태다).
    TextureViewDesc viewDesc;
    viewDesc.swizzle = {SwizzleSource::Alpha, SwizzleSource::Alpha, SwizzleSource::Green,
                        SwizzleSource::One};
    auto view = device.CreateTextureView(swizzled.Value(), viewDesc);
    Check(view.IsOk(), "swizzled view is created");
    if (view) {
        Check(bench.Render(view.Value()) && NearColor(bench.Pixel(0), 40, 40, 20, 255),
              "view swizzle (A, A, G, 1) reads (40, 40, 20, 255)");
        Check(bench.Render(swizzled.Value()) && NearColor(bench.Pixel(0), 30, 0, 255, 10),
              "texture swizzle is unchanged after drawing its view");
        device.Retire(view.Value());
    }
    device.Retire(swizzled.Value());

    // A8은 (0, 0, 0, a)로 읽는 형식이다. (A, A, A, 1)을 걸면 (a, a, a, 1)이다(OpenGL ES는 R8 저장소
    // 흉내 대응과 합성한다).
    if (HasPixelFormatCaps(device.GetPixelFormatCaps(PixelFormat::A8Unorm),
                           PixelFormatCaps::Sampled)) {
        const uint8 alpha = 77;
        auto plain = UniformTexture(device, PixelFormat::A8Unorm, &alpha, 1, {});
        auto broadcast = UniformTexture(
            device, PixelFormat::A8Unorm, &alpha, 1,
            {SwizzleSource::Alpha, SwizzleSource::Alpha, SwizzleSource::Alpha, SwizzleSource::One});
        Check(plain.IsOk() && broadcast.IsOk(), "A8 textures are created");
        if (plain) {
            Check(bench.Render(plain.Value()) && NearColor(bench.Pixel(0), 0, 0, 0, 77),
                  "A8 reads (0, 0, 0, a)");
            device.Retire(plain.Value());
        }
        if (broadcast) {
            Check(bench.Render(broadcast.Value()) && NearColor(bench.Pixel(0), 77, 77, 77, 255),
                  "A8 with swizzle (A, A, A, 1) reads (a, a, a, 1)");
            device.Retire(broadcast.Value());
        }
    }
    bench.Release();
}

// 깊이 비교 샘플러: D32Float 깊이 0.5를 비교 값 0.25·0.75로 표본한다.
void RunCompare(Device& device) noexcept {
    const PixelFormat format = PixelFormat::D32Float;
    if (!HasPixelFormatCaps(device.GetPixelFormatCaps(format), PixelFormatCaps::Sampled)) {
        printf("  [NOT RUN] %s textures: D32Float is not sampled on this device\n", backendName);
        return;
    }
    Bench bench;
    if (!bench.Initialize(device, 2, shaders::TextureComparePS, "Shadow")) {
        Check(false, "comparison bench resources are created");
        return;
    }
    const float depth = 0.5f;
    uint8 texel[4] = {};
    platform::MemoryCopy(texel, &depth, 4);
    const CompareFunction functions[2] = {CompareFunction::LessEqual,
                                          CompareFunction::GreaterEqual};
    for (const CompareFunction function : functions) {
        TextureDesc desc;
        desc.width = Extent;
        desc.height = Extent;
        desc.format = format;
        desc.sampler.filter = Filter::Point;
        desc.sampler.addressU = AddressMode::Clamp;
        desc.sampler.addressV = AddressMode::Clamp;
        desc.sampler.comparison = true;
        desc.sampler.compare = function;
        uint8 data[Extent * Extent * 4] = {};
        for (uint32 index = 0; index < Extent * Extent; ++index) {
            platform::MemoryCopy(data + index * 4, texel, 4);
        }
        auto texture = device.CreateTexture(desc, Span<const uint8>(data, sizeof(data)));
        Check(texture.IsOk(), "comparison sampler depth texture is created");
        if (!texture) {
            PrintLastError(device);
            continue;
        }
        // 비교 결과는 "비교 값 함수 텍스처 깊이"다: LessEqual은 0.25 ≤ 0.5만 참이다.
        const bool lessEqual = function == CompareFunction::LessEqual;
        Check(bench.Render(texture.Value()) &&
                  NearColor(bench.Pixel(0), lessEqual ? 255 : 0, lessEqual ? 255 : 0,
                            lessEqual ? 255 : 0, 255) &&
                  NearColor(bench.Pixel(1), lessEqual ? 0 : 255, lessEqual ? 0 : 255,
                            lessEqual ? 0 : 255, 255),
              lessEqual ? "LessEqual comparison passes only for reference 0.25"
                        : "GreaterEqual comparison passes only for reference 0.75");
        device.Retire(texture.Value());
    }
    bench.Release();
}

// 필터 5종: 모두 만들어진다. AnisotropicPoint는 하드웨어 대응이 없는 backend(D3D11, OPTIONS19가
// 없는 D3D12)에서 AnisotropicLinear로 만든다(UE와 같다, DeviceFeatures::anisotropicPointFilter).
void RunFilters(Device& device) noexcept {
    const uint8 texel[4] = {1, 2, 3, 4};
    for (uint32 value = 0; value <= static_cast<uint32>(MaxFilter); ++value) {
        const auto filter = static_cast<Filter>(value);
        TextureDesc desc;
        desc.width = Extent;
        desc.height = Extent;
        desc.format = PixelFormat::RGBA8Unorm;
        desc.mipLevels = 3;
        desc.sampler.filter = filter;
        desc.sampler.maxAnisotropy = 8;
        uint8 data[(16 + 4 + 1) * 4] = {};
        for (uint32 index = 0; index < 21; ++index) {
            platform::MemoryCopy(data + index * 4, texel, 4);
        }
        auto texture = device.CreateTexture(desc, Span<const uint8>(data, sizeof(data)));
        char what[96];
        snprintf(what, sizeof(what), "filter %s texture is created", FilterName(filter));
        Check(texture.IsOk(), what);
        if (texture) {
            device.Retire(texture.Value());
        }
    }
}

// staging 업로드: 배열·cube·3D의 mip 하나를 staging(offset 0과 512)에서 올리고 ReadTexture로
// 대조한다. 3D 64×4×4 RGBA8은 행이 256 byte라 D3D12가 staging에서 바로 복사하는 footprint 경로를,
// 배열·cube는 ring 경로(D3D12)와 layerCount 복사(Vulkan)를 탄다.
void RunStagingUploads(Device& device, Allocator& allocator) noexcept {
    TextureDesc descs[3] = {ColorDesc(TextureDimension::Texture2DArray, 3, 1, 2),
                            ColorDesc(TextureDimension::TextureCube, 1, 1, 2),
                            ColorDesc(TextureDimension::Texture3D, 1, 4, 1)};
    descs[2].width = 64;
    const char* labels[3] = {"2D array", "cube", "3D"};
    for (uint32 kind = 0; kind < 3; ++kind) {
        const TextureDesc& desc = descs[kind];
        if (desc.dimension == TextureDimension::Texture3D &&
            !HasPixelFormatCaps(device.GetPixelFormatCaps(desc.format),
                                PixelFormatCaps::Texture3D)) {
            continue;
        }
        Array<uint8> data(allocator);
        if (!data.TryResize(static_cast<usize>(TextureDataBytes(desc)))) {
            Check(false, "staging data allocation");
            return;
        }
        FillBytes(data.View(), 21 + kind);
        auto texture = device.CreateTexture(desc, data.View());
        Check(texture.IsOk(), "staging target texture is created");
        if (!texture) {
            continue;
        }
        const uint32 mip = desc.mipLevels - 1;
        const usize bytes = static_cast<usize>(TextureMipBytes(desc, mip));
        const uint64 offsets[2] = {0, 512};
        for (const uint64 offset : offsets) {
            auto staging = device.AcquireStaging(offset + bytes);
            Check(staging.IsOk(), "staging is acquired");
            if (!staging) {
                continue;
            }
            Array<uint8> fresh(allocator);
            Array<uint8> read(allocator);
            if (!fresh.TryResize(bytes) || !read.TryResize(bytes)) {
                Check(false, "staging compare allocation");
                device.ReleaseStaging(staging.Value().handle);
                continue;
            }
            FillBytes(fresh.View(), 40 + kind * 2 + static_cast<uint32>(offset));
            platform::MemoryCopy(staging.Value().data + offset, fresh.Data(), bytes);
            auto uploaded = device.UploadTextureFromStaging(texture.Value(), mip,
                                                            staging.Value().handle, offset);
            device.ReleaseStaging(staging.Value().handle);
            auto readback = device.ReadTexture(texture.Value(), mip, 0, read.View());
            char what[128];
            snprintf(what, sizeof(what), "%s staging upload at offset %u reads back", labels[kind],
                     static_cast<uint32>(offset));
            Check(uploaded.IsOk() && readback.IsOk() &&
                      platform::MemoryCompare(read.Data(), fresh.Data(), bytes) == 0,
                  what);
            if (!uploaded || !readback) {
                PrintLastError(device);
            }
        }
        device.Retire(texture.Value());
    }
}

// 블록 압축(BC1) 배열·cube·3D 왕복이다. OpenGL ES는 블록 압축 텍스처를 읽지 못한다(ReadTexture
// Unsupported, PixelFormat.ko.md 14.2절).
void RunCompressed(Device& device, Allocator& allocator) noexcept {
    const PixelFormat format = PixelFormat::BC1Unorm;
    const PixelFormatCaps caps = device.GetPixelFormatCaps(format);
    if (!HasPixelFormatCaps(caps, PixelFormatCaps::Sampled | PixelFormatCaps::Mipmaps)) {
        return;
    }
    TextureDesc descs[3] = {ColorDesc(TextureDimension::Texture2DArray, 2, 1, 2),
                            ColorDesc(TextureDimension::TextureCube, 1, 1, 2),
                            ColorDesc(TextureDimension::Texture3D, 1, 2, 1)};
    const char* labels[3] = {"BC1 2D array", "BC1 cube", "BC1 3D"};
    for (uint32 kind = 0; kind < 3; ++kind) {
        TextureDesc desc = descs[kind];
        desc.format = format;
        desc.width = 8;
        desc.height = 8;
        if ((desc.dimension == TextureDimension::Texture3D &&
             !HasPixelFormatCaps(caps, PixelFormatCaps::Texture3D)) ||
            (desc.dimension == TextureDimension::TextureCube &&
             !HasPixelFormatCaps(caps, PixelFormatCaps::TextureCube))) {
            continue;
        }
        Array<uint8> data(allocator);
        if (!data.TryResize(static_cast<usize>(TextureDataBytes(desc)))) {
            Check(false, "compressed data allocation");
            return;
        }
        FillBytes(data.View(), 60 + kind);
        auto texture = device.CreateTexture(desc, data.View());
        char what[96];
        snprintf(what, sizeof(what), "%s texture is created", labels[kind]);
        Check(texture.IsOk(), what);
        if (!texture) {
            PrintLastError(device);
            continue;
        }
        if (device.Info().backend == Backend::OpenGL) {
            Array<uint8> read(allocator);
            if (read.TryResize(static_cast<usize>(TextureMipBytes(desc, 0)))) {
                auto readback = device.ReadTexture(texture.Value(), 0, 0, read.View());
                Check(!readback && readback.Error() == ErrorCode::Unsupported,
                      "OpenGL ES compressed ReadTexture is Unsupported");
            }
        } else {
            RunRoundTrip(device, texture.Value(), desc, data.View(), labels[kind], allocator);
        }
        device.Retire(texture.Value());
    }
}

// swizzle은 셰이더 읽기에만 적용한다: (B, G, R, A) 렌더 타깃을 빨강으로 clear하면 저장·ReadPixels는
// 빨강이고 셰이더로 읽으면 파랑이다(Vulkan은 attachment에 항등 view를 따로 쓴다). 깊이 텍스처를
// (0, 0, 0, 1)로 그린 뒤의 ReadTexture는 저장된 깊이다(OpenGL ES는 성분 대응이 텍스처 객체 상태다).
void RunSwizzleIsolation(Device& device) noexcept {
    if (!device.Caps().features.textureViewSwizzle) {
        return;
    }
    Bench bench;
    if (!bench.Initialize(device, 1, shaders::TextureLoadPS, "Source")) {
        Check(false, "swizzle isolation bench resources are created");
        return;
    }
    TextureDesc targetDesc;
    targetDesc.width = Extent;
    targetDesc.height = Extent;
    targetDesc.format = PixelFormat::RGBA8Unorm;
    targetDesc.usage = TextureUsage::RenderTarget;
    targetDesc.swizzle = {SwizzleSource::Blue, SwizzleSource::Green, SwizzleSource::Red,
                          SwizzleSource::Alpha};
    auto target = device.CreateTexture(targetDesc, {});
    Check(target.IsOk(), "swizzled render target is created");
    if (target) {
        auto frame = device.BeginOffscreen(target.Value(), {1.0f, 0.0f, 0.0f, 1.0f});
        const bool ended = frame.IsOk() && device.EndFrame().IsOk();
        uint8 stored[Extent * Extent * 4] = {};
        const bool read =
            ended && device.ReadPixels(target.Value(), Span<uint8>(stored, sizeof(stored))).IsOk();
        Check(read && NearColor(stored + (Extent + 1) * 4, 255, 0, 0, 255),
              "swizzled render target stores the cleared color unswizzled");
        Check(bench.Render(target.Value()) && NearColor(bench.Pixel(0), 0, 0, 255, 255),
              "swizzled render target reads swizzled in a shader");
        device.Retire(target.Value());
    }

    const PixelFormat depthFormat = PixelFormat::D32Float;
    if (HasPixelFormatCaps(device.GetPixelFormatCaps(depthFormat), PixelFormatCaps::Sampled)) {
        TextureDesc depthDesc;
        depthDesc.width = Extent;
        depthDesc.height = Extent;
        depthDesc.format = depthFormat;
        depthDesc.sampler.filter = Filter::Point;
        depthDesc.swizzle = {SwizzleSource::Zero, SwizzleSource::Zero, SwizzleSource::Zero,
                             SwizzleSource::One};
        const float value = 0.625f;
        float depths[Extent * Extent] = {};
        for (float& depth : depths) {
            depth = value;
        }
        auto depth = device.CreateTexture(
            depthDesc, Span<const uint8>(reinterpret_cast<const uint8*>(depths), sizeof(depths)));
        Check(depth.IsOk(), "swizzled depth texture is created");
        if (depth) {
            Check(bench.Render(depth.Value()) && NearColor(bench.Pixel(0), 0, 0, 0, 255),
                  "swizzled depth texture reads (0, 0, 0, 1) in a shader");
            float read[Extent * Extent] = {};
            auto readback = device.ReadTexture(
                depth.Value(), 0, 0, Span<uint8>(reinterpret_cast<uint8*>(read), sizeof(read)));
            Check(readback.IsOk() && read[5] == value,
                  "ReadTexture after a swizzled draw reads the stored depth");
            device.Retire(depth.Value());
        }
    }
    bench.Release();
}

// 비교 샘플러 선형 필터(PCF): 2×2 깊이(왼쪽 열 0.25, 오른쪽 열 0.75)를 두 열 사이 u = 0.5에서 비교
// 값 0.5, LessEqual로 표본하면 왼쪽 결과 0과 오른쪽 결과 1의 평균 0.5다.
void RunCompareLinear(Device& device) noexcept {
    const PixelFormat format = PixelFormat::D32Float;
    if (!HasPixelFormatCaps(device.GetPixelFormatCaps(format), PixelFormatCaps::Sampled)) {
        return;
    }
    Bench bench;
    if (!bench.Initialize(device, 1, shaders::TextureCompareLinearPS, "Shadow")) {
        Check(false, "PCF bench resources are created");
        return;
    }
    TextureDesc desc;
    desc.width = 2;
    desc.height = 2;
    desc.format = format;
    desc.sampler.filter = Filter::Bilinear;
    desc.sampler.addressU = AddressMode::Clamp;
    desc.sampler.addressV = AddressMode::Clamp;
    desc.sampler.comparison = true;
    desc.sampler.compare = CompareFunction::LessEqual;
    const float depths[4] = {0.25f, 0.75f, 0.25f, 0.75f};
    auto texture = device.CreateTexture(
        desc, Span<const uint8>(reinterpret_cast<const uint8*>(depths), sizeof(depths)));
    Check(texture.IsOk(), "PCF depth texture is created");
    if (texture) {
        // 선형 비교 결과는 구현마다 가중 오차가 있어 0.5 ± 0.05로 본다.
        const bool drawn = bench.Render(texture.Value());
        const int red = bench.Pixel(0)[0];
        Check(drawn && red >= 115 && red <= 140,
              "bilinear comparison between a failing and a passing texel is about 0.5");
        device.Retire(texture.Value());
    }
    // 비교 샘플러는 깊이 형식에만 붙는다.
    TextureDesc color = desc;
    color.format = PixelFormat::RGBA8Unorm;
    const uint8 texels[16] = {};
    auto rejected = device.CreateTexture(color, Span<const uint8>(texels, sizeof(texels)));
    Check(!rejected && rejected.Error() == ErrorCode::InvalidArgument,
          "comparison sampler on a color format is InvalidArgument");
    bench.Release();
}

} // namespace

uint32 RunTextureChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    RunArray(device, allocator);
    RunVolume(device, allocator);
    RunCube(device, allocator);
    RunCubeArray(device, allocator);
    RunStagingUploads(device, allocator);
    RunCompressed(device, allocator);
    RunSwizzle(device);
    RunSwizzleIsolation(device);
    RunCompare(device);
    RunCompareLinear(device);
    RunFilters(device);
    auto flushed = device.WaitIdle();
    Check(flushed.IsOk(), "WaitIdle after texture checks");
    printf("  %s textures: dimensions, swizzle %s, anisotropic point %s, %u failure%s\n", name,
           device.Caps().features.textureViewSwizzle ? "yes" : "no",
           device.Caps().features.anisotropicPointFilter ? "yes" : "no", failures,
           failures == 1 ? "" : "s");
    return failures;
}

} // namespace sandbox
