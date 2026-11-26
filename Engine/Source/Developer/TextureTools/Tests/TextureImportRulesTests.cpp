#include "TextureImportTestSupport.h"

#include <Base/InitializerList.h>

#include <Platform/File.h>
#include <Platform/Process.h>

#include <math.h>
#include <stdio.h>

// 텍스처 임포트 규칙 시험이다(Docs/Design/AssetSystem 12.12절 — 14.1.97절 대조표): PNG infill(세
// 값·네 형식·행 채우기·위쪽 빈 행), 선형 → sRGB 8비트 변환, 설정 정규화(UE Validate·LODGroup),
// 임포트 요청 규칙, asset-import 예약 인자 번역(명령 문법 표·이스케이프·사용법 위반·DDS 차원 선택),
// hook 등록.
using namespace rocks;
namespace texture_import_rules_tests_file {

using namespace rocks::texture_import_test;
using reflection_text::TextAssignment;

namespace {

image::DecodedImage Image(Allocator& allocator, TextureSourceFormat format, uint32 width,
                          uint32 height, Span<const uint8> bytes) noexcept {
    image::DecodedImage out(allocator);
    out.format = format;
    out.width = width;
    out.height = height;
    CHECK(out.pixels.TryAppend(bytes));
    return out;
}

constexpr uint8 W = 255;

bool HasOption(const AssetImportTranslation& out, StringView path, StringView value) noexcept {
    for (const TextAssignment& option : out.options) {
        if (option.path == path && option.value == value) {
            return true;
        }
    }

    return false;
}

Result<bool> Translate(Allocator& allocator, Span<const AssetImportReservedArgument> reserved,
                       Span<const AssetImportSourceInput> sources, StringView importer,
                       AssetImportTranslation& out) noexcept {
    return TranslateTextureImportArguments(nullptr, allocator, reserved, sources, importer, out);
}

} // namespace

// stb 표 변환(UE ToFColorSRGB): 8비트 sRGB 값을 공식(UE sRGBToLinearTable 주석)으로 선형화해 다시
// 바꾸면 같은 값이다. 범위 밖은 0·255, NaN은 0이다.
TEST(TextureImport_LinearToSrgb) {
    for (uint32 value = 0; value < 256; ++value) {
        const double c = static_cast<double>(value) / 255.0;
        const double linear =
            c > 0.04045 ? pow(c * (1.0 / 1.055) + 0.0521327, 2.4) : c * (1.0 / 12.92);
        CHECK(texture_import::LinearToSrgb8(static_cast<float>(linear)) == value);
    }

    CHECK(texture_import::LinearToSrgb8(2.0f) == 255 && texture_import::LinearToSrgb8(-1.0f) == 0);
    const float nan = platform::BitCast<float>(0x7fc00000u);
    CHECK(texture_import::LinearToSrgb8(nan) == 0);
}

// 이진 투명(OnlyOnBinaryTransparency 기본): 흰색 투명 texel이 이웃 불투명 색을 받고 알파는 0으로
// 남는다. 이웃 하나뿐인 칸은 그 색 그대로(선형 왕복), 빈 칸 사이는 행 채우기(왼쪽 절반 왼쪽 색).
TEST(TextureImport_InfillBinary) {
    SystemAllocator allocator;
    // 1×12 행: [빨강 불투명][흰 투명 × 10][파랑 불투명] — 반지름 4 밖은 행 채우기가 칠한다.
    Array<uint8> bytes(allocator);
    const uint8 red[] = {200, 0, 0, 255};
    const uint8 white[] = {W, W, W, 0};
    const uint8 blue[] = {0, 0, 200, 255};
    REQUIRE(bytes.TryAppend(Span<const uint8>(red)));
    for (uint32 index = 0; index < 10; ++index) {
        REQUIRE(bytes.TryAppend(Span<const uint8>(white)));
    }

    REQUIRE(bytes.TryAppend(Span<const uint8>(blue)));
    image::DecodedImage image = Image(allocator, TextureSourceFormat::RGBA8, 12, 1, bytes.View());
    REQUIRE(texture_import::FillTransparentTexels(
        image, TextureImportPngInfill::OnlyOnBinaryTransparency, allocator));
    const uint8* p = image.pixels.Data();
    // 1번 칸은 빨강만 이웃이다 → 빨강 그대로, 알파 0.
    CHECK(p[4] == 200 && p[5] == 0 && p[6] == 0 && p[7] == 0);
    // 10번 칸은 파랑만 이웃이다.
    CHECK(p[40] == 0 && p[42] == 200 && p[43] == 0);
    // 5·6번 칸은 반지름 밖 — 행 채우기: [5, 7) 구간의 왼쪽 절반(5)은 왼쪽 색, 오른쪽(6)은 오른쪽
    // 색.
    CHECK(p[5 * 4] == 200 && p[5 * 4 + 2] == 0);
    CHECK(p[6 * 4] == 0 && p[6 * 4 + 2] == 200);
    // 불투명 칸은 그대로다.
    CHECK(p[0] == 200 && p[3] == 255 && p[47] == 255);

    // Never는 그대로다.
    image::DecodedImage never = Image(allocator, TextureSourceFormat::RGBA8, 12, 1, bytes.View());
    REQUIRE(texture_import::FillTransparentTexels(never, TextureImportPngInfill::Never, allocator));
    CHECK(never.pixels[4] == W && never.pixels[7] == 0);
}

// 복합 알파: 알파 128 texel이 있으면 OnlyOnBinaryTransparency는 그대로, Always는 채운다(알파 가중 —
// 반투명 texel도 이웃이다). 알파 0인데 흰색이 아닌 texel도 복합이다.
TEST(TextureImport_InfillComplexAlpha) {
    SystemAllocator allocator;
    const uint8 bytes[] = {100, 100, 100, 128, W, W, W, 0, 0, 0, 0, 0};
    image::DecodedImage binary = Image(allocator, TextureSourceFormat::RGBA8, 3, 1, bytes);
    REQUIRE(texture_import::FillTransparentTexels(
        binary, TextureImportPngInfill::OnlyOnBinaryTransparency, allocator));
    CHECK(binary.pixels[4] == W);
    image::DecodedImage always = Image(allocator, TextureSourceFormat::RGBA8, 3, 1, bytes);
    REQUIRE(
        texture_import::FillTransparentTexels(always, TextureImportPngInfill::Always, allocator));
    // 이웃은 0번(회색, 알파 128)뿐이다 → 회색 그대로. 검정 투명 texel(2번)은 건드리지 않는다.
    CHECK(always.pixels[4] == 100 && always.pixels[6] == 100 && always.pixels[7] == 0);
    CHECK(always.pixels[8] == 0 && always.pixels[11] == 0);

    // 흰색 투명이 없으면 Always도 그대로다.
    const uint8 opaque[] = {1, 2, 3, 255, 4, 5, 6, 128};
    image::DecodedImage none = Image(allocator, TextureSourceFormat::RGBA8, 2, 1, opaque);
    REQUIRE(texture_import::FillTransparentTexels(none, TextureImportPngInfill::Always, allocator));
    CHECK(none.pixels[4] == 4 && none.pixels[7] == 128);
}

// 행 채우기: 행 전체가 흰색 투명이면 위 행 색을 받고, 맨 위 빈 행들은 처음 채워진 행에서 받는다.
// 모두 흰색 투명이면 그대로다.
TEST(TextureImport_InfillRows) {
    SystemAllocator allocator;
    // 1×12 세로: 0–5행 투명, 6행 초록, 7–11행 투명(6행에서 반지름 4 안은 이웃 채우기, 밖은 행
    // 채우기).
    Array<uint8> bytes(allocator);
    const uint8 white[] = {W, W, W, 0};
    const uint8 green[] = {0, 150, 0, 255};
    for (uint32 row = 0; row < 12; ++row) {
        REQUIRE(bytes.TryAppend(row == 6 ? Span<const uint8>(green) : Span<const uint8>(white)));
    }

    image::DecodedImage image = Image(allocator, TextureSourceFormat::RGBA8, 1, 12, bytes.View());
    REQUIRE(texture_import::FillTransparentTexels(
        image, TextureImportPngInfill::OnlyOnBinaryTransparency, allocator));
    for (uint32 row = 0; row < 12; ++row) {
        const uint8* texel = image.pixels.Data() + row * 4;
        CHECK(texel[0] == 0 && texel[1] == 150 && texel[2] == 0);
        CHECK(texel[3] == (row == 6 ? 255 : 0));
    }

    const uint8 empty[] = {W, W, W, 0, W, W, W, 0};
    image::DecodedImage blank = Image(allocator, TextureSourceFormat::RGBA8, 1, 2, empty);
    REQUIRE(
        texture_import::FillTransparentTexels(blank, TextureImportPngInfill::Always, allocator));
    CHECK(blank.pixels[0] == W && blank.pixels[4] == W);
}

// 다른 형식: GA8(회색+알파 — UE에서는 BGRA8로 채워진다), RGBA16·GA16(16비트 최대 65535 판정, 선형
// v/65535 왕복). 그 밖 형식(G8)은 그대로다.
TEST(TextureImport_InfillFormats) {
    SystemAllocator allocator;
    const uint8 ga8[] = {77, 255, W, 0};
    image::DecodedImage gray = Image(allocator, TextureSourceFormat::GA8, 2, 1, ga8);
    REQUIRE(texture_import::FillTransparentTexels(
        gray, TextureImportPngInfill::OnlyOnBinaryTransparency, allocator));
    CHECK(gray.pixels[2] == 77 && gray.pixels[3] == 0);

    // RGBA16 little-endian: [(1000, 2000, 3000, 65535)][흰 투명].
    const uint8 rgba16[] = {0xe8, 0x03, 0xd0, 0x07, 0xb8, 0x0b, 0xff, 0xff,
                            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00};
    image::DecodedImage wide = Image(allocator, TextureSourceFormat::RGBA16, 2, 1, rgba16);
    REQUIRE(texture_import::FillTransparentTexels(
        wide, TextureImportPngInfill::OnlyOnBinaryTransparency, allocator));
    const uint16* texels = reinterpret_cast<const uint16*>(wide.pixels.Data());
    CHECK(texels[4] == 1000 && texels[5] == 2000 && texels[6] == 3000 && texels[7] == 0);

    const uint8 ga16[] = {0x10, 0x27, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00};
    image::DecodedImage gray16 = Image(allocator, TextureSourceFormat::GA16, 2, 1, ga16);
    REQUIRE(
        texture_import::FillTransparentTexels(gray16, TextureImportPngInfill::Always, allocator));
    const uint16* g16 = reinterpret_cast<const uint16*>(gray16.pixels.Data());
    CHECK(g16[2] == 10000 && g16[3] == 0);

    const uint8 g8[] = {W, 0};
    image::DecodedImage plain = Image(allocator, TextureSourceFormat::G8, 2, 1, g8);
    REQUIRE(
        texture_import::FillTransparentTexels(plain, TextureImportPngInfill::Always, allocator));
    CHECK(plain.pixels[0] == W && plain.pixels[1] == 0);
}

// 설정 정규화(UE ValidateSettingsAfterImportOrEdit·PostEditChangeProperty LODGroup).
TEST(TextureImport_NormalizeSettings) {
    TextureEnv env;
    REQUIRE(env.ready);
    auto created =
        env.objects.NewObject<Texture>(env.objects.OwnerAccess(), TypeIdOf<TextureCube>());
    REQUIRE(created);
    auto borrowed =
        env.objects.ResolveForWrite<Texture>(env.objects.OwnerAccess(), created.Value().Handle());
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    Texture& texture = *value.Value();
    const auto reset = [&texture]() noexcept {
        texture.compressionSettings = TextureCompressionSettings::Default;
        texture.srgb = 1;
        texture.useLegacyGamma = 1;
        texture.lodGroup = TextureGroup::World;
        texture.mipGenSettings = TextureMipGenSettings::FromTextureGroup;
        texture.powerOfTwoMode = TexturePowerOfTwoSetting::None;
        texture.maxTextureSize = 0;
        texture.numCinematicMipLevels = 0;
        texture.resizeDuringBuildX = 0;
        texture.resizeDuringBuildY = 0;
        texture.filter = TextureFilter::Nearest;
        texture.source.format = TextureSourceFormat::RGBA8;
    };

    // 선형 종류는 SRGB off(대입과 무관), Grayscale·Default는 그대로, SRGB off면 legacy gamma off.
    const TextureCompressionSettings offKinds[] = {
        TextureCompressionSettings::Normalmap, TextureCompressionSettings::Masks,
        TextureCompressionSettings::Alpha,     TextureCompressionSettings::HDR,
        TextureCompressionSettings::HDR_F32,   TextureCompressionSettings::HDR_Compressed,
        TextureCompressionSettings::HalfFloat, TextureCompressionSettings::SingleFloat};
    for (TextureCompressionSettings kind : offKinds) {
        reset();
        texture.compressionSettings = kind;
        NormalizeTextureSettings(texture, TextureDimension::TextureCube, {});
        CHECK(texture.srgb == 0 && texture.useLegacyGamma == 0);
    }

    for (TextureCompressionSettings kind :
         {TextureCompressionSettings::Grayscale, TextureCompressionSettings::Default,
          TextureCompressionSettings::VectorDisplacementmap}) {
        reset();
        texture.compressionSettings = kind;
        NormalizeTextureSettings(texture, TextureDimension::TextureCube, {});
        CHECK(texture.srgb == 1 && texture.useLegacyGamma == 1);
    }

    // LUT 그룹: NoMipmaps·SRGB off(원본이 있을 때만).
    reset();
    texture.lodGroup = TextureGroup::ColorLookupTable;
    NormalizeTextureSettings(texture, TextureDimension::TextureCube, {});
    CHECK(texture.mipGenSettings == TextureMipGenSettings::NoMipmaps && texture.srgb == 0);
    reset();
    texture.lodGroup = TextureGroup::ColorLookupTable;
    texture.source.format = TextureSourceFormat::Invalid;
    NormalizeTextureSettings(texture, TextureDimension::TextureCube, {});
    CHECK(texture.mipGenSettings == TextureMipGenSettings::FromTextureGroup && texture.srgb == 1);

    // 최대 크기: 0 이하 → 0, 300 → 512, 20000 → 16384. cinematic mip은 0 이상.
    const int32 sizes[][2] = {{-5, 0}, {0, 0}, {1, 1}, {300, 512}, {512, 512}, {20000, 16384}};
    for (const auto& size : sizes) {
        reset();
        texture.maxTextureSize = size[0];
        texture.numCinematicMipLevels = -3;
        NormalizeTextureSettings(texture, TextureDimension::TextureCube, {});
        CHECK(texture.maxTextureSize == size[1] && texture.numCinematicMipLevels == 0);
    }

    // Volume 상한은 2048이다(UE UVolumeTexture::GetMaximumDimension).
    reset();
    texture.maxTextureSize = 3000;
    texture.resizeDuringBuildX = 4096;
    NormalizeTextureSettings(texture, TextureDimension::Volume, {});
    CHECK(texture.maxTextureSize == 2048 && texture.resizeDuringBuildX == 2048);

    // 크기 조정: LeaveExistingMips와 함께면 None, Resize는 Texture2D만, resize 값은 [0, 16384].
    reset();
    texture.mipGenSettings = TextureMipGenSettings::LeaveExistingMips;
    texture.powerOfTwoMode = TexturePowerOfTwoSetting::PadToPowerOfTwo;
    NormalizeTextureSettings(texture, TextureDimension::Texture2D, {});
    CHECK(texture.powerOfTwoMode == TexturePowerOfTwoSetting::None);
    reset();
    texture.powerOfTwoMode = TexturePowerOfTwoSetting::ResizeToSpecificResolution;
    NormalizeTextureSettings(texture, TextureDimension::Texture2D, {});
    CHECK(texture.powerOfTwoMode == TexturePowerOfTwoSetting::ResizeToSpecificResolution);
    NormalizeTextureSettings(texture, TextureDimension::TextureCube, {});
    CHECK(texture.powerOfTwoMode == TexturePowerOfTwoSetting::None);
    reset();
    texture.resizeDuringBuildX = -2;
    texture.resizeDuringBuildY = 99999;
    NormalizeTextureSettings(texture, TextureDimension::Texture2D, {});
    CHECK(texture.resizeDuringBuildX == 0 && texture.resizeDuringBuildY == 16384);

    // LODGroup 8BitData·16BitData: 대입했을 때만(대소문자 무시) 압축·SRGB·필터·mip을 바꾼다.
    const StringView assigned[] = {"LODGROUP"};
    reset();
    texture.lodGroup = TextureGroup::EightBitData;
    NormalizeTextureSettings(texture, TextureDimension::TextureCube, {});
    CHECK(texture.compressionSettings == TextureCompressionSettings::Default && texture.srgb == 1);
    NormalizeTextureSettings(texture, TextureDimension::TextureCube, assigned);
    CHECK(texture.compressionSettings == TextureCompressionSettings::VectorDisplacementmap &&
          texture.srgb == 0 && texture.filter == TextureFilter::Default &&
          texture.mipGenSettings == TextureMipGenSettings::FromTextureGroup);
    reset();
    texture.lodGroup = TextureGroup::SixteenBitData;
    texture.mipGenSettings = TextureMipGenSettings::NoMipmaps;
    NormalizeTextureSettings(texture, TextureDimension::TextureCube, assigned);
    CHECK(texture.compressionSettings == TextureCompressionSettings::HDR && texture.srgb == 0 &&
          texture.mipGenSettings == TextureMipGenSettings::FromTextureGroup);

    // 두 번 불러도 같다.
    reset();
    texture.compressionSettings = TextureCompressionSettings::Normalmap;
    texture.maxTextureSize = 300;
    NormalizeTextureSettings(texture, TextureDimension::TextureCube, assigned);
    NormalizeTextureSettings(texture, TextureDimension::TextureCube, assigned);
    CHECK(texture.srgb == 0 && texture.maxTextureSize == 512);
}

// hook 등록: Texture 타입에 정규화 hook 하나(+ 원본 묶음 재구성 셋), 표를 거쳐 파생
// 클래스(Texture2D)에서도 불린다.
TEST(TextureImport_NormalizeHook) {
    TextureEnv env;
    REQUIRE(env.ready);
    AssetTypeHookTable table(env.allocator, env.types);
    Array<AssetTypeHookRegistration> tokens(env.allocator);
    REQUIRE(RegisterTextureTypeHooks(nullptr, table, tokens));
    // 정규화 hook(Texture) 하나 + 원본 묶음 재구성 hook 셋(Texture2DArray·TextureCubeArray·
    // VolumeTexture — 12.14절 A5j2).
    CHECK(table.Count() == 4 && tokens.Size() == 4);
    auto created = env.objects.NewObject<Texture>(env.objects.OwnerAccess(), TypeIdOf<Texture2D>());
    REQUIRE(created);
    const ObjectHandle handle = created.Value().Handle();
    {
        auto borrowed = env.objects.ResolveForWrite<Texture>(env.objects.OwnerAccess(), handle);
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        value.Value()->compressionSettings = TextureCompressionSettings::Masks;
        value.Value()->source.format = TextureSourceFormat::RGBA8;
        value.Value()->powerOfTwoMode = TexturePowerOfTwoSetting::ResizeToSpecificResolution;
    }

    REQUIRE(table.NormalizeSettings(env.objects, env.objects.OwnerAccess(), handle,
                                    TypeIdOf<Texture2D>(), AssetId{}, {}));
    auto borrowed = env.objects.Resolve<Texture>(env.objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    // Texture2D라 Resize는 남는다.
    CHECK(value.Value()->srgb == 0 &&
          value.Value()->powerOfTwoMode == TexturePowerOfTwoSetting::ResizeToSpecificResolution);
    tokens.Clear();
}

// 임포트 요청: 종류별 임포터 이름·역할(slice:<i>·cube:<i>.<면>)·옵션(기본값과 다른 것만), 규칙
// 위반.
TEST(TextureImport_Request) {
    SystemAllocator allocator;
    const StringView six[] = {"a", "b", "c", "d", "e", "f"};
    const StringView twelve[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"};
    const StringView one[] = {"x.png"};
    {
        AssetImportTranslation out(allocator);
        TextureImportSpec spec;
        spec.kind = TextureImportKind::CubeArray;
        spec.files = twelve;
        REQUIRE(MakeTextureImportRequest(spec, out).Value());
        CHECK(out.importer == "texture_cube_array" && out.sources.Size() == 12 &&
              out.sources[0].role == "cube:0.px" && out.sources[7].role == "cube:1.nx" &&
              out.sources[11].role == "cube:1.nz" && out.sources[11].path == "l" &&
              out.options.IsEmpty());
    }

    {
        AssetImportTranslation out(allocator);
        TextureImportSpec spec;
        spec.kind = TextureImportKind::Volume;
        spec.files = twelve;
        spec.pngInfill = TextureImportPngInfill::Always;
        REQUIRE(MakeTextureImportRequest(spec, out).Value());
        CHECK(out.importer == "texture_volume" && out.sources[10].role == "slice:10" &&
              HasOption(out, "pngInfill", "Always") && out.options.Size() == 1);
    }

    {
        AssetImportTranslation out(allocator);
        TextureImportSpec spec;
        spec.kind = TextureImportKind::VolumeFile;
        spec.files = one;
        spec.volumeTileX = 16;
        spec.volumeTileY = 8;
        REQUIRE(MakeTextureImportRequest(spec, out).Value());
        CHECK(out.importer == "texture_volume_file" &&
              out.sources[0].role == DefaultImportSourceRole &&
              HasOption(out, "volumeTileX", "16") && HasOption(out, "volumeTileY", "8"));
    }

    {
        AssetImportTranslation out(allocator);
        TextureImportSpec spec;
        spec.kind = TextureImportKind::CubeFile;
        spec.files = one;
        spec.cubeLayout = TextureCubeLayout::Strip6;
        REQUIRE(MakeTextureImportRequest(spec, out).Value());
        CHECK(HasOption(out, "cubeLayout", "Strip6"));
    }

    struct Bad {
        TextureImportKind kind;
        Span<const StringView> files;
        TextureCubeLayout layout;
        uint32 tileX;
        uint32 tileY;
        TextureImportPngInfill infill;
    };
    const Bad bad[] = {
        {TextureImportKind::Cube, Span<const StringView>(six, 5), TextureCubeLayout::None, 0, 0,
         TextureImportPngInfill::OnlyOnBinaryTransparency},
        {TextureImportKind::CubeArray, Span<const StringView>(twelve, 7), TextureCubeLayout::None,
         0, 0, TextureImportPngInfill::OnlyOnBinaryTransparency},
        {TextureImportKind::Array,
         {},
         TextureCubeLayout::None,
         0,
         0,
         TextureImportPngInfill::OnlyOnBinaryTransparency},
        {TextureImportKind::Texture2D, six, TextureCubeLayout::None, 0, 0,
         TextureImportPngInfill::OnlyOnBinaryTransparency},
        {TextureImportKind::VolumeFile, one, TextureCubeLayout::None, 4, 0,
         TextureImportPngInfill::OnlyOnBinaryTransparency},
        {TextureImportKind::Texture2D, one, TextureCubeLayout::Strip6, 0, 0,
         TextureImportPngInfill::OnlyOnBinaryTransparency},
        {TextureImportKind::Texture2D, one, TextureCubeLayout::None, 2, 2,
         TextureImportPngInfill::OnlyOnBinaryTransparency},
        {TextureImportKind::ArrayFile, one, TextureCubeLayout::None, 0, 0,
         TextureImportPngInfill::Never},
    };
    for (const Bad& item : bad) {
        AssetImportTranslation out(allocator);
        TextureImportSpec spec;
        spec.kind = item.kind;
        spec.files = item.files;
        spec.cubeLayout = item.layout;
        spec.volumeTileX = item.tileX;
        spec.volumeTileY = item.tileY;
        spec.pngInfill = item.infill;
        auto made = MakeTextureImportRequest(spec, out);
        REQUIRE(made);
        CHECK(!made.Value());
    }
}

// 명령 문법 표(12.12절): `--cube=`(면 순서 자유 → 면 순서), `--array=`·`--volume=`, `--cube-array=`
// (`;` 구분), `--cube-layout=`(대소문자 무시), `--volume-tile=<W>x<H>`, 이스케이프 `\,`·`\;`(그 밖
// `\`는 글자 그대로).
TEST(TextureImport_TranslateArguments) {
    SystemAllocator allocator;
    {
        const AssetImportReservedArgument reserved[] = {
            {"cube", "nz:f.png,px:a.png,nx:b.png,py:c.png,ny:d\\,x.png,pz:C:\\Art\\e.png"}};
        AssetImportTranslation out(allocator);
        REQUIRE(Translate(allocator, reserved, {}, {}, out).Value());
        REQUIRE(out.sources.Size() == 6);
        CHECK(out.importer == "texture_cube" && out.sources[0].role == "px" &&
              out.sources[0].path == "a.png" && out.sources[3].path == "d,x.png" &&
              out.sources[4].path == "C:\\Art\\e.png" && out.sources[5].role == "nz" &&
              out.sources[5].path == "f.png");
    }

    {
        const AssetImportReservedArgument reserved[] = {{"array", "s0.png,s1.png,s2.png"}};
        AssetImportTranslation out(allocator);
        REQUIRE(Translate(allocator, reserved, {}, {}, out).Value());
        CHECK(out.importer == "texture_array" && out.sources.Size() == 3 &&
              out.sources[2].role == "slice:2" && out.sources[2].path == "s2.png");
    }

    {
        const AssetImportReservedArgument reserved[] = {
            {"cube-array", "px:a,nx:b,py:c,ny:d,pz:e,nz:f;px:g,nx:h,py:i,ny:j,pz:k,nz:l\\;m"}};
        AssetImportTranslation out(allocator);
        REQUIRE(Translate(allocator, reserved, {}, {}, out).Value());
        CHECK(out.importer == "texture_cube_array" && out.sources.Size() == 12 &&
              out.sources[6].role == "cube:1.px" && out.sources[11].path == "l;m");
    }

    {
        const AssetImportReservedArgument reserved[] = {{"volume", "v0.png"}};
        AssetImportTranslation out(allocator);
        REQUIRE(Translate(allocator, reserved, {}, {}, out).Value());
        CHECK(out.importer == "texture_volume" && out.sources[0].role == "slice:0");
    }

    const AssetImportSourceInput single[] = {{DefaultImportSourceRole, "cross.png"}};
    {
        const AssetImportReservedArgument reserved[] = {{"cube-layout", "horizontalcross"}};
        AssetImportTranslation out(allocator);
        REQUIRE(Translate(allocator, reserved, single, {}, out).Value());
        CHECK(out.importer == "texture_cube_file" && out.sources[0].path == "cross.png" &&
              HasOption(out, "cubeLayout", "HorizontalCross"));
    }

    {
        const AssetImportReservedArgument reserved[] = {{"volume-tile", "16x32"}};
        AssetImportTranslation out(allocator);
        REQUIRE(Translate(allocator, reserved, single, {}, out).Value());
        CHECK(out.importer == "texture_volume_file" && HasOption(out, "volumeTileX", "16") &&
              HasOption(out, "volumeTileY", "32"));
    }

    // 사용법 위반은 false다.
    struct Usage {
        AssetImportReservedArgument first;
        AssetImportReservedArgument second;
        bool withSource;
        StringView importer;
    };
    const AssetImportReservedArgument none{};
    const Usage usages[] = {
        {{"cube", "px:a,nx:b,py:c,ny:d,pz:e"}, none, false, {}},
        {{"cube", "px:a,px:b,py:c,ny:d,pz:e,nz:f"}, none, false, {}},
        {{"cube", "xx:a,nx:b,py:c,ny:d,pz:e,nz:f"}, none, false, {}},
        {{"cube", "px:,nx:b,py:c,ny:d,pz:e,nz:f"}, none, false, {}},
        {{"array", "a,,b"}, none, false, {}},
        {{"array", "a"}, none, true, {}},
        {{"array", "a"}, {"volume", "b"}, false, {}},
        {{"array", "a"}, {"cube-layout", "Strip6"}, false, {}},
        {{"array", "a"}, none, false, "texture"},
        {{"cube-layout", "Diagonal"}, none, true, {}},
        {{"cube-layout", "None"}, none, true, {}},
        {{"cube-layout", "Strip6"}, none, false, {}},
        {{"cube-layout", "Strip6"}, {"volume-tile", "2x2"}, true, {}},
        {{"volume-tile", "2x"}, none, true, {}},
        {{"volume-tile", "0x2"}, none, true, {}},
        {{"volume-tile", "2*2"}, none, true, {}},
        {{"cube-array", "px:a,nx:b,py:c,ny:d,pz:e,nz:f;"}, none, false, {}},
    };
    for (const Usage& item : usages) {
        AssetImportReservedArgument reserved[2] = {item.first, item.second};
        const usize count = item.second.name.IsEmpty() ? 1 : 2;
        AssetImportTranslation out(allocator);
        auto translated =
            Translate(allocator, Span<const AssetImportReservedArgument>(reserved, count),
                      item.withSource ? Span<const AssetImportSourceInput>(single)
                                      : Span<const AssetImportSourceInput>(),
                      item.importer, out);
        REQUIRE(translated);
        CHECK(!translated.Value());
    }

    // 예약 인자 없이 DDS가 아닌 원본 하나는 바꾸지 않는다.
    AssetImportTranslation plain(allocator);
    REQUIRE(Translate(allocator, {}, single, {}, plain).Value());
    CHECK(plain.importer.IsEmpty() && plain.sources.IsEmpty() && plain.options.IsEmpty());
}

namespace {

bool SameTranslation(const AssetImportTranslation& a, const AssetImportTranslation& b) noexcept {
    if (a.importer != b.importer || a.sources.Size() != b.sources.Size() ||
        a.options.Size() != b.options.Size()) {
        return false;
    }

    for (usize index = 0; index < a.sources.Size(); ++index) {
        if (a.sources[index].role != b.sources[index].role ||
            a.sources[index].path != b.sources[index].path) {
            return false;
        }
    }

    for (usize index = 0; index < a.options.Size(); ++index) {
        if (a.options[index].path != b.options[index].path ||
            a.options[index].value != b.options[index].value) {
            return false;
        }
    }

    return true;
}

} // namespace

// 번역 할당 실패 전수(14.1.119 후속 1번): 번역기의 작업 Allocator와 결과(원본·옵션·소유 문자열)의
// n번째 할당이 실패하면 OutOfMemory이고, 실패가 없으면(시도가 다 쓰이면) 기준과 같은 임포터·원본·
// 옵션이다. 사용법 위반은 할당 실패가 아니면 그대로 false다. 구성: Cube(이스케이프), Array,
// CubeArray, Volume, 한 파일 Cube 배치, Volume 타일, 사용법 위반 하나.
TEST(TextureImport_TranslateAllocationFailures) {
    SystemAllocator allocator;
    const AssetImportSourceInput single[] = {{DefaultImportSourceRole, "cross.png"}};
    struct Case {
        AssetImportReservedArgument reserved;
        bool withSource;
        bool valid;
    };
    const Case cases[] = {
        {{"cube", "nz:f.png,px:a.png,nx:b.png,py:c.png,ny:d\\,x.png,pz:e.png"}, false, true},
        {{"array", "s0.png,s1.png,s2.png"}, false, true},
        {{"cube-array", "px:a,nx:b,py:c,ny:d,pz:e,nz:f;px:g,nx:h,py:i,ny:j,pz:k,nz:l"},
         false,
         true},
        {{"volume", "v0.png,v1.png"}, false, true},
        {{"cube-layout", "Strip6"}, true, true},
        {{"volume-tile", "16x32"}, true, true},
        {{"cube", "px:a,px:b,py:c,ny:d,pz:e,nz:f"}, false, false},
    };
    for (const Case& item : cases) {
        const AssetImportReservedArgument reserved[] = {item.reserved};
        const Span<const AssetImportSourceInput> sources =
            item.withSource ? Span<const AssetImportSourceInput>(single)
                            : Span<const AssetImportSourceInput>();
        AssetImportTranslation reference(allocator);
        auto translated = Translate(allocator, reserved, sources, {}, reference);
        REQUIRE(translated && translated.Value() == item.valid);
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(allocator);
            AssetImportTranslation out(failing);
            failing.FailOnAttempt(attempt);
            auto result = Translate(failing, reserved, sources, {}, out);
            const bool exhausted = failing.Attempts() < attempt;
            failing.DisableFailure();
            if (result) {
                CHECK(result.Value() == item.valid);
                CHECK(!item.valid || SameTranslation(out, reference));
            } else {
                ++failures;
                CHECK(result.Error() == ErrorCode::OutOfMemory && !exhausted);
            }

            if (exhausted) {
                break;
            }
        }

        CHECK(failures > 0);
    }
}

// DDS 차원 선택: 원본 하나가 .dds이면 머리의 차원으로 임포터를 고른다(2D는 그대로, `--importer=`가
// 있거나 읽지 못하면 바꾸지 않는다).
TEST(TextureImport_TranslateDdsDimension) {
    SystemAllocator allocator;
    char directory[96] = {};
    const int written = snprintf(directory, sizeof(directory), "rocks-texture-tools-%u",
                                 static_cast<unsigned>(platform::CurrentProcessId()));
    REQUIRE(written > 0);
    const StringView root(directory, static_cast<usize>(written));
    REQUIRE(
        platform::MakeDirectoryTree(platform::PhysicalFileSystem(), root.Data(), root.ByteCount()));
    const uint8 texel[24] = {};
    struct Case {
        const char* name;
        uint32 depth;
        uint32 dimension;
        uint32 misc;
        uint32 arraySize;
        StringView importer;
    };
    const Case cases[] = {
        {"cube.dds", 1, DdsTexture2D, DdsMiscCube, 1, "texture_cube_file"},
        {"cubes.DDS", 1, DdsTexture2D, DdsMiscCube, 2, "texture_cube_array_file"},
        {"array.dds", 1, DdsTexture2D, 0, 2, "texture_array_file"},
        {"volume.dds", 2, DdsTexture3D, 0, 1, "texture_volume_file"},
        {"plain.dds", 1, DdsTexture2D, 0, 1, {}},
    };
    for (const Case& item : cases) {
        const Array<uint8> dds = MakeDds(allocator, 1, 1, item.depth, DxgiRgba8, item.dimension,
                                         item.misc, item.arraySize, texel);
        char path[160] = {};
        const int bytes = snprintf(path, sizeof(path), "%s/%s", directory, item.name);
        REQUIRE(bytes > 0);
        platform::File file;
        REQUIRE(file.Open(platform::PhysicalFileSystem(), path, static_cast<usize>(bytes),
                          platform::FileOpenMode::CreateAlways));
        const platform::FileTransfer put = file.WriteAll(dds.Data(), dds.Size());
        CHECK(put.status && put.bytes == dds.Size());
        CHECK(file.Close());
        const AssetImportSourceInput sources[] = {
            {DefaultImportSourceRole, StringView(path, static_cast<usize>(bytes))}};
        AssetImportTranslation out(allocator);
        REQUIRE(Translate(allocator, {}, sources, {}, out).Value());
        CHECK(out.importer == item.importer && out.sources.IsEmpty());
        AssetImportTranslation named(allocator);
        REQUIRE(Translate(allocator, {}, sources, "texture", named).Value());
        CHECK(named.importer.IsEmpty());
    }

    const AssetImportSourceInput missing[] = {{DefaultImportSourceRole, "no-such-file.dds"}};
    AssetImportTranslation out(allocator);
    REQUIRE(Translate(allocator, {}, missing, {}, out).Value());
    CHECK(out.importer.IsEmpty());
    CHECK(platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), root.Data(),
                                        root.ByteCount()));
}

// 한 파일 Cube·Volume 나누기의 경계: 칸이 정사각이 아니면·타일 slice 수가 상한을 넘으면 거짓.
TEST(TextureImport_ExtractBounds) {
    SystemAllocator allocator;
    uint8 bytes[8 * 3 * 4] = {};
    image::DecodedImage image = Image(allocator, TextureSourceFormat::RGBA8, 8, 3, bytes);
    Array<uint8> out(allocator);
    uint32 face = 0;
    auto extracted =
        texture_import::ExtractCubeLayout(image, TextureCubeLayout::HorizontalCross, out, face);
    REQUIRE(extracted);
    CHECK(!extracted.Value());
    extracted = texture_import::ExtractCubeLayout(image, TextureCubeLayout::None, out, face);
    REQUIRE(extracted);
    CHECK(!extracted.Value());
    uint32 slices = 0;
    auto tiles = texture_import::ExtractVolumeTiles(image, 1, 1, 23, out, slices);
    REQUIRE(tiles);
    CHECK(!tiles.Value());
    tiles = texture_import::ExtractVolumeTiles(image, 1, 1, 24, out, slices);
    REQUIRE(tiles);
    CHECK(tiles.Value() && slices == 24);
}

} // namespace texture_import_rules_tests_file
