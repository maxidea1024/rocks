#include <Base/InitializerList.h>
#include <GraphicsTypes/CookSampling.h>
#include <TestSupport/Test.h>
#include <TextureTools/TextureFormatSelect.h>

#include <stdio.h>

// 쿠킹 텍스처 형식 선택 시험이다(Docs/Design/AssetSystem 12.17.5 2·5 — UE 5.8
// GetDefaultTextureFormatName 순서). 기대값은 UE 원본 규칙으로 손으로 적은 표이고, 표 밖 전체
// 조합은 불변식으로 본다.
using namespace rocks;
namespace texture_format_select_tests_file {

namespace {

using graphics::Backend;
using graphics::GraphicsPlatform;
using graphics::PixelFormat;
using C = TextureCompressionSettings;
using S = TextureSourceFormat;
using F = PixelFormat;

constexpr C VisibleCompressions[] = {C::Default,
                                     C::Normalmap,
                                     C::Masks,
                                     C::Grayscale,
                                     C::Displacementmap,
                                     C::VectorDisplacementmap,
                                     C::HDR,
                                     C::EditorIcon,
                                     C::Alpha,
                                     C::DistanceFieldFont,
                                     C::HDR_Compressed,
                                     C::BC7,
                                     C::HalfFloat,
                                     C::SingleFloat,
                                     C::HDR_F32};
constexpr S Sources[] = {S::G8,     S::GA8,     S::G16,     S::GA16, S::RGBA8,
                         S::RGBA16, S::RGBA16F, S::RGBA32F, S::R16F, S::R32F};

TextureFormatRequest Request(C compression, S source, bool srgb, uint32 size = 256) noexcept {
    TextureFormatRequest request;
    request.compression = compression;
    request.sourceFormat = source;
    request.srgb = srgb;
    request.topSizeX = size;
    request.topSizeY = size;
    return request;
}

PixelFormat Choose(const TextureFormatRequest& request, const TextureFormatTarget& target,
                   bool alpha, TextureFormatChoice* choice = nullptr) noexcept {
    TextureFormatChoice local;
    TextureFormatChoice& out = choice != nullptr ? *choice : local;
    if (!SelectTextureFormat(request, target, alpha, out)) {
        return PixelFormat::Unknown;
    }

    return out.pixelFormat;
}

bool IsBlock(PixelFormat format) noexcept {
    return graphics::GetPixelFormatInfo(format).compression != graphics::PixelCompression::None;
}

bool HasWarning(const TextureFormatChoice& choice, AssetDiagnosticCode code,
                PixelFormat before) noexcept {
    for (usize index = 0; index < choice.warningCount; ++index) {
        if (choice.warnings[index].code == code &&
            choice.warnings[index].detail == graphics::PixelFormatNumber(before)) {
            return true;
        }
    }

    return false;
}

bool HasCode(const TextureFormatChoice& choice, AssetDiagnosticCode code) noexcept {
    for (usize index = 0; index < choice.warningCount; ++index) {
        if (choice.warnings[index].code == code) {
            return true;
        }
    }

    return false;
}

struct Row {
    C compression;
    S source;
    bool srgb;
    bool alpha;
    PixelFormat expected;
};

} // namespace

// UE 원본 규칙 표(Windows, 검증 끔, 256²): 설정별 이름 → sRGB 짝, AutoDXT 알파, G8 + sRGB → BGRA8
// sRGB, 막는 설정의 sRGB 끔, TD11 대체.
TEST(TextureFormatSelect_UeTable) {
    const TextureFormatTarget target;
    const Row rows[] = {
        {C::Default, S::RGBA8, true, false, F::BC1UnormSrgb},
        {C::Default, S::RGBA8, true, true, F::BC3UnormSrgb},
        {C::Default, S::RGBA8, false, false, F::BC1Unorm},
        {C::Default, S::RGBA8, false, true, F::BC3Unorm},
        {C::Default, S::G8, true, false, F::BC1UnormSrgb},
        {C::Default, S::RGBA16F, false, true, F::BC3Unorm},
        {C::Masks, S::RGBA8, true, true, F::BC3Unorm},
        {C::Masks, S::RGBA8, true, false, F::BC1Unorm},
        {C::Normalmap, S::RGBA8, true, false, F::BC5Unorm},
        {C::Normalmap, S::RGBA16, false, true, F::BC5Unorm},
        {C::Grayscale, S::G8, false, false, F::R8Unorm},
        {C::Grayscale, S::G8, true, false, F::BGRA8UnormSrgb},
        {C::Grayscale, S::RGBA8, true, false, F::BGRA8UnormSrgb},
        {C::Grayscale, S::RGBA16, false, false, F::R8Unorm},
        {C::Grayscale, S::G16, true, false, F::R16Unorm},
        {C::Displacementmap, S::G16, false, false, F::R16Unorm},
        {C::Displacementmap, S::RGBA8, true, false, F::BGRA8UnormSrgb},
        {C::Displacementmap, S::RGBA8, false, false, F::R8Unorm},
        {C::VectorDisplacementmap, S::RGBA8, true, false, F::BGRA8UnormSrgb},
        {C::VectorDisplacementmap, S::RGBA16F, false, false, F::BGRA8Unorm},
        {C::HDR, S::RGBA16F, true, false, F::RGBA16Float},
        {C::HDR, S::RGBA8, false, false, F::RGBA16Float},
        {C::EditorIcon, S::RGBA8, true, true, F::BGRA8UnormSrgb},
        {C::EditorIcon, S::RGBA16F, false, false, F::RGBA16Float},
        {C::EditorIcon, S::G8, false, false, F::BGRA8Unorm},
        {C::Alpha, S::G8, true, false, F::BC4Unorm},
        {C::DistanceFieldFont, S::G8, false, false, F::R8Unorm},
        {C::DistanceFieldFont, S::RGBA8, true, false, F::BGRA8UnormSrgb},
        {C::HDR_Compressed, S::RGBA16F, false, false, F::BC6HUfloat},
        {C::BC7, S::RGBA8, true, true, F::BC7UnormSrgb},
        {C::BC7, S::RGBA8, false, false, F::BC7Unorm},
        {C::HalfFloat, S::R16F, false, false, F::R16Float},
        {C::SingleFloat, S::R32F, false, false, F::R32Float},
        {C::HDR_F32, S::RGBA32F, false, false, F::RGBA32Float},
    };
    usize index = 0;
    for (const Row& row : rows) {
        const PixelFormat got =
            Choose(Request(row.compression, row.source, row.srgb), target, row.alpha);
        if (got != row.expected) {
            printf("  row %zu: %s, expected %s\n", index, graphics::PixelFormatName(got),
                   graphics::PixelFormatName(row.expected));
        }

        CHECK(got == row.expected);
        ++index;
    }

    // compressionNoAlpha·ForceAlpha(ForceNoAlpha가 먼저).
    TextureFormatRequest request = Request(C::Default, S::RGBA8, false);
    request.compressionNoAlpha = true;
    CHECK(Choose(request, target, true) == F::BC1Unorm);
    request.compressionForceAlpha = true;
    CHECK(Choose(request, target, false) == F::BC1Unorm);
    request.compressionNoAlpha = false;
    CHECK(Choose(request, target, false) == F::BC3Unorm);
    // TC3(12.17.6 6·9): BC7·BC6H는 대체 없이 그 형식이고 경고가 없다. BC7 프로필은 유효 알파다
    // (ForceNoAlpha가 ForceAlpha보다 먼저).
    TextureFormatChoice choice;
    CHECK(Choose(Request(C::BC7, S::RGBA8, true), target, false, &choice) == F::BC7UnormSrgb);
    CHECK(choice.warningCount == 0 && choice.encoding == TextureFormatEncoding::Bc7 &&
          !choice.alphaProfile);
    CHECK(Choose(Request(C::BC7, S::RGBA8, true), target, true, &choice) == F::BC7UnormSrgb &&
          choice.alphaProfile);
    TextureFormatRequest bc7 = Request(C::BC7, S::RGBA8, false);
    bc7.compressionForceAlpha = true;
    CHECK(Choose(bc7, target, false, &choice) == F::BC7Unorm && choice.alphaProfile);
    bc7.compressionNoAlpha = true;
    CHECK(Choose(bc7, target, true, &choice) == F::BC7Unorm && !choice.alphaProfile);
    CHECK(Choose(Request(C::HDR_Compressed, S::RGBA16F, false), target, true, &choice) ==
          F::BC6HUfloat);
    CHECK(choice.warningCount == 0 && choice.encoding == TextureFormatEncoding::Bc6h &&
          !choice.alphaProfile);
    // 알파 판정은 AutoDXT 형식과 BC7 프로필에만 쓴다.
    CHECK(TextureFormatNeedsAlpha(Request(C::BC7, S::RGBA8, true), target));
    CHECK(!TextureFormatNeedsAlpha(Request(C::HDR_Compressed, S::RGBA16F, false), target));
    // 비압축 강제의 HDR_Compressed는 경고가 없다(인코더를 고르지 않았다).
    CHECK(Choose(Request(C::HDR_Compressed, S::RGBA16F, false, 6), target, false, &choice) ==
          F::RGBA16Float);
    CHECK(choice.warningCount == 0);
}

// 비압축 강제(Texture.cpp 4237–4263): compressionNone·EditorIcon·LOD 그룹 셋·상위 mip 한 변이 4
// 미만이거나 4의 배수가 아님(Z는 보지 않는다). 대체: 회색 설정 + 회색 원본은 G8·G16, float
// 원본·HDR_Compressed는 RGBA16F, 그 밖(Normalmap·Masks·RGBA16 원본 포함)은 BGRA8.
TEST(TextureFormatSelect_UncompressedConditions) {
    const TextureFormatTarget target;
    TextureFormatRequest request = Request(C::Default, S::RGBA8, false);
    CHECK(Choose(request, target, false) == F::BC1Unorm);
    request.compressionNone = true;
    CHECK(Choose(request, target, true) == F::BGRA8Unorm);
    request.compressionNone = false;
    for (const TextureGroup group :
         {TextureGroup::ColorLookupTable, TextureGroup::Bokeh, TextureGroup::IESLightProfile}) {
        request.lodGroup = group;
        CHECK(Choose(request, target, false) == F::BGRA8Unorm);
    }

    request.lodGroup = TextureGroup::World;
    const uint32 sizes[][2] = {{2, 256}, {256, 2}, {6, 8}, {8, 6}, {3, 3}, {1, 1}, {5, 4}};
    for (const auto& size : sizes) {
        request.topSizeX = size[0];
        request.topSizeY = size[1];
        CHECK(Choose(request, target, false) == F::BGRA8Unorm);
    }

    request.topSizeX = 4;
    request.topSizeY = 12;
    CHECK(Choose(request, target, false) == F::BC1Unorm);
    // Volume도 X·Y만 본다(데스크톱은 압축 볼륨을 지원한다).
    request.dimension = TextureDimension::Volume;
    CHECK(Choose(request, target, false) == F::BC1Unorm);
    // 대체 이름.
    CHECK(Choose(Request(C::Normalmap, S::RGBA8, false, 6), target, false) == F::BGRA8Unorm);
    CHECK(Choose(Request(C::Masks, S::RGBA16, false, 6), target, true) == F::BGRA8Unorm);
    CHECK(Choose(Request(C::Default, S::RGBA16, false, 6), target, false) == F::BGRA8Unorm);
    CHECK(Choose(Request(C::Default, S::RGBA16F, false, 6), target, false) == F::RGBA16Float);
    CHECK(Choose(Request(C::Default, S::RGBA8, true, 6), target, false) == F::BGRA8UnormSrgb);
    CHECK(Choose(Request(C::Alpha, S::G8, false, 6), target, false) == F::BGRA8Unorm);
    CHECK(Choose(Request(C::Default, S::G8, false, 6), target, false) == F::BGRA8Unorm);
    // ColorLookupTable은 sRGB도 끈다.
    TextureFormatRequest lut = Request(C::Default, S::RGBA8, true);
    lut.lodGroup = TextureGroup::ColorLookupTable;
    TextureFormatChoice choice;
    CHECK(Choose(lut, target, false, &choice) == F::BGRA8Unorm && choice.srgbIgnored);
}

// 32F 필터·norm16 대체와 Android(TC4 전 비압축 — 경고 없음).
TEST(TextureFormatSelect_FloatNorm16AndAndroid) {
    TextureFormatTarget windows;
    // 검증을 끈 대상은 32F 선형 필터를 된다고 본다.
    CHECK(Choose(Request(C::HDR_F32, S::RGBA32F, false), windows, false) == F::RGBA32Float);
    windows.norm16Supported = false;
    CHECK(Choose(Request(C::Grayscale, S::G16, false), windows, false) == F::R16Float);
    TextureFormatTarget android;
    android.platform = GraphicsPlatform::Android;
    TextureFormatChoice choice;
    CHECK(Choose(Request(C::Default, S::RGBA8, false), android, true, &choice) == F::BGRA8Unorm);
    CHECK(choice.warningCount == 0);
    CHECK(Choose(Request(C::Normalmap, S::RGBA8, false), android, false) == F::BGRA8Unorm);
    CHECK(Choose(Request(C::BC7, S::RGBA8, false), android, false, &choice) == F::BGRA8Unorm &&
          choice.warningCount == 0);
    // Android OpenGL: 32F 선형 필터 보장이 없으면(쿠킹 보장 표) 필터가 Point가 아닐 때만 16F다.
    android.backends = TextureFormatBackendBit(Backend::OpenGL);
    const Backend gl[] = {Backend::OpenGL};
    const bool filterable32 = graphics::HasPixelFormatCaps(
        graphics::CookSamplingGuarantee(Span<const Backend>(gl, 1), GraphicsPlatform::Android,
                                        F::RGBA32Float),
        graphics::CookSamplingCaps);
    TextureFormatRequest hdr = Request(C::HDR_F32, S::RGBA32F, false);
    CHECK(Choose(hdr, android, false) == (filterable32 ? F::RGBA32Float : F::RGBA16Float));
    hdr.pointFilter = true;
    const bool sampled32 = graphics::HasPixelFormatCaps(
        graphics::CookSamplingGuarantee(Span<const Backend>(gl, 1), GraphicsPlatform::Android,
                                        F::RGBA32Float),
        graphics::PixelFormatCaps::Sampled);
    CHECK(Choose(hdr, android, false) == (sampled32 ? F::RGBA32Float : F::RGBA16Float));
}

// 샘플링 검증(TD12): 대상 backend 모두의 보장 표로 판정하고, 모자라면 CookSamplingFallback과
// PlatformFormatFallback 경고(detail = 바꾸기 전 형식)다. 쿠킹 대상이 아닌 조합은
// InvalidArgument다.
TEST(TextureFormatSelect_SamplingFallback) {
    TextureFormatTarget android;
    android.platform = GraphicsPlatform::Android;
    android.backends = TextureFormatBackendBit(Backend::OpenGL);
    const Backend gl[] = {Backend::OpenGL};
    const Span<const Backend> glList(gl, 1);
    TextureFormatChoice choice;
    const PixelFormat got = Choose(Request(C::Default, S::RGBA8, true), android, false, &choice);
    const PixelFormat expected =
        graphics::HasPixelFormatCaps(
            graphics::CookSamplingGuarantee(glList, GraphicsPlatform::Android, F::BGRA8UnormSrgb),
            graphics::CookSamplingCaps)
            ? F::BGRA8UnormSrgb
            : graphics::CookSamplingFallback(glList, GraphicsPlatform::Android, F::BGRA8UnormSrgb);
    CHECK(got == expected);
    CHECK((got != F::BGRA8UnormSrgb) ==
          HasWarning(choice, AssetDiagnosticCode::PlatformFormatFallback, F::BGRA8UnormSrgb));
    // 렌더 표 예(CookSampling.h): ES에는 BGRA8 보장이 없다.
    CHECK(got == F::RGBA8UnormSrgb && choice.srgb);
    // Windows 세 backend: BC·BGRA8·16F는 보장이다(엔진 요구).
    TextureFormatTarget windows;
    windows.backends = TextureFormatBackendBit(Backend::D3D12) |
                       TextureFormatBackendBit(Backend::D3D11) |
                       TextureFormatBackendBit(Backend::Vulkan);
    for (const C compression : VisibleCompressions) {
        for (const bool alpha : {false, true}) {
            CHECK(Choose(Request(compression, S::RGBA8, true), windows, alpha, &choice) !=
                  F::Unknown);
            CHECK(!HasCode(choice, AssetDiagnosticCode::PlatformFormatFallback));
        }
    }

    // 쿠킹 대상이 아닌 조합(Metal·Windows)은 대체가 없다.
    TextureFormatTarget wrong;
    wrong.backends = TextureFormatBackendBit(Backend::Metal);
    CHECK(!SelectTextureFormat(Request(C::Default, S::RGBA8, false), wrong, false, choice));
}

// 불변식(전체 조합 — 설정 15 × 원본 10 × sRGB 2 × 알파 2 × 크기 2 × 플랫폼 2): 결과가 있고, 블록
// 형식은 4의 배수 크기·데스크톱에서만, sRGB 출력은 막지 않는 설정에서만, 인코딩은 형식과 맞고,
// swizzle·채널 복제는 CompressionSettings로만 정해지며, 알파 의존은 Default·Masks의 압축
// 경로뿐이다.
TEST(TextureFormatSelect_Invariants) {
    usize checked = 0;
    for (const C compression : VisibleCompressions) {
        for (const S source : Sources) {
            for (const bool srgb : {false, true}) {
                for (const uint32 size : {256u, 6u}) {
                    for (const GraphicsPlatform platform :
                         {GraphicsPlatform::Windows, GraphicsPlatform::Android}) {
                        TextureFormatTarget target;
                        target.platform = platform;
                        const TextureFormatRequest request =
                            Request(compression, source, srgb, size);
                        // AutoDXT는 형식이, BC7은 인코더 프로필이 알파에 달렸다(12.17.6 4).
                        const bool depends = TextureFormatNeedsAlpha(request, target);
                        const bool alphaDependent =
                            (compression == C::Default || compression == C::Masks ||
                             compression == C::BC7) &&
                            size == 256 && platform == GraphicsPlatform::Windows;
                        CHECK(depends == alphaDependent);
                        for (const bool alpha : {false, true}) {
                            TextureFormatChoice choice;
                            REQUIRE(SelectTextureFormat(request, target, alpha, choice));
                            const PixelFormat format = choice.pixelFormat;
                            CHECK(format != F::Unknown);
                            if (IsBlock(format)) {
                                CHECK(size == 256 && platform == GraphicsPlatform::Windows);
                            }

                            const bool prevented =
                                compression == C::Alpha || compression == C::Normalmap ||
                                compression == C::Masks || compression == C::HDR ||
                                compression == C::HDR_F32 || compression == C::HDR_Compressed ||
                                compression == C::HalfFloat || compression == C::SingleFloat;
                            if (choice.srgb) {
                                CHECK(srgb && !prevented);
                            }

                            CHECK(choice.srgb == graphics::GetPixelFormatInfo(format).srgb);
                            if (prevented && srgb) {
                                CHECK(choice.srgbIgnored);
                            }

                            const bool bc = choice.encoding != TextureFormatEncoding::Uncompressed;
                            CHECK(bc == IsBlock(format));
                            const bool red = compression == C::Grayscale || compression == C::Alpha;
                            const bool alphaCopy = compression == C::Displacementmap ||
                                                   compression == C::DistanceFieldFont;
                            CHECK(choice.replicate == (red ? TextureChannelReplicate::Red
                                                       : alphaCopy
                                                           ? TextureChannelReplicate::Alpha
                                                           : TextureChannelReplicate::None));
                            const bool rrrr = red || compression == C::DistanceFieldFont;
                            CHECK(choice.swizzle[0] == TextureSwizzle::R);
                            CHECK(choice.swizzle[1] == (rrrr ? TextureSwizzle::R
                                                        : compression == C::Displacementmap
                                                            ? TextureSwizzle::Zero
                                                            : TextureSwizzle::G));
                            CHECK(choice.swizzle[3] == (rrrr ? TextureSwizzle::R
                                                        : compression == C::Displacementmap ||
                                                                compression == C::Normalmap
                                                            ? TextureSwizzle::One
                                                            : TextureSwizzle::A));
                            ++checked;
                        }
                    }
                }
            }
        }
    }

    CHECK(checked == 15u * 10u * 2u * 2u * 2u * 2u);
}

// 인자 오류: 원본 Invalid·범위 밖, 크기 0, 숨김 설정(LQ·EncodedReflectionCapture), 범위 밖
// 플랫폼·backend 비트.
TEST(TextureFormatSelect_ArgumentErrors) {
    const TextureFormatTarget target;
    TextureFormatChoice choice;
    CHECK(SelectTextureFormat(Request(C::Default, S::Invalid, false), target, false, choice)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(SelectTextureFormat(Request(C::Default, static_cast<S>(11), false), target, false, choice)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(SelectTextureFormat(Request(C::Default, S::RGBA8, false, 0), target, false, choice)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(SelectTextureFormat(Request(C::LQ, S::RGBA8, false), target, false, choice).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(SelectTextureFormat(Request(C::EncodedReflectionCapture, S::RGBA8, false), target, false,
                              choice)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(SelectTextureFormat(Request(static_cast<C>(17), S::RGBA8, false), target, false, choice)
              .Error() == ErrorCode::InvalidArgument);
    TextureFormatTarget wrong;
    wrong.platform = static_cast<GraphicsPlatform>(3);
    CHECK(SelectTextureFormat(Request(C::Default, S::RGBA8, false), wrong, false, choice).Error() ==
          ErrorCode::InvalidArgument);
    wrong = TextureFormatTarget{};
    wrong.backends = uint32{1} << graphics::BackendCount;
    CHECK(SelectTextureFormat(Request(C::Default, S::RGBA8, false), wrong, false, choice).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(choice.pixelFormat == F::Unknown);
    CHECK(!TextureFormatNeedsAlpha(Request(C::Default, S::Invalid, false), target));
}

} // namespace texture_format_select_tests_file
