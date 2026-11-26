#include <Base/InitializerList.h>
#include <Base/NumericLimits.h>
#include <Core/Math/Float16.h>
#include <Core/Memory/SystemAllocator.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Meta/GeneratedType.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Memory.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <Texture/Texture.generated.h>
#include <TextureTools/TextureBuild.h>

#include <string.h>

// 텍스처 빌드 시험이다(Docs/Design/AssetSystem 12.13절 — 14.1.99절 대조표, 규칙은 UE 5.8에
// 맞춘 12.5절): mip 표(2×2 상자 — 홀수 끝 버림·길이 1 축, Volume slice 평균, 2DArray, Unfiltered),
// PowerOfTwoMode(Pad sRGB 채움 색·테두리·Volume Z·Square, Stretch 올림, Resize 0 축), AdaptiveSharp
// 재표본 기준값, 최대 크기·쿠킹 편향(UE CalculateLODBias), 선형화, 출력 형식·swizzle·sRGB 확장,
// SrgbIgnored, 양자화 경계, PreserveBorder·NormalizeNormals, 진단, 결정성, 할당 실패.
using namespace rocks;
namespace texture_build_tests_file {

using graphics::PixelFormat;

namespace {

// 선형 G8 원본(srgb off — 값 v / 255 그대로) 설정이다.
TextureBuildSettings Gray(uint32 x, uint32 y, uint32 slices = 1) noexcept {
    TextureBuildSettings settings;
    settings.format = TextureSourceFormat::G8;
    settings.sizeX = x;
    settings.sizeY = y;
    settings.numSlices = slices;
    settings.srgb = false;
    return settings;
}

struct Built {
    explicit Built(Allocator& allocator) noexcept : result(allocator) {
    }

    TextureBuildResult result;
    bool ok = false;
};

bool Build(Allocator& allocator, const TextureBuildSettings& settings, Span<const uint8> pixels,
           Built& out) noexcept {
    out.ok = static_cast<bool>(BuildTexture(allocator, settings, pixels, out.result));
    return out.ok;
}

bool HasWarning(const TextureBuildResult& result, AssetDiagnosticCode code) noexcept {
    for (const TextureBuildWarning& warning : result.warnings) {
        if (warning.code == code) {
            return true;
        }
    }

    return false;
}

uint16 Le16(const Array<uint8>& bytes, usize at) noexcept {
    return static_cast<uint16>(bytes[at] | (bytes[at + 1] << 8));
}

} // namespace

// SimpleAverage(12.5절 mip 규칙 — UE 2×2 상자): 4×4, 5×3(홀수 끝 열·행 버림), 1×7(길이 1 축은 같은
// texel 둘), 사슬 끝 1×1, 출력 R8(Default G8 — swizzle rrr1).
TEST(TextureBuild_SimpleAverage) {
    SystemAllocator allocator;
    {
        const uint8 pixels[16] = {0,   20,  40,  60,  20,  40,  60,  80,
                                  100, 120, 140, 160, 120, 140, 160, 180};
        Built built(allocator);
        REQUIRE(Build(allocator, Gray(4, 4), pixels, built));
        CHECK(built.result.pixelFormat == graphics::PixelFormat::R8Unorm && !built.result.srgb &&
              built.result.swizzle[1] == TextureSwizzle::R &&
              built.result.swizzle[3] == TextureSwizzle::One);
        REQUIRE(built.result.mips.Size() == 3);
        CHECK(memcmp(built.result.mips[0].bytes.Data(), pixels, 16) == 0);
        const Array<uint8>& mip1 = built.result.mips[1].bytes;
        REQUIRE(mip1.Size() == 4);
        CHECK(mip1[0] == 20 && mip1[1] == 60 && mip1[2] == 120 && mip1[3] == 160);
        CHECK(built.result.mips[2].bytes[0] == 90 && built.result.mips[2].sizeX == 1);
    }

    {
        // 행이 같은 5×3: x는 [0, 50, 100, 150, 200].
        uint8 pixels[15];
        for (uint32 y = 0; y < 3; ++y) {
            for (uint32 x = 0; x < 5; ++x) {
                pixels[y * 5 + x] = static_cast<uint8>(x * 50);
            }
        }

        Built built(allocator);
        REQUIRE(Build(allocator, Gray(5, 3), pixels, built));
        REQUIRE(built.result.mips.Size() == 3);
        CHECK(built.result.mips[1].sizeX == 2 && built.result.mips[1].sizeY == 1);
        CHECK(built.result.mips[1].bytes[0] == 25 && built.result.mips[1].bytes[1] == 125);
        CHECK(built.result.mips[2].bytes[0] == 75);
    }

    {
        // 1×7: y는 10씩. mip 1 = (5, 25, 45), mip 2 = 15(마지막 행은 버린다).
        uint8 pixels[7];
        for (uint32 y = 0; y < 7; ++y) {
            pixels[y] = static_cast<uint8>(y * 10);
        }

        Built built(allocator);
        REQUIRE(Build(allocator, Gray(1, 7), pixels, built));
        REQUIRE(built.result.mips.Size() == 3);
        CHECK(built.result.mips[1].sizeX == 1 && built.result.mips[1].sizeY == 3);
        CHECK(built.result.mips[1].bytes[0] == 5 && built.result.mips[1].bytes[1] == 25 &&
              built.result.mips[1].bytes[2] == 45);
        CHECK(built.result.mips[2].bytes[0] == 15);
    }
}

// Volume(slice 2z·2z + 1의 2D 상자를 (a + b) × 0.5 — 2×2×2, 3×3×3은 끝 slice·행·열 버림), 2DArray는
// slice마다 X·Y만, Unfiltered는 (2i, 2j)(Volume slice 2z) 표본.
TEST(TextureBuild_VolumeArrayAndUnfiltered) {
    SystemAllocator allocator;
    {
        const uint8 voxels[8] = {0, 10, 20, 30, 40, 50, 60, 70};
        TextureBuildSettings settings = Gray(2, 2, 2);
        settings.dimension = TextureDimension::Volume;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, voxels, built));
        REQUIRE(built.result.mips.Size() == 2);
        CHECK(built.result.numSlices == 1 && built.result.mips[0].sizeZ == 2 &&
              built.result.mips[1].sizeZ == 1 && built.result.mips[1].bytes[0] == 35);
    }

    {
        // 3×3×3 균일 27 + 가운데 0: slice 0 상자 27, slice 1 상자 (27 × 3 + 0) / 4 = 20.25 → 23.625
        // → 24.
        uint8 voxels[27];
        for (uint8& voxel : voxels) {
            voxel = 27;
        }

        voxels[13] = 0;
        TextureBuildSettings settings = Gray(3, 3, 3);
        settings.dimension = TextureDimension::Volume;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, voxels, built));
        REQUIRE(built.result.mips.Size() == 2);
        CHECK(built.result.mips[1].bytes[0] == 24);
    }

    {
        const uint8 slices[8] = {10, 20, 30, 40, 50, 60, 70, 80};
        TextureBuildSettings settings = Gray(2, 2, 2);
        settings.dimension = TextureDimension::Texture2DArray;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, slices, built));
        REQUIRE(built.result.mips.Size() == 2);
        CHECK(built.result.numSlices == 2 && built.result.mips[1].bytes.Size() == 2 &&
              built.result.mips[1].bytes[0] == 25 && built.result.mips[1].bytes[1] == 65);
    }

    {
        const uint8 pixels[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        TextureBuildSettings settings = Gray(4, 4);
        settings.mipGenSettings = TextureMipGenSettings::Unfiltered;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, pixels, built));
        const Array<uint8>& mip1 = built.result.mips[1].bytes;
        CHECK(mip1[0] == 1 && mip1[1] == 3 && mip1[2] == 9 && mip1[3] == 11);
        CHECK(built.result.mips[2].bytes[0] == 1);
    }
}

// PowerOfTwoMode(UE GetPowerOfTwoTargetTextureSize): Pad(채움 색은 8비트 sRGB 디코딩 — srgb on
// 출력이면 같은 값으로 돌아온다, 테두리 texel, PadToSquare, Volume Z), Stretch(올림 — 3 → 4, 5 → 8,
// Square, Volume은 X·Y만), Resize(0 축은 원래 길이), Cube 정사각을 깨는 Resize는 InvalidSetting,
// 비정사각 Cube 원본은 InvalidSource.
TEST(TextureBuild_PowerOfTwoMode) {
    SystemAllocator allocator;
    const uint8 pixels[6] = {10, 20, 30, 40, 50, 60};
    {
        TextureBuildSettings settings = Gray(3, 2);
        settings.srgb = true;
        settings.powerOfTwoMode = TexturePowerOfTwoSetting::PadToPowerOfTwo;
        settings.paddingColor = Color(200, 0, 0, 255);
        settings.mipGenSettings = TextureMipGenSettings::NoMipmaps;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, pixels, built));
        REQUIRE(built.result.mips.Size() == 1);
        CHECK(built.result.pixelFormat == graphics::PixelFormat::RGBA8UnormSrgb);
        const Array<uint8>& top = built.result.mips[0].bytes;
        CHECK(built.result.mips[0].sizeX == 4 && built.result.mips[0].sizeY == 2);
        CHECK(top[0] == 10 && top[2 * 4] == 30 && top[3 * 4] == 200 && top[7 * 4] == 200);
        settings.padWithBorderColor = true;
        REQUIRE(Build(allocator, settings, pixels, built));
        CHECK(built.result.mips[0].bytes[3 * 4] == 30 && built.result.mips[0].bytes[7 * 4] == 60);
        settings.powerOfTwoMode = TexturePowerOfTwoSetting::PadToSquarePowerOfTwo;
        REQUIRE(Build(allocator, settings, pixels, built));
        CHECK(built.result.mips[0].sizeX == 4 && built.result.mips[0].sizeY == 4 &&
              built.result.mips[0].bytes[15 * 4] == 60);
    }

    {
        // Volume Pad는 Z도 늘린다(2×2×3 → 4×4×4 Square, 새 slice는 앞 slice 테두리).
        uint8 voxels[12];
        for (usize index = 0; index < 12; ++index) {
            voxels[index] = static_cast<uint8>(index * 10);
        }

        TextureBuildSettings settings = Gray(2, 2, 3);
        settings.dimension = TextureDimension::Volume;
        settings.powerOfTwoMode = TexturePowerOfTwoSetting::PadToSquarePowerOfTwo;
        settings.padWithBorderColor = true;
        settings.mipGenSettings = TextureMipGenSettings::NoMipmaps;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, voxels, built));
        CHECK(built.result.mips[0].sizeX == 4 && built.result.mips[0].sizeY == 4 &&
              built.result.mips[0].sizeZ == 4);
        // slice 3(새)의 (3, 3)은 slice 2의 (1, 1) = 110.
        CHECK(built.result.mips[0].bytes[3 * 16 + 15] == 110);
        settings.powerOfTwoMode = TexturePowerOfTwoSetting::StretchToPowerOfTwo;
        REQUIRE(Build(allocator, settings, voxels, built));
        CHECK(built.result.mips[0].sizeX == 2 && built.result.mips[0].sizeZ == 3);
    }

    {
        TextureBuildSettings settings = Gray(3, 2);
        settings.powerOfTwoMode = TexturePowerOfTwoSetting::StretchToPowerOfTwo;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, pixels, built));
        CHECK(built.result.mips[0].sizeX == 4 && built.result.mips[0].sizeY == 2);
        settings.powerOfTwoMode = TexturePowerOfTwoSetting::StretchToSquarePowerOfTwo;
        REQUIRE(Build(allocator, settings, pixels, built));
        CHECK(built.result.mips[0].sizeX == 4 && built.result.mips[0].sizeY == 4);
        const uint8 five[5] = {9, 9, 9, 9, 9};
        TextureBuildSettings wide = Gray(5, 1);
        wide.powerOfTwoMode = TexturePowerOfTwoSetting::StretchToPowerOfTwo;
        REQUIRE(Build(allocator, wide, five, built));
        CHECK(built.result.mips[0].sizeX == 8 && built.result.mips[0].bytes[0] == 9 &&
              built.result.mips[0].bytes[7] == 9);
    }

    {
        TextureBuildSettings settings = Gray(3, 2);
        settings.powerOfTwoMode = TexturePowerOfTwoSetting::ResizeToSpecificResolution;
        settings.resizeX = 6;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, pixels, built));
        CHECK(built.result.mips[0].sizeX == 6 && built.result.mips[0].sizeY == 2);
    }

    {
        uint8 faces[6 * 4];
        for (uint8& value : faces) {
            value = 5;
        }

        TextureBuildSettings settings = Gray(2, 2, 6);
        settings.dimension = TextureDimension::TextureCube;
        settings.powerOfTwoMode = TexturePowerOfTwoSetting::ResizeToSpecificResolution;
        settings.resizeX = 4;
        Built built(allocator);
        CHECK(!Build(allocator, settings, faces, built));
        CHECK(built.result.failure.code == AssetDiagnosticCode::InvalidSetting);
        TextureBuildSettings oblong = Gray(2, 3, 6);
        oblong.dimension = TextureDimension::TextureCube;
        uint8 tall[6 * 6] = {};
        CHECK(!Build(allocator, oblong, tall, built));
        CHECK(built.result.failure.code == AssetDiagnosticCode::InvalidSource);
    }
}

// AdaptiveSharp 재표본 기준값(Python 같은 식 — 14.1.99절, 가장자리 clamp): [0, 1] 2 → 4 확대
// Catmull-Rom (0, 52, 203, 255 — 출력 양자화가 음수 lobe를 0으로), [0, 1, 0, 1, 0] 5 → 4 축소
// Mitchell (63, 144, 144, 63), 같은 길이 축은 복사, 재표본 넘침은 mip 평균까지 남고, float 출력은
// 음수를 남긴다.
TEST(TextureBuild_AdaptiveSharp) {
    SystemAllocator allocator;
    const auto resize = [&allocator](Span<const uint8> pixels, uint32 sourceX, uint32 targetX,
                                     uint32 targetY, Built& built) noexcept {
        TextureBuildSettings settings = Gray(sourceX, 1);
        settings.powerOfTwoMode = TexturePowerOfTwoSetting::ResizeToSpecificResolution;
        settings.resizeX = targetX;
        settings.resizeY = targetY;
        settings.mipGenSettings = TextureMipGenSettings::NoMipmaps;
        return Build(allocator, settings, pixels, built);
    };
    const uint8 step[2] = {0, 255};
    Built built(allocator);
    REQUIRE(resize(step, 2, 4, 1, built));
    const uint8 up[4] = {0, 52, 203, 255};
    CHECK(memcmp(built.result.mips[0].bytes.Data(), up, 4) == 0);
    const uint8 wave[5] = {0, 255, 0, 255, 0};
    REQUIRE(resize(wave, 5, 4, 1, built));
    const uint8 down[4] = {63, 144, 144, 63};
    CHECK(memcmp(built.result.mips[0].bytes.Data(), down, 4) == 0);
    // 같은 길이 축은 point 표본(복사 — UE FilterIsNopWhenSameSize): 4 → 4는 그대로, Y만 1 → 2.
    const uint8 same[4] = {0, 255, 0, 255};
    REQUIRE(resize(same, 4, 4, 2, built));
    CHECK(built.result.mips[0].sizeY == 2 &&
          memcmp(built.result.mips[0].bytes.Data(), same, 4) == 0 &&
          memcmp(built.result.mips[0].bytes.Data() + 4, same, 4) == 0);

    // 재표본은 자르지 않는다: 2 → 4 확대의 넘침(−0.0703, 1.0703)이 mip 평균에 들어간다(mip 1 = 17,
    // 238 — 잘랐다면 26, 229), mip 2 = 128.
    TextureBuildSettings overshoot = Gray(2, 1);
    overshoot.powerOfTwoMode = TexturePowerOfTwoSetting::ResizeToSpecificResolution;
    overshoot.resizeX = 4;
    REQUIRE(Build(allocator, overshoot, step, built));
    REQUIRE(built.result.mips.Size() == 3);
    CHECK(built.result.mips[1].bytes[0] == 17 && built.result.mips[1].bytes[1] == 238 &&
          built.result.mips[2].bytes[0] == 128);

    TextureBuildSettings floating = Gray(2, 1);
    floating.compression = TextureCompressionSettings::HalfFloat;
    floating.powerOfTwoMode = TexturePowerOfTwoSetting::ResizeToSpecificResolution;
    floating.resizeX = 4;
    floating.mipGenSettings = TextureMipGenSettings::NoMipmaps;
    REQUIRE(Build(allocator, floating, step, built));
    CHECK(Float16::FromBits(Le16(built.result.mips[0].bytes, 0)).ToFloat() < 0.0f);
}

// 최대 크기(12.5절 4번)와 쿠킹 편향(7번 — UE CalculateLODBias): LODBias(텍스처 + 그룹 합은
// 호출자·음수), MaxTextureSize(재표본 한 번) + LODBias는 더해진다, 그룹 MaxLODSize·MinLODSize는
// CeilLog2(2의 거듭제곱 아님·Min > Max면 Min), NoMipmaps는 편향 없음(그룹 MaxLODSize 무시,
// MaxTextureSize만), Volume은 X·Y로 편향을 정하고 사슬은 Z 포함.
TEST(TextureBuild_LodComposition) {
    SystemAllocator allocator;
    Array<uint8> pixels(allocator);
    REQUIRE(pixels.TryResize(256 * 256));
    for (uint8& value : pixels) {
        value = 100;
    }

    struct Case {
        int32 lodBias;
        int32 maxTextureSize;
        int32 maxLodSize;
        int32 minLodSize;
        bool noMips;
        uint32 top;
        usize mips;
    };
    const Case cases[] = {
        {0, 0, 4096, 1, false, 256, 9},   {2, 0, 4096, 1, false, 64, 7},
        {0, 100, 4096, 1, false, 64, 7},  {1, 128, 4096, 1, false, 64, 7},
        {0, 0, 128, 1, false, 128, 8},    {0, 0, 100, 1, false, 128, 8},
        {3, 0, 4096, 128, false, 128, 8}, {9, 0, 4096, 1, false, 1, 1},
        {-2, 0, 4096, 1, false, 256, 9},  {0, 0, 64, 256, false, 64, 7},
        {3, 0, 64, 256, false, 256, 9},   {0, 64, 4096, 1, true, 64, 1},
        {4, 0, 4096, 1, true, 256, 1},    {0, 0, 64, 1, true, 256, 1},
    };
    for (const Case& item : cases) {
        TextureBuildSettings settings = Gray(256, 256);
        settings.lodBias = item.lodBias;
        settings.maxTextureSize = item.maxTextureSize;
        settings.maxLodSize = item.maxLodSize;
        settings.minLodSize = item.minLodSize;
        settings.mipGenSettings =
            item.noMips ? TextureMipGenSettings::NoMipmaps : TextureMipGenSettings::SimpleAverage;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, pixels.View(), built));
        CHECK(built.result.mips.Size() == item.mips && built.result.mips[0].sizeX == item.top);
        CHECK(built.result.mips[0].bytes[0] == 100);
    }

    // Volume 2×2×8: 사슬 4(Z 포함), 편향은 X·Y(TextureMaxLOD 1) — LODBias 1이면 상위 하나를 버린다.
    uint8 voxels[32] = {};
    TextureBuildSettings volume = Gray(2, 2, 8);
    volume.dimension = TextureDimension::Volume;
    volume.lodBias = 1;
    Built built(allocator);
    REQUIRE(Build(allocator, volume, voxels, built));
    CHECK(built.result.mips.Size() == 3 && built.result.mips[0].sizeX == 1 &&
          built.result.mips[0].sizeZ == 4);
}

// 선형화(12.2절 1번)를 HalfFloat 출력(선형 값 그대로 16F)으로 본다: 8비트 srgb on = sRGB 표, legacy
// gamma = x^2.2, srgb off = v / 255, override Linear·sRGB, 16비트는 SRGB와 무관하게 선형(override
// sRGB만 구간 함수), 알파는 늘 선형. flipGreen은 g → 1 − g.
TEST(TextureBuild_Linearization) {
    SystemAllocator allocator;
    const auto half = [](float value) noexcept { return Float16::FromFloat(value).bits; };
    const uint8 gray[1] = {128};
    struct Case {
        bool srgb;
        bool legacy;
        TextureSourceEncoding encoding;
        float expected;
    };
    const Case cases[] = {
        {true, false, TextureSourceEncoding::None, deterministic::SrgbByteToLinear(128)},
        {true, true, TextureSourceEncoding::None,
         static_cast<float>(deterministic::Gamma22ToLinear(128.0 / 255.0))},
        {false, false, TextureSourceEncoding::None, static_cast<float>(128.0 / 255.0)},
        {true, false, TextureSourceEncoding::Linear, static_cast<float>(128.0 / 255.0)},
        {false, false, TextureSourceEncoding::sRGB, deterministic::SrgbByteToLinear(128)},
    };
    for (const Case& item : cases) {
        TextureBuildSettings settings = Gray(1, 1);
        settings.compression = TextureCompressionSettings::HalfFloat;
        settings.srgb = item.srgb;
        settings.useLegacyGamma = item.legacy;
        settings.encodingOverride = item.encoding;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, gray, built));
        CHECK(built.result.pixelFormat == graphics::PixelFormat::R16Float &&
              Le16(built.result.mips[0].bytes, 0) == half(item.expected));
    }

    // 16비트: srgb on이어도 선형, override sRGB는 구간 함수.
    const uint8 wide[2] = {0x00, 0x80};
    TextureBuildSettings settings = Gray(1, 1);
    settings.format = TextureSourceFormat::G16;
    settings.compression = TextureCompressionSettings::HalfFloat;
    settings.srgb = true;
    Built built(allocator);
    REQUIRE(Build(allocator, settings, wide, built));
    CHECK(Le16(built.result.mips[0].bytes, 0) == half(static_cast<float>(32768.0 / 65535.0)));
    settings.encodingOverride = TextureSourceEncoding::sRGB;
    REQUIRE(Build(allocator, settings, wide, built));
    CHECK(Le16(built.result.mips[0].bytes, 0) ==
          half(static_cast<float>(deterministic::SrgbToLinear(32768.0 / 65535.0))));

    // RGBA8 알파 선형 + flipGreen(HDR — RGBA16F, srgb 무시).
    const uint8 rgba[4] = {255, 64, 0, 128};
    TextureBuildSettings color = Gray(1, 1);
    color.format = TextureSourceFormat::RGBA8;
    color.compression = TextureCompressionSettings::HDR;
    color.srgb = true;
    color.flipGreenChannel = true;
    REQUIRE(Build(allocator, color, rgba, built));
    CHECK(built.result.pixelFormat == graphics::PixelFormat::RGBA16Float);
    CHECK(Le16(built.result.mips[0].bytes, 2) == half(1.0f - deterministic::SrgbByteToLinear(64)));
    CHECK(Le16(built.result.mips[0].bytes, 6) == half(static_cast<float>(128.0 / 255.0)));
    CHECK(HasWarning(built.result, AssetDiagnosticCode::SrgbIgnored));
}

// 출력(12.4.3절 표): 종류 × 원본의 형식·swizzle·채널 값, GA 원본의 RG = (회색, 알파), sRGB R8·RG8을
// RGBA8 sRGB로 넓힘(swizzle 항등), RGBA8 sRGB, HDR_Compressed 알파 1, DistanceFieldFont는 늘 R8,
// SrgbIgnored(선형 종류·float 출력 — 16비트 정수 출력은 경고 없음).
TEST(TextureBuild_OutputFormats) {
    SystemAllocator allocator;
    struct Case {
        TextureCompressionSettings compression;
        TextureSourceFormat format;
        graphics::PixelFormat expected;
        TextureSwizzle swizzleG;
        TextureSwizzle swizzleA;
    };
    const Case cases[] = {
        {TextureCompressionSettings::Default, TextureSourceFormat::G8,
         graphics::PixelFormat::R8Unorm, TextureSwizzle::R, TextureSwizzle::One},
        {TextureCompressionSettings::Default, TextureSourceFormat::GA8,
         graphics::PixelFormat::RG8Unorm, TextureSwizzle::R, TextureSwizzle::G},
        {TextureCompressionSettings::Default, TextureSourceFormat::RGBA8,
         graphics::PixelFormat::RGBA8Unorm, TextureSwizzle::G, TextureSwizzle::A},
        {TextureCompressionSettings::Default, TextureSourceFormat::G16,
         graphics::PixelFormat::R16Unorm, TextureSwizzle::R, TextureSwizzle::One},
        {TextureCompressionSettings::Default, TextureSourceFormat::RGBA16,
         graphics::PixelFormat::RGBA16Unorm, TextureSwizzle::G, TextureSwizzle::A},
        {TextureCompressionSettings::Default, TextureSourceFormat::R32F,
         graphics::PixelFormat::R16Float, TextureSwizzle::R, TextureSwizzle::One},
        {TextureCompressionSettings::Normalmap, TextureSourceFormat::G8,
         graphics::PixelFormat::RGBA8Unorm, TextureSwizzle::G, TextureSwizzle::A},
        {TextureCompressionSettings::Masks, TextureSourceFormat::RGBA16,
         graphics::PixelFormat::RGBA16Unorm, TextureSwizzle::G, TextureSwizzle::A},
        // Grayscale·Alpha·DistanceFieldFont는 UE sampler type 계약 .rrrr(12.16.4).
        {TextureCompressionSettings::Grayscale, TextureSourceFormat::RGBA8,
         graphics::PixelFormat::R8Unorm, TextureSwizzle::R, TextureSwizzle::R},
        {TextureCompressionSettings::Grayscale, TextureSourceFormat::RGBA16F,
         graphics::PixelFormat::R16Float, TextureSwizzle::R, TextureSwizzle::R},
        {TextureCompressionSettings::Displacementmap, TextureSourceFormat::G16,
         graphics::PixelFormat::R16Unorm, TextureSwizzle::Zero, TextureSwizzle::One},
        {TextureCompressionSettings::VectorDisplacementmap, TextureSourceFormat::RGBA8,
         graphics::PixelFormat::RGBA8Unorm, TextureSwizzle::G, TextureSwizzle::A},
        {TextureCompressionSettings::HDR, TextureSourceFormat::G8,
         graphics::PixelFormat::RGBA16Float, TextureSwizzle::G, TextureSwizzle::A},
        {TextureCompressionSettings::EditorIcon, TextureSourceFormat::RGBA32F,
         graphics::PixelFormat::RGBA8Unorm, TextureSwizzle::G, TextureSwizzle::A},
        {TextureCompressionSettings::Alpha, TextureSourceFormat::RGBA8,
         graphics::PixelFormat::R8Unorm, TextureSwizzle::R, TextureSwizzle::R},
        {TextureCompressionSettings::DistanceFieldFont, TextureSourceFormat::G16,
         graphics::PixelFormat::R8Unorm, TextureSwizzle::R, TextureSwizzle::R},
        {TextureCompressionSettings::HDR_Compressed, TextureSourceFormat::RGBA8,
         graphics::PixelFormat::RGBA16Float, TextureSwizzle::G, TextureSwizzle::A},
        {TextureCompressionSettings::BC7, TextureSourceFormat::G8,
         graphics::PixelFormat::RGBA8Unorm, TextureSwizzle::G, TextureSwizzle::A},
        {TextureCompressionSettings::HalfFloat, TextureSourceFormat::RGBA8,
         graphics::PixelFormat::R16Float, TextureSwizzle::Zero, TextureSwizzle::One},
        {TextureCompressionSettings::SingleFloat, TextureSourceFormat::G8,
         graphics::PixelFormat::R32Float, TextureSwizzle::Zero, TextureSwizzle::One},
        {TextureCompressionSettings::HDR_F32, TextureSourceFormat::RGBA16,
         graphics::PixelFormat::RGBA32Float, TextureSwizzle::G, TextureSwizzle::A},
    };
    uint8 source[16] = {};
    for (const Case& item : cases) {
        TextureBuildSettings settings = Gray(1, 1);
        settings.format = item.format;
        settings.compression = item.compression;
        Built built(allocator);
        REQUIRE(Build(allocator, settings,
                      Span<const uint8>(source, TextureSourceTexelBytes(item.format)), built));
        CHECK(built.result.pixelFormat == item.expected &&
              built.result.swizzle[1] == item.swizzleG && built.result.swizzle[3] == item.swizzleA);
    }

    // GA8 → RG8 = (회색, 알파).
    const uint8 ga[2] = {77, 33};
    TextureBuildSettings settings = Gray(1, 1);
    settings.format = TextureSourceFormat::GA8;
    Built built(allocator);
    REQUIRE(Build(allocator, settings, ga, built));
    CHECK(built.result.mips[0].bytes[0] == 77 && built.result.mips[0].bytes[1] == 33);

    // sRGB 확장: G8 Default srgb on → RGBA8 sRGB (128, 128, 128, 255), GA8 → (77, 77, 77, 33).
    settings.format = TextureSourceFormat::G8;
    settings.srgb = true;
    const uint8 gray[1] = {128};
    REQUIRE(Build(allocator, settings, gray, built));
    CHECK(built.result.pixelFormat == graphics::PixelFormat::RGBA8UnormSrgb && built.result.srgb &&
          built.result.swizzle[1] == TextureSwizzle::G &&
          built.result.swizzle[3] == TextureSwizzle::A);
    const uint8 expandedGray[4] = {128, 128, 128, 255};
    CHECK(memcmp(built.result.mips[0].bytes.Data(), expandedGray, 4) == 0);
    settings.format = TextureSourceFormat::GA8;
    REQUIRE(Build(allocator, settings, ga, built));
    const uint8 expandedGa[4] = {77, 77, 77, 33};
    CHECK(memcmp(built.result.mips[0].bytes.Data(), expandedGa, 4) == 0);
    CHECK(built.result.warnings.IsEmpty());

    // SrgbIgnored: 선형 종류(Normalmap), float 출력(Default R32F). 16비트 정수 출력은 경고 없음.
    settings.format = TextureSourceFormat::RGBA8;
    settings.compression = TextureCompressionSettings::Normalmap;
    const uint8 rgba[4] = {1, 2, 3, 4};
    REQUIRE(Build(allocator, settings, rgba, built));
    CHECK(built.result.pixelFormat == graphics::PixelFormat::RGBA8Unorm &&
          HasWarning(built.result, AssetDiagnosticCode::SrgbIgnored) &&
          built.result.warnings.Size() == 1);
    settings.format = TextureSourceFormat::R32F;
    settings.compression = TextureCompressionSettings::Default;
    const uint8 single[4] = {0, 0, 0x80, 0x3f};
    REQUIRE(Build(allocator, settings, single, built));
    CHECK(HasWarning(built.result, AssetDiagnosticCode::SrgbIgnored));
    settings.format = TextureSourceFormat::G16;
    const uint8 wide[2] = {0xff, 0xff};
    REQUIRE(Build(allocator, settings, wide, built));
    CHECK(built.result.pixelFormat == graphics::PixelFormat::R16Unorm && !built.result.srgb &&
          built.result.warnings.IsEmpty() && Le16(built.result.mips[0].bytes, 0) == 0xffff);
    settings.format = TextureSourceFormat::RGBA16;
    const uint8 wideColor[8] = {0x00, 0x80, 0, 0, 0, 0, 0xff, 0xff};
    REQUIRE(Build(allocator, settings, wideColor, built));
    CHECK(built.result.pixelFormat == graphics::PixelFormat::RGBA16Unorm &&
          built.result.warnings.IsEmpty() && Le16(built.result.mips[0].bytes, 0) == 0x8000);

    // HDR_Compressed는 알파 1(half 0x3c00).
    settings.format = TextureSourceFormat::RGBA8;
    settings.compression = TextureCompressionSettings::HDR_Compressed;
    settings.srgb = false;
    REQUIRE(Build(allocator, settings, rgba, built));
    CHECK(Le16(built.result.mips[0].bytes, 6) == 0x3c00);
}

// 양자화(12.2절 5번): 정수는 [0, 1] 자름·round-half-up(0.5/255 경계), 16F는 NaN → 0·±Inf와 65504
// 넘음 → ±65504, 32F는 NaN → 0·±Inf → ±float 최대.
TEST(TextureBuild_Quantization) {
    SystemAllocator allocator;
    const float values[4] = {platform::BitCast<float>(0x7fc00000u),
                             platform::BitCast<float>(0x7f800000u), 70000.0f,
                             platform::BitCast<float>(0xff800000u)};
    uint8 pixels[16];
    platform::MemoryCopy(pixels, values, sizeof(values));
    TextureBuildSettings settings = Gray(1, 1);
    settings.format = TextureSourceFormat::RGBA32F;
    settings.compression = TextureCompressionSettings::HDR;
    Built built(allocator);
    REQUIRE(Build(allocator, settings, pixels, built));
    const Array<uint8>& halfBytes = built.result.mips[0].bytes;
    CHECK(Le16(halfBytes, 0) == 0 && Le16(halfBytes, 2) == 0x7bff && Le16(halfBytes, 4) == 0x7bff &&
          Le16(halfBytes, 6) == 0xfbff);
    settings.compression = TextureCompressionSettings::HDR_F32;
    REQUIRE(Build(allocator, settings, pixels, built));
    float stored[4];
    platform::MemoryCopy(stored, built.result.mips[0].bytes.Data(), sizeof(stored));
    CHECK(stored[0] == 0.0f && stored[1] == NumericLimits<float>::Max() && stored[2] == 70000.0f &&
          stored[3] == -NumericLimits<float>::Max());
    settings.compression = TextureCompressionSettings::EditorIcon;
    REQUIRE(Build(allocator, settings, pixels, built));
    CHECK(built.result.mips[0].bytes[0] == 0 && built.result.mips[0].bytes[1] == 255 &&
          built.result.mips[0].bytes[3] == 0);

    // UE float 식 (uint8)(0.5f + v × 255.0f): 0.5/255는 1, 그 바로 아래 float도 1(0.5 +
    // 0.49999997이 float로 1.0 — UE와 같다), 0.49/255는 0. float 원본에서 8비트 출력(R8)을 내는
    // 설정은 DistanceFieldFont다(Grayscale은 float 원본이면 R16F — 위 출력 규칙).
    const float half = static_cast<float>(0.5 / 255.0);
    uint8 edge[4];
    platform::MemoryCopy(edge, &half, sizeof(half));
    TextureBuildSettings gray = Gray(1, 1);
    gray.format = TextureSourceFormat::R32F;
    gray.compression = TextureCompressionSettings::DistanceFieldFont;
    gray.srgb = false;
    REQUIRE(Build(allocator, gray, edge, built));
    CHECK(built.result.mips[0].bytes[0] == 1);
    const float below = platform::BitCast<float>(platform::BitCast<uint32>(half) - 1);
    platform::MemoryCopy(edge, &below, sizeof(below));
    REQUIRE(Build(allocator, gray, edge, built));
    CHECK(built.result.mips[0].bytes[0] == 1);
    const float under = static_cast<float>(0.49 / 255.0);
    platform::MemoryCopy(edge, &under, sizeof(under));
    REQUIRE(Build(allocator, gray, edge, built));
    CHECK(built.result.mips[0].bytes[0] == 0);
}

// 1채널 float 원본의 풀기(12.13절 — UE CopyImage, 2026-11-25 A5j2b에서 고침): R32F·R16F 원본은
// (v, 0, 0, 1)이라 RGBA16F 출력(HDR)의 G·B는 0, A는 1이다(회색 원본처럼 복제하지 않는다).
TEST(TextureBuild_SingleChannelFloatDecode) {
    SystemAllocator allocator;
    constexpr uint16 HalfHalf = 0x3800;
    constexpr uint16 HalfOne = 0x3C00;
    const auto load16 = [](const uint8* data) noexcept {
        return static_cast<uint16>(data[0] | (data[1] << 8));
    };
    TextureBuildSettings settings = Gray(1, 1);
    settings.format = TextureSourceFormat::R32F;
    settings.compression = TextureCompressionSettings::HDR;
    settings.srgb = false;
    const float value = 0.5f;
    uint8 single[4];
    platform::MemoryCopy(single, &value, sizeof(value));
    Built built(allocator);
    REQUIRE(Build(allocator, settings, single, built));
    REQUIRE(built.result.pixelFormat == graphics::PixelFormat::RGBA16Float &&
            built.result.mips[0].bytes.Size() == 8);
    const uint8* out = built.result.mips[0].bytes.Data();
    CHECK(load16(out) == HalfHalf && load16(out + 2) == 0 && load16(out + 4) == 0 &&
          load16(out + 6) == HalfOne);

    settings.format = TextureSourceFormat::R16F;
    const uint8 half[2] = {0x00, 0x38};
    REQUIRE(Build(allocator, settings, half, built));
    out = built.result.mips[0].bytes.Data();
    CHECK(load16(out) == HalfHalf && load16(out + 2) == 0 && load16(out + 4) == 0 &&
          load16(out + 6) == HalfOne);
}

// PreserveBorder(UE GenerateMipBorder — 테두리 texel을 직전 mip 2×2 발자국의 원본 테두리 평균으로,
// Wrap이면 경고)와 NormalizeNormals(UE NormalizeMip — 사슬 뒤 mip 0 포함 후처리, 길이 0은 (0, 0,
// 1)).
TEST(TextureBuild_BorderAndNormals) {
    SystemAllocator allocator;
    {
        // 4×4: 테두리 200, 안쪽 0. mip 1(2×2)은 모두 테두리 → 200.
        uint8 pixels[16];
        for (uint32 y = 0; y < 4; ++y) {
            for (uint32 x = 0; x < 4; ++x) {
                pixels[y * 4 + x] = x == 0 || y == 0 || x == 3 || y == 3 ? 200 : 0;
            }
        }

        TextureBuildSettings settings = Gray(4, 4);
        settings.preserveBorder = true;
        settings.addressX = TextureAddress::Clamp;
        settings.addressY = TextureAddress::Clamp;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, pixels, built));
        CHECK(built.result.mips[1].bytes[0] == 200 && built.result.mips[1].bytes[3] == 200);
        CHECK(built.result.warnings.IsEmpty());
        settings.addressY = TextureAddress::Wrap;
        REQUIRE(Build(allocator, settings, pixels, built));
        CHECK(HasWarning(built.result, AssetDiagnosticCode::PreserveBorderWithWrap));
        settings.preserveBorder = false;
        REQUIRE(Build(allocator, settings, pixels, built));
        CHECK(built.result.mips[1].bytes[0] == 150);
    }

    {
        // 2×1 Normalmap: (+x)와 (+y) 법선 → 평균 (½, ½, 0)을 재정규화 → (√½, √½, 0).
        const uint8 normals[8] = {255, 128, 128, 255, 128, 255, 128, 255};
        TextureBuildSettings settings = Gray(2, 1);
        settings.format = TextureSourceFormat::RGBA8;
        settings.compression = TextureCompressionSettings::Normalmap;
        settings.normalizeNormals = true;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, normals, built));
        const Array<uint8>& mip1 = built.result.mips[1].bytes;
        // (√½ + 1) / 2 ≈ 0.8536 → 218.
        CHECK(mip1[0] == 218 && mip1[1] == 218 && mip1[2] < 130);
        // 성분마다 합이 255인 둘의 평균은 정확히 0.5 → 길이 0 → (0, 0, 1) → (128, 128, 255).
        const uint8 opposite[8] = {255, 0, 255, 255, 0, 255, 0, 255};
        REQUIRE(Build(allocator, settings, opposite, built));
        const Array<uint8>& zero = built.result.mips[1].bytes;
        CHECK(zero[0] == 128 && zero[1] == 128 && zero[2] == 255);
        // mip 0도 정규화한다((255, 0, 255) → (1, −1, 1) / √3 → (201, 54, 201)).
        const Array<uint8>& top = built.result.mips[0].bytes;
        CHECK(top[0] == 201 && top[1] == 54 && top[2] == 201);
    }
}

// 진단과 해석: byte 수 불일치·0 크기 InvalidSource, 숨김 CompressionSettings InvalidSetting,
// Resolve의 slice 수 규칙·비 2D Resize·그룹 계약 mipGen, FromTextureGroup → 그룹 값, LODBias 합,
// NormalizeNormals는 Normalmap만, Cube 주소 Clamp, Volume addressMode.
TEST(TextureBuild_DiagnosticsAndResolve) {
    SystemAllocator allocator;
    const uint8 pixels[4] = {};
    Built built(allocator);
    CHECK(!Build(allocator, Gray(2, 3), pixels, built) &&
          built.result.failure.code == AssetDiagnosticCode::InvalidSource &&
          built.result.failure.detail == 4);
    TextureBuildSettings hidden = Gray(2, 2);
    hidden.compression = static_cast<TextureCompressionSettings>(13);
    CHECK(!Build(allocator, hidden, pixels, built) &&
          built.result.failure.code == AssetDiagnosticCode::InvalidSetting);

    SystemAllocator objects;
    TypeRegistry types(objects);
    ModuleRegistration registration(types, objects);
    REQUIRE(RegisterTextureTypes(registration));
    {
        ObjectRegistry registry(objects, types, ContextId{0x54584255ull, 1});
        auto created =
            registry.NewObject<Texture>(registry.OwnerAccess(), TypeIdOf<VolumeTexture>());
        REQUIRE(created);
        auto borrowed =
            registry.ResolveForWrite<Texture>(registry.OwnerAccess(), created.Value().Handle());
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        Texture& texture = *value.Value();
        texture.source.format = TextureSourceFormat::G8;
        texture.source.sizeX = 2;
        texture.source.sizeY = 2;
        texture.source.numSlices = 2;
        texture.lodBias = 1;
        texture.normalizeNormals = 1;
        static_cast<VolumeTexture&>(texture).addressMode = TextureAddress::Mirror;
        TextureLodGroupValues group;
        group.lodBias = 2;
        group.mipGenSettings = TextureMipGenSettings::Unfiltered;
        TextureBuildSettings settings;
        TextureBuildResult result(allocator);
        REQUIRE(ResolveTextureBuildSettings(texture, TextureDimension::Volume, group, settings,
                                            result));
        CHECK(settings.mipGenSettings == TextureMipGenSettings::Unfiltered &&
              settings.lodBias == 3 && !settings.normalizeNormals &&
              settings.addressZ == TextureAddress::Mirror && settings.numSlices == 2);
        texture.compressionSettings = TextureCompressionSettings::Normalmap;
        REQUIRE(ResolveTextureBuildSettings(texture, TextureDimension::Volume, group, settings,
                                            result));
        CHECK(settings.normalizeNormals);
        // 2 slice는 Cube가 아니다·Texture2D가 아니다.
        CHECK(!ResolveTextureBuildSettings(texture, TextureDimension::TextureCube, group, settings,
                                           result) &&
              result.failure.code == AssetDiagnosticCode::InvalidSource);
        texture.powerOfTwoMode = TexturePowerOfTwoSetting::ResizeToSpecificResolution;
        CHECK(!ResolveTextureBuildSettings(texture, TextureDimension::Volume, group, settings,
                                           result) &&
              result.failure.code == AssetDiagnosticCode::InvalidSetting);
        texture.powerOfTwoMode = TexturePowerOfTwoSetting::None;
        group.mipGenSettings = TextureMipGenSettings::Sharpen5;
        CHECK(
            ResolveTextureBuildSettings(texture, TextureDimension::Volume, group, settings, result)
                    .Error() == ErrorCode::Unsupported &&
            result.failure.code == AssetDiagnosticCode::UnsupportedSetting);
    }

    {
        // CPU에서 읽는 Texture2D는 NoMipmaps다.
        ObjectRegistry registry(objects, types, ContextId{0x54584255ull, 2});
        auto created = registry.NewObject<Texture>(registry.OwnerAccess(), TypeIdOf<Texture2D>());
        REQUIRE(created);
        auto borrowed =
            registry.ResolveForWrite<Texture>(registry.OwnerAccess(), created.Value().Handle());
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        Texture& texture = *value.Value();
        texture.source.format = TextureSourceFormat::G8;
        texture.source.sizeX = 4;
        texture.source.sizeY = 4;
        texture.source.numSlices = 1;
        texture.availability = TextureAvailability::CPU;
        static_cast<Texture2D&>(texture).addressY = TextureAddress::Mirror;
        TextureBuildSettings settings;
        TextureBuildResult result(allocator);
        REQUIRE(ResolveTextureBuildSettings(texture, TextureDimension::Texture2D,
                                            TextureLodGroupValues{}, settings, result));
        CHECK(settings.mipGenSettings == TextureMipGenSettings::NoMipmaps &&
              settings.addressY == TextureAddress::Mirror);
        texture.source.format = TextureSourceFormat::Invalid;
        CHECK(!ResolveTextureBuildSettings(texture, TextureDimension::Texture2D,
                                           TextureLodGroupValues{}, settings, result) &&
              result.failure.code == AssetDiagnosticCode::InvalidSource);
    }

    CHECK(registration.Unregister());

    // 작업 byte 상한: 16385 × 1을 PadToSquare하면 32768² RGBA32F = 16 GiB — 할당 전에 거부한다.
    Array<uint8> line(allocator);
    REQUIRE(line.TryResize(16385));
    TextureBuildSettings huge = Gray(16385, 1);
    huge.powerOfTwoMode = TexturePowerOfTwoSetting::PadToSquarePowerOfTwo;
    Built capped(allocator);
    CHECK(!Build(allocator, huge, line.View(), capped) &&
          capped.result.failure.code == AssetDiagnosticCode::InvalidSource &&
          capped.result.failure.error == ErrorCode::CapacityExceeded);
}

// 결정성(같은 입력 두 번 같은 byte)과 할당 실패 전수(OutOfMemory·진단 None, 또는 성공).
TEST(TextureBuild_DeterminismAndAllocation) {
    SystemAllocator allocator;
    uint8 pixels[5 * 3 * 4];
    for (usize index = 0; index < sizeof(pixels); ++index) {
        pixels[index] = static_cast<uint8>(index * 37);
    }

    TextureBuildSettings settings = Gray(5, 3);
    settings.format = TextureSourceFormat::RGBA8;
    settings.srgb = true;
    settings.powerOfTwoMode = TexturePowerOfTwoSetting::StretchToSquarePowerOfTwo;
    settings.preserveBorder = true;
    Built first(allocator);
    Built second(allocator);
    REQUIRE(Build(allocator, settings, pixels, first));
    REQUIRE(Build(allocator, settings, pixels, second));
    REQUIRE(first.result.mips.Size() == second.result.mips.Size());
    for (usize mip = 0; mip < first.result.mips.Size(); ++mip) {
        CHECK(first.result.mips[mip].bytes.Size() == second.result.mips[mip].bytes.Size() &&
              memcmp(first.result.mips[mip].bytes.Data(), second.result.mips[mip].bytes.Data(),
                     first.result.mips[mip].bytes.Size()) == 0);
    }

    test::NFailAllocator failing(allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        TextureBuildResult result(failing);
        failing.FailOnAttempt(attempt);
        auto built = BuildTexture(failing, settings, pixels, result);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(built);
            break;
        }

        CHECK(!built && built.Error() == ErrorCode::OutOfMemory &&
              result.failure.code == AssetDiagnosticCode::None);
        ++failures;
    }

    CHECK(failures > 0);
}

// flipGreen(UE — Pad·최대 크기 뒤 g → 1 − Clamp(g, 0, 1)): Pad 채움 색의 G도 뒤집히고(0 → 255),
// float 원본의 범위 밖 G는 잘린 뒤 뒤집힌다(2 → 0, −1 → 1). Pad 채움 알파는 a / 255(128 → 128).
TEST(TextureBuild_FlipGreenOrder) {
    SystemAllocator allocator;
    {
        const uint8 pixels[4] = {10, 64, 30, 255};
        TextureBuildSettings settings = Gray(1, 1);
        settings.format = TextureSourceFormat::RGBA8;
        settings.flipGreenChannel = true;
        settings.powerOfTwoMode = TexturePowerOfTwoSetting::PadToSquarePowerOfTwo;
        settings.paddingColor = Color(0, 0, 0, 128);
        settings.mipGenSettings = TextureMipGenSettings::NoMipmaps;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, pixels, built));
        // 1×1은 이미 2의 거듭제곱 — 채움이 없다. 3×1로 다시 본다.
        const uint8 row[12] = {10, 64, 30, 255, 10, 64, 30, 255, 10, 64, 30, 255};
        TextureBuildSettings padded = settings;
        padded.sizeX = 3;
        REQUIRE(Build(allocator, padded, row, built));
        const Array<uint8>& top = built.result.mips[0].bytes;
        CHECK(built.result.mips[0].sizeX == 4 && top[1] == 191 && top[3 * 4 + 1] == 255 &&
              top[3 * 4 + 3] == 128);
    }

    {
        const float texels[8] = {0.0f, 2.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 1.0f};
        uint8 pixels[32];
        platform::MemoryCopy(pixels, texels, sizeof(texels));
        TextureBuildSettings settings = Gray(2, 1);
        settings.format = TextureSourceFormat::RGBA32F;
        settings.compression = TextureCompressionSettings::HDR;
        settings.flipGreenChannel = true;
        settings.mipGenSettings = TextureMipGenSettings::NoMipmaps;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, pixels, built));
        CHECK(Le16(built.result.mips[0].bytes, 2) == 0x0000 &&
              Le16(built.result.mips[0].bytes, 8 + 2) == 0x3c00);
    }
}

// 최대 크기의 2×2 단계 경로(PreserveBorder·Volume — UE GenerateMipChain): PreserveBorder 4×4 →
// 2×2는 테두리 평균, Volume Unfiltered는 (2i, 2j)·slice 2z 표본(mipGen을 따른다).
TEST(TextureBuild_MaxSizeStepPath) {
    SystemAllocator allocator;
    {
        uint8 pixels[16];
        for (uint32 y = 0; y < 4; ++y) {
            for (uint32 x = 0; x < 4; ++x) {
                pixels[y * 4 + x] = x == 0 || y == 0 || x == 3 || y == 3 ? 200 : 0;
            }
        }

        TextureBuildSettings settings = Gray(4, 4);
        settings.preserveBorder = true;
        settings.addressX = TextureAddress::Clamp;
        settings.addressY = TextureAddress::Clamp;
        settings.maxTextureSize = 2;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, pixels, built));
        CHECK(built.result.mips[0].sizeX == 2 && built.result.mips[0].bytes[0] == 200 &&
              built.result.mips[0].bytes[3] == 200);
    }

    {
        uint8 voxels[32];
        for (usize index = 0; index < 32; ++index) {
            voxels[index] = static_cast<uint8>(index);
        }

        TextureBuildSettings settings = Gray(4, 4, 2);
        settings.dimension = TextureDimension::Volume;
        settings.mipGenSettings = TextureMipGenSettings::Unfiltered;
        settings.maxTextureSize = 2;
        Built built(allocator);
        REQUIRE(Build(allocator, settings, voxels, built));
        const Array<uint8>& top = built.result.mips[0].bytes;
        CHECK(built.result.mips[0].sizeX == 2 && built.result.mips[0].sizeZ == 1 && top[0] == 0 &&
              top[1] == 2 && top[2] == 8 && top[3] == 10);
    }
}

// Cube·CubeArray 성공 빌드(면마다 mip — slice 수 유지), 직접 채운 설정의 검사(Cube slice 수·계약
// mipGen), Volume Unfiltered mip(slice 2z만), NormalizeNormals 제곱합 1 유지.
TEST(TextureBuild_CubeAndChecks) {
    SystemAllocator allocator;
    uint8 faces[6 * 4];
    for (usize index = 0; index < sizeof(faces); ++index) {
        faces[index] = static_cast<uint8>(index * 4);
    }

    TextureBuildSettings cube = Gray(2, 2, 6);
    cube.dimension = TextureDimension::TextureCube;
    Built built(allocator);
    REQUIRE(Build(allocator, cube, faces, built));
    REQUIRE(built.result.mips.Size() == 2);
    CHECK(built.result.numSlices == 6 && built.result.mips[1].bytes.Size() == 6 &&
          built.result.mips[1].bytes[5] == 86);
    TextureBuildSettings five = Gray(2, 2, 5);
    five.dimension = TextureDimension::TextureCube;
    uint8 fiveFaces[5 * 4] = {};
    CHECK(!Build(allocator, five, fiveFaces, built) &&
          built.result.failure.code == AssetDiagnosticCode::InvalidSource);
    TextureBuildSettings sharpen = Gray(2, 2);
    sharpen.mipGenSettings = TextureMipGenSettings::Sharpen3;
    const uint8 plain[4] = {};
    CHECK(!Build(allocator, sharpen, plain, built) &&
          built.result.failure.code == AssetDiagnosticCode::UnsupportedSetting);

    const uint8 voxels[8] = {1, 2, 3, 4, 50, 60, 70, 80};
    TextureBuildSettings volume = Gray(2, 2, 2);
    volume.dimension = TextureDimension::Volume;
    volume.mipGenSettings = TextureMipGenSettings::Unfiltered;
    REQUIRE(Build(allocator, volume, voxels, built));
    CHECK(built.result.mips[1].bytes[0] == 1);

    // (1, 0.5, 0.5) float → v = (1, 0, 0), 제곱합 정확히 1 → 그대로.
    const float normal[4] = {1.0f, 0.5f, 0.5f, 1.0f};
    uint8 normalBytes[16];
    platform::MemoryCopy(normalBytes, normal, sizeof(normal));
    TextureBuildSettings normals = Gray(1, 1);
    normals.format = TextureSourceFormat::RGBA32F;
    normals.compression = TextureCompressionSettings::Normalmap;
    normals.normalizeNormals = true;
    REQUIRE(Build(allocator, normals, normalBytes, built));
    CHECK(Le16(built.result.mips[0].bytes, 0) == 0x3c00 &&
          Le16(built.result.mips[0].bytes, 2) == 0x3800 &&
          Le16(built.result.mips[0].bytes, 4) == 0x3800);
}

// Float16 변환(12.13절 결정성): 하드웨어(F16C)·소프트웨어 경로가 같은 bit여야 하는 경계 —
// subnormal·가장 작은 정규수·반올림 동률·NaN을 RGBA16F 출력에서 고정 bit로 본다.
TEST(TextureBuild_HalfConversion) {
    SystemAllocator allocator;
    const float values[4] = {
        platform::BitCast<float>(0x33800000u),  // 2^-24 = 가장 작은 subnormal half
        platform::BitCast<float>(0x38800000u),  // 2^-14 = 가장 작은 정규 half
        platform::BitCast<float>(0x3f801000u),  // 1 + 2^-11 = 동률 → 짝수 0x3c00
        platform::BitCast<float>(0x7fc00001u)}; // NaN → 0
    uint8 pixels[16];
    platform::MemoryCopy(pixels, values, sizeof(values));
    TextureBuildSettings settings = Gray(1, 1);
    settings.format = TextureSourceFormat::RGBA32F;
    settings.compression = TextureCompressionSettings::HDR;
    Built built(allocator);
    REQUIRE(Build(allocator, settings, pixels, built));
    const Array<uint8>& bytes = built.result.mips[0].bytes;
    CHECK(Le16(bytes, 0) == 0x0001 && Le16(bytes, 2) == 0x0400 && Le16(bytes, 4) == 0x3c00 &&
          Le16(bytes, 6) == 0x0000);
}

} // namespace texture_build_tests_file
