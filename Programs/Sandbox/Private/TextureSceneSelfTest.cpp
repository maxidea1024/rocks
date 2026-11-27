#include "TextureSceneSelfTest.h"

#include "CpuDisplayColor.h"
#include "MemoryPlatformTexture.h"
#include "SandboxShaders.h"
#include "ScreenHost.h"
#include "SelfTestEnvironment.h"
#include "TexelCodec.h"
#include "TextureSceneCapture.h"

#include <Core/Containers/Array.h>
#include <Core/Move.h>
#include <Input/InputEvent.h>
#include <PlatformWindow/Window.h>
#include <math.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;

// 메모리 장면의 값 계열이다(형식과 함께 texel 값을 정한다).
enum class SceneFamily : uint8 {
    // 색(빨강·초록·파랑·흰색, mip 1부터 절반 밝기)
    Ldr,
    // 두 색(사분면 짝수·홀수 — 블록 하나에 색이 둘뿐이라 BC 끝점만 쓴다, mip 1부터 절반 밝기)
    Duo,
    // HDR 색(배율 2 + mip)
    Hdr,
    // 회색 한 채널(.rrrr swizzle)
    Gray,
    // 부호 없는 정수(빨강 채널)
    Uint,
    // 부호 있는 정수(음수 포함)
    Sint,
    // 노멀 맵 x·y(쿠킹 swizzle rg01 — Normal sampler type, 표시는 복원 법선)
    Normal
};

// 장면 하나다: 텍스처와 그릴 층·mip.
struct SceneDesc {
    const char* name;
    TextureDimension dimension;
    graphics::PixelFormat format;
    SceneFamily family;
    TextureSamplerFilter filter;
    uint32 layer;
    uint32 mip;
};

// 차원마다의 크기·층·mip 수다(사분면이 mip 1에서도 2 texel 이상 — 필터와 무관한 표본점).
constexpr uint32 PlaneSize = 8;
constexpr uint32 PlaneMips = 4;
// mip 0·1은 스트리밍 bulk, 2·3은 상주 꼬리다(6.5절 두 위치를 모두 거친다).
constexpr uint32 PlaneResidentMips = 2;
constexpr uint32 SmallSize = 4;
constexpr uint32 SmallMips = 2;
constexpr uint32 ArrayLayers = 3;
constexpr uint32 CubeArrayCubes = 2;
constexpr uint32 VolumeDepth = 4;

constexpr SceneDesc Scenes[] = {
    {"2D RGBA8Unorm mip 0", TextureDimension::Texture2D, graphics::PixelFormat::RGBA8Unorm,
     SceneFamily::Ldr, TextureSamplerFilter::Bilinear, 0, 0},
    {"2D RGBA8Unorm mip 1", TextureDimension::Texture2D, graphics::PixelFormat::RGBA8Unorm,
     SceneFamily::Ldr, TextureSamplerFilter::Trilinear, 0, 1},
    {"2D RGBA8Unorm mip 2 (tail)", TextureDimension::Texture2D, graphics::PixelFormat::RGBA8Unorm,
     SceneFamily::Ldr, TextureSamplerFilter::Point, 0, 2},
    {"2D RGBA8UnormSrgb", TextureDimension::Texture2D, graphics::PixelFormat::RGBA8UnormSrgb,
     SceneFamily::Ldr, TextureSamplerFilter::Bilinear, 0, 1},
    {"2D RGBA16Float", TextureDimension::Texture2D, graphics::PixelFormat::RGBA16Float,
     SceneFamily::Hdr, TextureSamplerFilter::Bilinear, 0, 0},
    {"2D RGBA32Float", TextureDimension::Texture2D, graphics::PixelFormat::RGBA32Float,
     SceneFamily::Hdr, TextureSamplerFilter::Point, 0, 1},
    {"2D R8Unorm rrrr", TextureDimension::Texture2D, graphics::PixelFormat::R8Unorm,
     SceneFamily::Gray, TextureSamplerFilter::Bilinear, 0, 0},
    {"2D R8Uint", TextureDimension::Texture2D, graphics::PixelFormat::R8Uint, SceneFamily::Uint,
     TextureSamplerFilter::Point, 0, 1},
    {"2D RGBA16Sint", TextureDimension::Texture2D, graphics::PixelFormat::RGBA16Sint,
     SceneFamily::Sint, TextureSamplerFilter::Point, 0, 0},
    {"2DArray RGBA8Unorm", TextureDimension::Texture2DArray, graphics::PixelFormat::RGBA8Unorm,
     SceneFamily::Ldr, TextureSamplerFilter::Point, 2, 1},
    {"2DArray RGBA8UnormSrgb", TextureDimension::Texture2DArray,
     graphics::PixelFormat::RGBA8UnormSrgb, SceneFamily::Ldr, TextureSamplerFilter::Point, 1, 0},
    {"2DArray RGBA16Float", TextureDimension::Texture2DArray, graphics::PixelFormat::RGBA16Float,
     SceneFamily::Hdr, TextureSamplerFilter::Point, 1, 1},
    {"2DArray RGBA32Float", TextureDimension::Texture2DArray, graphics::PixelFormat::RGBA32Float,
     SceneFamily::Hdr, TextureSamplerFilter::Point, 2, 0},
    {"2DArray R8Unorm rrrr", TextureDimension::Texture2DArray, graphics::PixelFormat::R8Unorm,
     SceneFamily::Gray, TextureSamplerFilter::Point, 1, 0},
    {"2DArray R8Uint", TextureDimension::Texture2DArray, graphics::PixelFormat::R8Uint,
     SceneFamily::Uint, TextureSamplerFilter::Point, 2, 0},
    {"2DArray RGBA16Sint", TextureDimension::Texture2DArray, graphics::PixelFormat::RGBA16Sint,
     SceneFamily::Sint, TextureSamplerFilter::Point, 1, 1},
    {"Cube RGBA8Unorm", TextureDimension::TextureCube, graphics::PixelFormat::RGBA8Unorm,
     SceneFamily::Ldr, TextureSamplerFilter::Bilinear, 0, 1},
    {"Cube RGBA8UnormSrgb", TextureDimension::TextureCube, graphics::PixelFormat::RGBA8UnormSrgb,
     SceneFamily::Ldr, TextureSamplerFilter::Point, 0, 0},
    {"Cube RGBA16Float", TextureDimension::TextureCube, graphics::PixelFormat::RGBA16Float,
     SceneFamily::Hdr, TextureSamplerFilter::Point, 0, 0},
    {"Cube RGBA32Float", TextureDimension::TextureCube, graphics::PixelFormat::RGBA32Float,
     SceneFamily::Hdr, TextureSamplerFilter::Point, 0, 1},
    {"Cube R8Unorm rrrr", TextureDimension::TextureCube, graphics::PixelFormat::R8Unorm,
     SceneFamily::Gray, TextureSamplerFilter::Point, 0, 0},
    {"Cube R8Uint (no display)", TextureDimension::TextureCube, graphics::PixelFormat::R8Uint,
     SceneFamily::Uint, TextureSamplerFilter::Point, 0, 0},
    {"CubeArray RGBA8Unorm", TextureDimension::TextureCubeArray, graphics::PixelFormat::RGBA8Unorm,
     SceneFamily::Ldr, TextureSamplerFilter::Point, 1, 1},
    {"CubeArray RGBA8UnormSrgb", TextureDimension::TextureCubeArray,
     graphics::PixelFormat::RGBA8UnormSrgb, SceneFamily::Ldr, TextureSamplerFilter::Bilinear, 0, 0},
    {"CubeArray RGBA16Float", TextureDimension::TextureCubeArray,
     graphics::PixelFormat::RGBA16Float, SceneFamily::Hdr, TextureSamplerFilter::Point, 1, 0},
    {"CubeArray RGBA32Float", TextureDimension::TextureCubeArray,
     graphics::PixelFormat::RGBA32Float, SceneFamily::Hdr, TextureSamplerFilter::Point, 0, 1},
    {"CubeArray R8Unorm rrrr", TextureDimension::TextureCubeArray, graphics::PixelFormat::R8Unorm,
     SceneFamily::Gray, TextureSamplerFilter::Point, 1, 0},
    {"Volume RGBA8Unorm", TextureDimension::Volume, graphics::PixelFormat::RGBA8Unorm,
     SceneFamily::Ldr, TextureSamplerFilter::Bilinear, 2, 0},
    {"Volume RGBA8UnormSrgb", TextureDimension::Volume, graphics::PixelFormat::RGBA8UnormSrgb,
     SceneFamily::Ldr, TextureSamplerFilter::Point, 1, 1},
    {"Volume RGBA16Float", TextureDimension::Volume, graphics::PixelFormat::RGBA16Float,
     SceneFamily::Hdr, TextureSamplerFilter::Point, 3, 0},
    {"Volume RGBA32Float", TextureDimension::Volume, graphics::PixelFormat::RGBA32Float,
     SceneFamily::Hdr, TextureSamplerFilter::Point, 0, 1},
    {"Volume R8Unorm rrrr", TextureDimension::Volume, graphics::PixelFormat::R8Unorm,
     SceneFamily::Gray, TextureSamplerFilter::Point, 1, 0},
    {"Volume R8Uint", TextureDimension::Volume, graphics::PixelFormat::R8Uint, SceneFamily::Uint,
     TextureSamplerFilter::Point, 2, 0},
    {"Volume RGBA16Sint", TextureDimension::Volume, graphics::PixelFormat::RGBA16Sint,
     SceneFamily::Sint, TextureSamplerFilter::Point, 1, 1},
    // 쿠킹 형식(개정 63 — TC2): BGRA8 비압축 대체와 블록 압축. 색 BC 장면은 표본 texel이 끝점인
    // 블록만 고른다(한 색 블록이나 두 색 계열 Duo): 끝점 사이 보간 색은 장치마다 다르다(sRGB 짝의
    // 보간 공간 — WARP는 sRGB 값, 하드웨어 D3D·Vulkan은 선형 값 — 과 반올림, 첫 실행에서 4–5 단위
    // 차이). 4보다 작은 mip(블록 하나)도 덮는다.
    {"2D BGRA8UnormSrgb", TextureDimension::Texture2D, graphics::PixelFormat::BGRA8UnormSrgb,
     SceneFamily::Ldr, TextureSamplerFilter::Point, 0, 1},
    {"2D BGRA8Unorm normal rg01", TextureDimension::Texture2D, graphics::PixelFormat::BGRA8Unorm,
     SceneFamily::Normal, TextureSamplerFilter::Bilinear, 0, 0},
    {"2D BC1Unorm", TextureDimension::Texture2D, graphics::PixelFormat::BC1Unorm, SceneFamily::Ldr,
     TextureSamplerFilter::Bilinear, 0, 0},
    {"2D BC1UnormSrgb mip 2 (tail, 2 x 2)", TextureDimension::Texture2D,
     graphics::PixelFormat::BC1UnormSrgb, SceneFamily::Duo, TextureSamplerFilter::Point, 0, 2},
    {"2D BC3UnormSrgb mip 1", TextureDimension::Texture2D, graphics::PixelFormat::BC3UnormSrgb,
     SceneFamily::Duo, TextureSamplerFilter::Point, 0, 1},
    {"2D BC4Unorm rrrr", TextureDimension::Texture2D, graphics::PixelFormat::BC4Unorm,
     SceneFamily::Gray, TextureSamplerFilter::Bilinear, 0, 0},
    {"2D BC5Unorm normal rg01", TextureDimension::Texture2D, graphics::PixelFormat::BC5Unorm,
     SceneFamily::Normal, TextureSamplerFilter::Bilinear, 0, 0},
    {"2D BC5Unorm normal mip 3 (tail, 1 x 1)", TextureDimension::Texture2D,
     graphics::PixelFormat::BC5Unorm, SceneFamily::Normal, TextureSamplerFilter::Point, 0, 3},
    {"2DArray BC1UnormSrgb mip 1 (2 x 2)", TextureDimension::Texture2DArray,
     graphics::PixelFormat::BC1UnormSrgb, SceneFamily::Duo, TextureSamplerFilter::Point, 2, 1},
    {"2DArray BC4Unorm rrrr", TextureDimension::Texture2DArray, graphics::PixelFormat::BC4Unorm,
     SceneFamily::Gray, TextureSamplerFilter::Point, 1, 0},
    {"Cube BC1Unorm", TextureDimension::TextureCube, graphics::PixelFormat::BC1Unorm,
     SceneFamily::Ldr, TextureSamplerFilter::Point, 0, 0},
    {"Cube BC3UnormSrgb mip 1 (2 x 2)", TextureDimension::TextureCube,
     graphics::PixelFormat::BC3UnormSrgb, SceneFamily::Ldr, TextureSamplerFilter::Point, 0, 1},
    {"CubeArray BC5Unorm normal rg01", TextureDimension::TextureCubeArray,
     graphics::PixelFormat::BC5Unorm, SceneFamily::Normal, TextureSamplerFilter::Point, 1, 0},
    {"Volume BC1UnormSrgb", TextureDimension::Volume, graphics::PixelFormat::BC1UnormSrgb,
     SceneFamily::Duo, TextureSamplerFilter::Point, 1, 0},
    {"Volume BC4Unorm rrrr mip 1 (2 x 2)", TextureDimension::Volume,
     graphics::PixelFormat::BC4Unorm, SceneFamily::Gray, TextureSamplerFilter::Point, 0, 1}};

constexpr uint32 SceneCount = sizeof(Scenes) / sizeof(Scenes[0]);
// 장면 뒤의 음성 장면 하나(장치 생성 실패 — typed 대체 표시)가 capture 자리 하나를 더 쓴다.
static_assert(SceneCount + 1 <= screen::MaxCaptureSlots, "one capture slot per scene");

// 값 계열의 사분면 기본 색이다.
constexpr float Palette[4][4] = {{1.0f, 0.0f, 0.0f, 1.0f},
                                 {0.0f, 1.0f, 0.0f, 1.0f},
                                 {0.0f, 0.0f, 1.0f, 1.0f},
                                 {1.0f, 1.0f, 1.0f, 1.0f}};

// 노멀 맵 계열의 x·y([0, 1] — 0.5가 0)다. 첫 값은 평평한 법선이다.
constexpr float NormalPalette[4][2] = {{0.5f, 0.5f}, {0.8f, 0.5f}, {0.5f, 0.25f}, {0.3f, 0.65f}};

// texel 채우기 문맥이다.
struct FillContext {
    SceneFamily family;
    TextureDimension dimension;
    uint32 width;
    uint32 height;
};

void SceneTexel(void* raw, uint32 level, uint32 slice, uint32 x, uint32 y, uint32 z,
                float (&value)[4]) noexcept {
    const auto& context = *static_cast<const FillContext*>(raw);
    const uint32 width = graphics::MipExtent(context.width, level);
    const uint32 height = graphics::MipExtent(context.height, level);
    const bool cube = context.dimension == TextureDimension::TextureCube ||
                      context.dimension == TextureDimension::TextureCubeArray;
    // cube 면은 한 색(필터·면 경계와 무관), 그 밖은 사분면 색이다.
    const uint32 quadrant =
        cube ? 0u
             : (x >= width / 2 && width > 1 ? 1u : 0u) + (y >= height / 2 && height > 1 ? 2u : 0u);
    const uint32 key = quadrant + slice + 2 * level + z;
    const float* color = Palette[key % 4];
    switch (context.family) {
    case SceneFamily::Ldr:
    case SceneFamily::Duo: {
        // Duo는 key 짝수·홀수로 빨강·흰색 두 색만 쓴다(층·z마다 사분면 색이 뒤바뀐다).
        const float* pick = context.family == SceneFamily::Duo ? Palette[(key % 2) * 3] : color;
        const float scale = level == 0 ? 1.0f : 0.5f;
        value[0] = pick[0] * scale;
        value[1] = pick[1] * scale;
        value[2] = pick[2] * scale;
        value[3] = 1.0f;
        return;
    }

    case SceneFamily::Hdr: {
        const float scale = 2.0f + static_cast<float>(level) + 0.5f * static_cast<float>(key % 3);
        value[0] = color[0] * scale + 0.125f;
        value[1] = color[1] * scale + 0.125f;
        value[2] = color[2] * scale + 0.125f;
        value[3] = 1.0f;
        return;
    }

    case SceneFamily::Gray:
        value[0] = 0.15f + 0.2f * static_cast<float>(key % 4);
        return;
    case SceneFamily::Uint:
        value[0] = static_cast<float>(20 + 50 * (key % 4) + 5 * level);
        return;
    case SceneFamily::Sint:
        value[0] = static_cast<float>(8000 * static_cast<int>(key % 4) - 4000);
        value[1] = static_cast<float>(30000 - 7000 * static_cast<int>(key % 4));
        value[2] = static_cast<float>(-300);
        value[3] = 1.0f;
        return;
    case SceneFamily::Normal:
        value[0] = NormalPalette[key % 4][0];
        value[1] = NormalPalette[key % 4][1];
        value[2] = 1.0f;
        value[3] = 1.0f;
        return;
    }
}

MemoryTextureSpec SpecOf(const SceneDesc& scene) noexcept {
    MemoryTextureSpec spec;
    spec.dimension = scene.dimension;
    spec.format = scene.format;
    spec.sampler.filter = scene.filter;
    spec.sampler.addressU = TextureAddress::Clamp;
    spec.sampler.addressV = TextureAddress::Clamp;
    spec.sampler.addressW = TextureAddress::Clamp;
    spec.width = spec.height = SmallSize;
    spec.mips = SmallMips;
    switch (scene.dimension) {
    case TextureDimension::Texture2D:
        spec.width = spec.height = PlaneSize;
        spec.mips = PlaneMips;
        spec.residentTailMips = PlaneResidentMips;
        break;
    case TextureDimension::Texture2DArray:
        spec.slices = ArrayLayers;
        break;
    case TextureDimension::TextureCube:
        spec.slices = graphics::CubeFaceCount;
        break;
    case TextureDimension::TextureCubeArray:
        spec.slices = graphics::CubeFaceCount * CubeArrayCubes;
        break;
    case TextureDimension::Volume:
        spec.depth = VolumeDepth;
        spec.residentTailMips = 1;
        break;
    }

    if (scene.family == SceneFamily::Gray) {
        // 단일 채널 sampler type 계약(Grayscale — AssetSystem 12.4.3절 .rrrr).
        for (TextureSwizzle& swizzle : spec.swizzle) {
            swizzle = TextureSwizzle::R;
        }
    }

    if (scene.family == SceneFamily::Normal) {
        // Normal sampler type 계약(Normalmap — 12.4.3절 rg01, 형식과 무관).
        spec.swizzle[2] = TextureSwizzle::Zero;
        spec.swizzle[3] = TextureSwizzle::One;
    }

    return spec;
}

uint32 failures = 0;

void Check(bool condition, const char* device, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s texture scenes: %s\n", device, what);
    }
}

// 창 메시지 펌프 hook이다(렌더 lane slot 대기 중 — 5.3절).
struct PumpState {
    platform::Window* window = nullptr;
    input::InputQueue* queue = nullptr;
};

void PumpWindow(void* raw) noexcept {
    auto* state = static_cast<PumpState*>(raw);
    state->window->PumpEvents(*state->queue);
    state->queue->Clear();
}

// 쓰지 못하게 bulk 하나를 1 byte 자르는 reader다(음성 — 잘린 bulk).
struct TruncatingReader {
    MemoryPlatformTexture* texture = nullptr;
    uint32 bulkId = 0;
};

Result<Span<const uint8>> ReadTruncated(void* raw, const BulkDataRef& ref) noexcept {
    const auto& reader = *static_cast<const TruncatingReader*>(raw);
    auto bytes = MemoryPlatformTexture::ReadBulk(reader.texture, ref);
    if (!bytes || ref.bulkId != reader.bulkId || bytes.Value().Size() == 0) {
        return bytes;
    }

    return Result<Span<const uint8>>::Success(
        Span<const uint8>(bytes.Value().Data(), bytes.Value().Size() - 1));
}

// 장치 쪽 음성 시험이다(한 모드에서 한 번): 검사 오류와 원인.
void CheckRejections(screen::ScreenHost& screen, graphics::Device& device, Allocator& allocator,
                     const char* name) noexcept {
    const uint32 before = screen.InspectorEntryCount();
    // 잘린 스트리밍 bulk → CorruptEntry(BulkSize).
    MemoryPlatformTexture plane(allocator);
    FillContext fill{SceneFamily::Ldr, TextureDimension::Texture2D, PlaneSize, PlaneSize};
    MemoryTextureSpec spec = SpecOf(Scenes[0]);
    if (!plane.Build(spec, &SceneTexel, &fill)) {
        Check(false, name, "negative texture builds");
        return;
    }

    TruncatingReader truncating{&plane, plane.Data().mips[0].bulk.bulkId};
    render::CookedTextureIssue issue = render::CookedTextureIssue::None;
    auto truncated = screen.AddTexture("Negative/Truncated", plane.Data(), &ReadTruncated,
                                       &truncating, false, &issue);
    Check(!truncated && truncated.Error() == ErrorCode::CorruptData &&
              issue == render::CookedTextureIssue::BulkSize,
          name, "a truncated mip bulk is rejected as CorruptEntry (BulkSize)");
    // 잘린 꼬리 → TailRange.
    TruncatingReader tail{&plane, plane.Data().tailBulk.bulkId};
    auto cutTail = screen.AddTexture("Negative/TruncatedTail", plane.Data(), &ReadTruncated, &tail,
                                     false, &issue);
    Check(!cutTail && issue == render::CookedTextureIssue::TailRange, name,
          "a truncated resident tail is rejected (TailRange)");
    // mip byte 수 불일치 → MipByteSize.
    plane.MutableData().mips[1].byteSize += 4;
    auto mismatch = screen.AddTexture("Negative/ByteSize", plane.Data(),
                                      &MemoryPlatformTexture::ReadBulk, &plane, false, &issue);
    Check(!mismatch && mismatch.Error() == ErrorCode::CorruptData &&
              issue == render::CookedTextureIssue::MipByteSize,
          name, "a mip byte size mismatch is rejected (MipByteSize)");
    // 장치 한도 초과(한 변 = 한도 + 1) → UnsupportedCapability(DeviceLimit).
    MemoryPlatformTexture wide(allocator);
    MemoryTextureSpec wideSpec;
    wideSpec.format = graphics::PixelFormat::R8Unorm;
    wideSpec.width = device.Caps().limits.textureSize2D + 1;
    wideSpec.height = 1;
    FillContext wideFill{SceneFamily::Gray, TextureDimension::Texture2D, wideSpec.width, 1};
    if (wide.Build(wideSpec, &SceneTexel, &wideFill)) {
        auto limit = screen.AddTexture("Negative/Limit", wide.Data(),
                                       &MemoryPlatformTexture::ReadBulk, &wide, false, &issue);
        Check(!limit && limit.Error() == ErrorCode::UnsupportedCapability &&
                  issue == render::CookedTextureIssue::DeviceLimit,
              name, "a texture over the device size limit is UnsupportedCapability (DeviceLimit)");
    }

    // 장치가 샘플링하지 못하는 압축 형식 → UnsupportedCapability(UnsupportedFormat).
    constexpr graphics::PixelFormat Candidates[] = {graphics::PixelFormat::ASTC4x4Unorm,
                                                    graphics::PixelFormat::ETC2RGB8Unorm,
                                                    graphics::PixelFormat::BC7Unorm};
    bool tested = false;
    for (const graphics::PixelFormat format : Candidates) {
        if (HasPixelFormatCaps(device.GetPixelFormatCaps(format),
                               graphics::PixelFormatCaps::Sampled)) {
            continue;
        }

        const uint8 block[16] = {};
        render::CookedTextureMip mip;
        mip.width = 4;
        mip.height = 4;
        mip.byteSize = sizeof(block);
        render::CookedTextureSource source;
        source.format = format;
        source.width = 4;
        source.height = 4;
        source.residentTailMips = 1;
        source.mips = Span<const render::CookedTextureMip>(&mip, 1);
        source.tail = Span<const uint8>(block, sizeof(block));
        auto prepared =
            render::PrepareCookedTexture(allocator, source, device, render::RenderHandle{}, &issue);
        Check(!prepared && prepared.Error() == ErrorCode::UnsupportedCapability &&
                  issue == render::CookedTextureIssue::UnsupportedFormat,
              name, "a format the device cannot sample is UnsupportedCapability");
        printf("  %s texture scenes: unsupported format check with %s\n", name,
               graphics::PixelFormatName(format));
        tested = true;
        break;
    }

    if (!tested) {
        printf("  [NOT RUN] %s texture scenes: unsupported format check (the device samples "
               "ASTC, ETC2 and BC7)\n",
               name);
    }

    Check(screen.InspectorEntryCount() == before, name,
          "rejected textures add no inspector entries");
}

// 장면 하나의 표본점 기대값을 계산한다(메모리 byte에서). 결과는 checks 끝에 붙는다.
void AddExpectations(const SceneDesc& scene, uint32 sceneIndex, uint32 slot,
                     const MemoryPlatformTexture& memory, const screen::InspectorTexture& texture,
                     Array<SceneCheck>& checks) noexcept {
    const TexturePlatformData& data = memory.Data();
    const TextureMipData& mip = data.mips[scene.mip];
    auto bytes = MipBytes(data, &MemoryPlatformTexture::ReadBulk,
                          const_cast<MemoryPlatformTexture*>(&memory), scene.mip);
    if (!bytes) {
        return;
    }

    AppendInspectorChecks(texture, scene.layer, scene.mip, mip.sizeX, mip.sizeY, data.numSlices,
                          mip.sizeZ, scene.format, bytes.Value(), sceneIndex, slot, scene.name,
                          checks);
}

// 한 모드를 돌린다. 돌리지 못했으면 false다. checks는 처음 모드가 채운다(빈 경우만).
bool RunMode(const char* name, render::RenderLaneMode mode, graphics::Device& device,
             Allocator& allocator, Array<SceneCheck>& checks, Array<uint8>& out) noexcept {
    const char* modeName = mode == render::RenderLaneMode::Thread ? "thread" : "inline";
    const bool fillChecks = checks.IsEmpty();
    platform::WindowDesc windowDesc;
    windowDesc.title = "Rocks texture scene self-test";
    windowDesc.width = SceneCaptureWidth;
    windowDesc.height = SceneCaptureHeight;
    windowDesc.visible = false;
    auto window = platform::Window::Create(allocator, windowDesc);
    input::InputQueue queue(allocator);
    if (!window || !queue.TryInitialize(64)) {
        printf("  [NOT RUN] %s texture scenes (%s): window unavailable\n", name, modeName);
        return false;
    }

    const uint64 validationBefore = device.ValidationErrorCount();
    PumpState pump{window.Value().Get(), &queue};
    screen::ScreenHost screen(allocator, device, shaders::Host());
    auto started = screen.Start(*window.Value(), mode, {&PumpWindow, &pump});
    Check(started.IsOk(), name, "render lane starts");
    if (!started) {
        return false;
    }

    Check(screen.RequestCapture(SceneCaptureWidth, SceneCaptureHeight).IsOk(), name,
          "capture texture is requested");
    // 장면마다 텍스처를 올리고 표시 항목을 기억한다(못 올리면 그 장면은 NOT RUN).
    uint32 entries[SceneCount];
    for (uint32 index = 0; index < SceneCount; ++index) {
        entries[index] = screen::ScreenHost::NoInspectorEntry;
        const SceneDesc& scene = Scenes[index];
        MemoryTextureSpec spec = SpecOf(scene);
        FillContext fill{scene.family, scene.dimension, spec.width, spec.height};
        MemoryPlatformTexture memory(allocator);
        if (!memory.Build(spec, &SceneTexel, &fill)) {
            Check(false, name, "a memory scene texture builds");
            continue;
        }

        render::CookedTextureIssue issue = render::CookedTextureIssue::None;
        auto added = screen.AddTexture(scene.name, memory.Data(), &MemoryPlatformTexture::ReadBulk,
                                       &memory, false, &issue);
        if (!added) {
            if (added.Error() == ErrorCode::UnsupportedCapability) {
                if (fillChecks) {
                    printf("  [NOT RUN] %s texture scene %s: %s (%s)\n", name, scene.name,
                           ErrorCodeName(added.Error()), render::CookedTextureIssueName(issue));
                }
            } else {
                Check(false, name, scene.name);
                printf("    %s: %s (%s)\n", scene.name, ErrorCodeName(added.Error()),
                       render::CookedTextureIssueName(issue));
            }

            continue;
        }

        entries[index] = added.Value();
        if (fillChecks) {
            AddExpectations(scene, index, index, memory,
                            screen.InspectorEntryAt(added.Value()).texture, checks);
        }
    }

    // 장치 생성 실패(검사를 건너뛴 크기 불일치 픽셀) → ResourceFailed, 표시는 typed 대체(흰 2D).
    render::CookedTextureUpload broken;
    broken.desc.width = SmallSize;
    broken.desc.height = SmallSize;
    broken.desc.format = graphics::PixelFormat::RGBA8Unorm;
    const uint8 shortPixels[3] = {};
    auto blob = render::RenderBlob::TryCopy(allocator, Span<const uint8>(shortPixels, 3));
    uint32 brokenEntry = screen::ScreenHost::NoInspectorEntry;
    if (blob) {
        broken.pixels = Move(blob.Value());
        screen::InspectorTexture brokenTexture;
        brokenTexture.width = brokenTexture.height = SmallSize;
        auto added = screen.AddUnchecked("Negative/DeviceFailure", Move(broken), brokenTexture);
        if (added) {
            brokenEntry = added.Value();
        }
    }

    if (mode == render::RenderLaneMode::Thread) {
        CheckRejections(screen, device, allocator, name);
    }

    // 장면마다 한 프레임(capture 자리 = 장면 번호, 음성 장면은 SceneCount).
    for (uint32 index = 0; index <= SceneCount; ++index) {
        const uint32 entry = index < SceneCount ? entries[index] : brokenEntry;
        if (entry == screen::ScreenHost::NoInspectorEntry) {
            continue;
        }

        const uint32 layer = index < SceneCount ? Scenes[index].layer : 0;
        const uint32 mip = index < SceneCount ? Scenes[index].mip : 0;
        screen.SetInspectorEntry(entry, layer, mip);
        screen.SetCaptureSlot(index);
        screen.SubmitFrame(nullptr);
    }

    Check(screen.WaitFramesIdle().IsOk(), name, "frames finish");
    screen.ProcessNotices();
    uint32 ready = 0;
    for (uint32 index = 0; index < SceneCount; ++index) {
        if (entries[index] != screen::ScreenHost::NoInspectorEntry) {
            ready +=
                screen.InspectorEntryAt(entries[index]).state == screen::InspectorEntryState::Ready
                    ? 1u
                    : 0u;
            Check(screen.InspectorEntryAt(entries[index]).state ==
                      screen::InspectorEntryState::Ready,
                  name, "an uploaded scene texture becomes Ready (ResourceReady)");
        }
    }

    if (brokenEntry != screen::ScreenHost::NoInspectorEntry) {
        Check(screen.InspectorEntryAt(brokenEntry).state == screen::InspectorEntryState::Failed,
              name, "a device creation failure becomes Failed (ResourceFailed)");
    }

    screen.BlockFrames();
    for (;;) {
        auto drained = screen.DrainRetires();
        if (!drained || drained.Value() == 0) {
            break;
        }
    }

    Check(screen.CloseResources().IsOk(), name, "render resources close");
    Check(screen.Stop().IsOk(), name, "render lane stops");
    const render::RenderLaneStats stats = screen.Lane().Stats();
    Check(screen.Lane().Failure() == ErrorCode::None &&
              screen.Renderer().Stats().graphFailures == 0,
          name, "frames render without lane or graph failure");
    Check(stats.tasksPosted == stats.tasksCompleted && screen.Lane().Handles().Count() == 0, name,
          "lane drained with no remaining tasks or handles");
    Check(device.IsOwnerThread() && device.PendingRetireCount() == 0, name,
          "device ownership returned with nothing pending");
    Check(device.ValidationErrorCount() == validationBefore, name,
          "no validation errors from texture scenes");
    // 음성 장면: typed 대체(흰 2D)와 정수 cube(표시 엔트리 없음 — 검정)가 대체 계수를 올린다.
    Check(screen.Renderer().Stats().inspectorFallbacks >= 1, name,
          "unready and undisplayable textures draw a fallback");
    if (brokenEntry != screen::ScreenHost::NoInspectorEntry) {
        const Array<uint8>& pixels = screen.Renderer().CapturedPixels(SceneCount);
        const DisplayPixel white{{255, 255, 255}};
        CheckCapturePixel(pixels, SceneCaptureWidth / 2, SceneCaptureHeight / 2, white,
                          SceneCaptureTolerance, name, modeName,
                          "a failed upload draws with the typed fallback", failures);
    }

    for (const SceneCheck& check : checks) {
        const Array<uint8>& pixels = screen.Renderer().CapturedPixels(check.slot);
        CheckCapturePixel(pixels, check.x, check.y, check.expected, SceneCaptureTolerance, name,
                          modeName, check.what, failures);
    }

    // 자리마다 같은 크기로 잇는다(그리지 않은 자리는 0 — 장치 사이 대조가 자리 위치로 찾는다).
    const usize slotBytes = static_cast<usize>(SceneCaptureWidth) * SceneCaptureHeight * 4;
    out.Clear();
    if (out.TryResize(slotBytes * (SceneCount + 1))) {
        for (uint32 slot = 0; slot <= SceneCount; ++slot) {
            const Array<uint8>& pixels = screen.Renderer().CapturedPixels(slot);
            for (usize index = 0; index < pixels.Size() && index < slotBytes; ++index) {
                out[slot * slotBytes + index] = pixels[index];
            }
        }
    }

    printf("  %s texture scenes (%s): %u textures ready, %u checks, %llu fallback frames\n", name,
           modeName, ready, static_cast<uint32>(checks.Size()),
           static_cast<unsigned long long>(screen.Renderer().Stats().inspectorFallbacks));
    return true;
}

} // namespace

uint32 RunTextureSceneChecks(const char* name, graphics::Device& device,
                             Allocator& allocator) noexcept {
    failures = 0;
    if (const char* reason = WindowPresentUnavailableReason(device)) {
        printf("  [NOT RUN] %s texture scenes: %s\n", name, reason);
        return 0;
    }

    if (!screen::CanOpenEngineShaders(shaders::Host())) {
        printf("  [NOT RUN] %s texture scenes: no shader provider (Release)\n", name);
        return 0;
    }

    Array<SceneCheck> checks(allocator);
    Array<uint8> thread(allocator);
    Array<uint8> inline_(allocator);
    const bool threadRan =
        RunMode(name, render::RenderLaneMode::Thread, device, allocator, checks, thread);
    const bool inlineRan =
        RunMode(name, render::RenderLaneMode::Inline, device, allocator, checks, inline_);
    if (threadRan && inlineRan) {
        bool same = thread.Size() == inline_.Size();
        for (usize index = 0; same && index < thread.Size(); ++index) {
            same = thread[index] == inline_[index];
        }

        Check(same, name, "thread and inline lane modes produce the same pixels");
    }

    if (threadRan) {
        CompareWithReferenceBackend("texture scenes", name, checks.View(), thread.View(),
                                    SceneCount + 1, failures);
    }

    printf("  [%s] %s texture scenes: five dimensions and representative formats match the CPU "
           "reference\n",
           failures == 0 ? "PASS" : "FAIL", name);
    return failures;
}

} // namespace sandbox
