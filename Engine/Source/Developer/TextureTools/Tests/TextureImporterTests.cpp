#include "TextureImportTestSupport.h"

#include <Base/InitializerList.h>

// 텍스처 임포터 시험이다(Docs/Design/AssetSystem 12.12절 — 14.1.97절 대조표): 임포터
// 정보(이름·역할· 접두·옵션·확장자·표지), 9개 구성의 성공 경로(크기·형식·slice 순서·bulk), 구성
// 위반, 새 임포트 기본값 1–7번, 회색 자동 감지 범위·여러 원본 규칙, TGA 형식 1·3, float, 재임포트
// 보존·되돌림, 디코딩 실패· 경고 detail.
using namespace rocks;
namespace texture_importer_tests_file {

using namespace rocks::texture_import_test;

namespace {

// 임포트 한 번의 결과다.
struct Imported {
    explicit Imported(TextureEnv& env, Span<const AssetImportSource> sources, const void* options,
                      ObjectHandle existing = {}) noexcept
        : context(env, sources, options, existing) {
    }

    FakeImportContext context;
    bool ok = false;
};

bool Run(TextureImportKind kind, Imported& run) noexcept {
    TextureImporter importer(kind);
    run.ok = static_cast<bool>(importer.Import(run.context));
    return run.ok;
}

AssetImportSource Source(StringView role, const Array<uint8>& bytes,
                         StringView path = "a.png") noexcept {
    return AssetImportSource{role, path, bytes.View()};
}

bool FailedWith(const Imported& run, AssetDiagnosticCode code, uint64 detail) noexcept {
    return !run.ok && run.context.failCode == code && run.context.failDetail == detail;
}

} // namespace

// 임포터 9개의 정보: 이름 = 12.12절 표, 출력 클래스, Cube 고정 역할, 가변 접두와 표지, 옵션 타입,
// `texture`만 기본 확장자(15개). 묶음 등록(이름·확장자로 찾기)과 null state 거부.
TEST(TextureImport_ImporterInfo) {
    TextureEnv env;
    REQUIRE(env.ready);
    const TypeId outputs[TextureImportKindCount] = {
        TypeIdOf<Texture2D>(),        TypeIdOf<TextureCube>(),    TypeIdOf<TextureCube>(),
        TypeIdOf<Texture2DArray>(),   TypeIdOf<Texture2DArray>(), TypeIdOf<TextureCubeArray>(),
        TypeIdOf<TextureCubeArray>(), TypeIdOf<VolumeTexture>(),  TypeIdOf<VolumeTexture>()};
    for (usize index = 0; index < TextureImportKindCount; ++index) {
        const TextureImporter importer(static_cast<TextureImportKind>(index));
        const AssetImporterInfo& info = importer.Info();
        CHECK(info.name == TextureImporterNames[index] && info.outputType == outputs[index] &&
              info.version == 1 && !(info.id == Guid{}));
        const bool variable = index == 3 || index == 5 || index == 7;
        CHECK(info.variableSourcesOnly == variable &&
              info.rolePrefixes.Size() == (variable ? 1u : 0u));
        CHECK(info.roles.Size() == (index == 1 ? TextureCubeFaces : 0u));
        CHECK(info.extensions.Size() == (index == 0 ? 15u : 0u));
        CHECK((info.optionsType == TypeId{}) == (index == 4 || index == 6));
    }

    CHECK(TextureImporter(TextureImportKind::Array).Info().rolePrefixes[0] == "slice:");
    CHECK(TextureImporter(TextureImportKind::CubeArray).Info().rolePrefixes[0] == "cube:");
    CHECK(TextureImporter(TextureImportKind::CubeFile).Info().optionsType ==
          TypeIdOf<TextureCubeFileOptions>());
    CHECK(TextureImporter(TextureImportKind::VolumeFile).Info().optionsType ==
          TypeIdOf<TextureVolumeFileOptions>());

    TextureImporterSet set;
    AssetImporterTable table(env.allocator, env.types);
    Array<AssetImporterRegistration> tokens(env.allocator);
    CHECK(RegisterTextureImporters(nullptr, table, tokens).Error() == ErrorCode::InvalidArgument);
    REQUIRE(RegisterTextureImporters(&set, table, tokens));
    CHECK(table.Count() == TextureImportKindCount && tokens.Size() == TextureImportKindCount);
    CHECK(table.FindByExtension("PNG") == &set.At(TextureImportKind::Texture2D) &&
          table.FindByExtension("dds") == &set.At(TextureImportKind::Texture2D));
    CHECK(table.FindByName("texture_cube_array_file") == &set.At(TextureImportKind::CubeArrayFile));
    tokens.Clear();
}

// 2D PNG: 원본 RGBA8·크기·slice 1·bulk = 픽셀, 새 임포트 기본값(1번 현대 설정 — composite
// Disabled·알파 임계 0.75·normalizeNormals·useNewMipFilter, 6번 SRGB on, 2의 거듭제곱이라 7번
// 없음).
TEST(TextureImport_Texture2DDefaults) {
    TextureEnv env;
    REQUIRE(env.ready);
    const Rgba texels[] = {
        {10, 20, 30, 255}, {40, 50, 60, 255}, {70, 80, 90, 255}, {100, 110, 120, 255}};
    const Array<uint32> channels = Channels(env.allocator, texels);
    const Array<uint8> png = MakePng(env.allocator, 2, 2, PngColor::Rgba, 8, channels.View());
    const AssetImportSource sources[] = {Source(DefaultImportSourceRole, png)};
    Imported run(env, sources, nullptr);
    REQUIRE(Run(TextureImportKind::Texture2D, run));
    Texture* texture = run.context.Imported();
    REQUIRE(texture != nullptr);
    CHECK(run.context.outputType == TypeIdOf<Texture2D>());
    CHECK(texture->source.format == TextureSourceFormat::RGBA8 && texture->source.sizeX == 2 &&
          texture->source.sizeY == 2 && texture->source.numSlices == 1 &&
          texture->source.numMips == 1 && texture->source.numLayers == 1);
    REQUIRE(run.context.bulk.Size() == 16);
    CHECK(run.context.bulk[0] == 10 && run.context.bulk[5] == 50 && run.context.bulk[15] == 255);
    CHECK(texture->source.bulk.rawBytes == 16);
    CHECK(texture->compositeTextureMode == TextureCompositeMode::Disabled &&
          texture->alphaCoverageThresholds.w == 0.75 && texture->alphaCoverageThresholds.x == 0.0 &&
          texture->normalizeNormals == 1 && texture->useNewMipFilter);
    CHECK(texture->compressionSettings == TextureCompressionSettings::Default &&
          texture->srgb == 1 && texture->mipGenSettings == TextureMipGenSettings::FromTextureGroup);
    CHECK(run.context.warnings.IsEmpty());
}

// 회색 자동 감지(2번): RGBA8 회색·불투명 → G8, GA8 A = 255 → G8, GA8 A < 255·RGB가 다른 RGBA8·
// RGBA16 회색은 그대로. G8도 Default·SRGB on(8비트 encoding Unknown).
TEST(TextureImport_GrayDetection) {
    TextureEnv env;
    REQUIRE(env.ready);
    struct Case {
        PngColor color;
        uint32 depth;
        uint32 texels[8];
        usize count;
        TextureSourceFormat expected;
    };
    const Case cases[] = {
        {PngColor::Rgba, 8, {7, 7, 7, 255, 9, 9, 9, 255}, 8, TextureSourceFormat::G8},
        {PngColor::Rgba, 8, {7, 7, 8, 255, 9, 9, 9, 255}, 8, TextureSourceFormat::RGBA8},
        {PngColor::Rgba, 8, {7, 7, 7, 254, 9, 9, 9, 255}, 8, TextureSourceFormat::RGBA8},
        {PngColor::GrayAlpha, 8, {7, 255, 9, 255}, 4, TextureSourceFormat::G8},
        {PngColor::GrayAlpha, 8, {7, 255, 9, 128}, 4, TextureSourceFormat::GA8},
        {PngColor::Rgba, 16, {7, 7, 7, 65535, 9, 9, 9, 65535}, 8, TextureSourceFormat::RGBA16},
    };
    for (const Case& item : cases) {
        const Array<uint8> png = MakePng(env.allocator, 2, 1, item.color, item.depth,
                                         Span<const uint32>(item.texels, item.count));
        const AssetImportSource sources[] = {Source(DefaultImportSourceRole, png)};
        Imported run(env, sources, nullptr);
        REQUIRE(Run(TextureImportKind::Texture2D, run));
        CHECK(run.context.Imported()->source.format == item.expected);
        if (item.expected == TextureSourceFormat::G8) {
            CHECK(run.context.bulk.Size() == 2 && run.context.bulk[0] == 7 &&
                  run.context.bulk[1] == 9);
        }

        CHECK(run.context.Imported()->srgb == 1);
    }

    // DDS RGBA8 회색은 감지하지 않는다(UE ImportDDS).
    const uint8 gray[] = {5, 5, 5, 255};
    const Array<uint8> dds = MakeDds(env.allocator, 1, 1, 1, DxgiRgba8, DdsTexture2D, 0, 1, gray);
    const AssetImportSource sources[] = {Source(DefaultImportSourceRole, dds, "a.dds")};
    Imported run(env, sources, nullptr);
    REQUIRE(Run(TextureImportKind::Texture2D, run));
    CHECK(run.context.Imported()->source.format == TextureSourceFormat::RGBA8);
}

// 2의 거듭제곱이 아닌 원본(7번): 3×5는 NoMipmaps·EditorIcon(4의 배수 아님), 12×4는
// NoMipmaps·Default. Cube(6면 3×3)는 EditorIcon이지만 NoMipmaps는 Texture2D만이다.
TEST(TextureImport_NonPowerOfTwo) {
    TextureEnv env;
    REQUIRE(env.ready);
    const Array<uint8> odd = SolidPng(env.allocator, 3, 5, {1, 2, 3, 255});
    const AssetImportSource oddSources[] = {Source(DefaultImportSourceRole, odd)};
    Imported first(env, oddSources, nullptr);
    REQUIRE(Run(TextureImportKind::Texture2D, first));
    CHECK(first.context.Imported()->mipGenSettings == TextureMipGenSettings::NoMipmaps &&
          first.context.Imported()->compressionSettings == TextureCompressionSettings::EditorIcon);

    const Array<uint8> wide = SolidPng(env.allocator, 12, 4, {1, 2, 3, 255});
    const AssetImportSource wideSources[] = {Source(DefaultImportSourceRole, wide)};
    Imported second(env, wideSources, nullptr);
    REQUIRE(Run(TextureImportKind::Texture2D, second));
    CHECK(second.context.Imported()->mipGenSettings == TextureMipGenSettings::NoMipmaps &&
          second.context.Imported()->compressionSettings == TextureCompressionSettings::Default);

    const Array<uint8> face = SolidPng(env.allocator, 3, 3, {1, 2, 3, 255});
    AssetImportSource faces[TextureCubeFaces];
    for (usize index = 0; index < TextureCubeFaces; ++index) {
        faces[index] = Source(TextureCubeFaceRoles[index], face);
    }

    Imported cube(env, faces, nullptr);
    REQUIRE(Run(TextureImportKind::Cube, cube));
    CHECK(cube.context.Imported()->mipGenSettings == TextureMipGenSettings::FromTextureGroup &&
          cube.context.Imported()->compressionSettings == TextureCompressionSettings::EditorIcon);
}

// TGA 8비트(5번): 형식 3은 Grayscale·SRGB off, 회색 색표 형식 1은 G8·Grayscale, 색 있는 색표 형식
// 1은 RGBA8·Default·SRGB on(UE와 다르다 — 색을 지킨다).
TEST(TextureImport_TgaEightBit) {
    TextureEnv env;
    REQUIRE(env.ready);
    const uint8 indices[] = {0, 1, 1, 0};
    const Array<uint8> gray = MakeTga8(env.allocator, 3, 2, 2, indices);
    const uint8 grayPalette[] = {10, 10, 10, 200, 200, 200};
    const Array<uint8> mappedGray = MakeTga8(env.allocator, 1, 2, 2, indices, grayPalette);
    const uint8 colorPalette[] = {255, 0, 0, 0, 255, 0};
    const Array<uint8> mappedColor = MakeTga8(env.allocator, 1, 2, 2, indices, colorPalette);
    struct Case {
        const Array<uint8>* file;
        TextureSourceFormat format;
        TextureCompressionSettings compression;
        uint8 srgb;
    };
    const Case cases[] = {
        {&gray, TextureSourceFormat::G8, TextureCompressionSettings::Grayscale, 0},
        {&mappedGray, TextureSourceFormat::G8, TextureCompressionSettings::Grayscale, 0},
        {&mappedColor, TextureSourceFormat::RGBA8, TextureCompressionSettings::Default, 1},
    };
    for (const Case& item : cases) {
        const AssetImportSource sources[] = {Source(DefaultImportSourceRole, *item.file, "a.tga")};
        Imported run(env, sources, nullptr);
        REQUIRE(Run(TextureImportKind::Texture2D, run));
        CHECK(run.context.Imported()->source.format == item.format &&
              run.context.Imported()->compressionSettings == item.compression &&
              run.context.Imported()->srgb == item.srgb);
    }
}

// float 결과(4번): DDS RGBA16F → HDR_Compressed·SRGB off. DDS sRGB 표시 RGBA8은 SRGB on, 선형
// RGBA8은 off(6번 encoding).
TEST(TextureImport_FloatAndDdsEncoding) {
    TextureEnv env;
    REQUIRE(env.ready);
    const uint8 half[8] = {0, 0x3c, 0, 0x3c, 0, 0x3c, 0, 0x3c};
    const Array<uint8> hdr = MakeDds(env.allocator, 1, 1, 1, DxgiRgba16F, DdsTexture2D, 0, 1, half);
    const AssetImportSource hdrSources[] = {Source(DefaultImportSourceRole, hdr, "a.dds")};
    Imported floatRun(env, hdrSources, nullptr);
    REQUIRE(Run(TextureImportKind::Texture2D, floatRun));
    CHECK(floatRun.context.Imported()->source.format == TextureSourceFormat::RGBA16F &&
          floatRun.context.Imported()->compressionSettings ==
              TextureCompressionSettings::HDR_Compressed &&
          floatRun.context.Imported()->srgb == 0);

    const uint8 color[] = {1, 2, 3, 255};
    for (uint32 dxgi : {DxgiRgba8Srgb, DxgiRgba8}) {
        const Array<uint8> dds = MakeDds(env.allocator, 1, 1, 1, dxgi, DdsTexture2D, 0, 1, color);
        const AssetImportSource sources[] = {Source(DefaultImportSourceRole, dds, "a.dds")};
        Imported run(env, sources, nullptr);
        REQUIRE(Run(TextureImportKind::Texture2D, run));
        CHECK(run.context.Imported()->srgb == (dxgi == DxgiRgba8Srgb ? 1 : 0));
    }
}

// 여러 원본: Cube 6면 순서(역할 순)·bulk 이음, 크기·형식·정사각 위반은 원본 번호 detail, 일부 면만
// 회색이면 바꾸지 않고(모두 RGBA8 — 형식 일치), 모든 면이 회색이면 모두 G8.
TEST(TextureImport_CubeFaces) {
    TextureEnv env;
    REQUIRE(env.ready);
    Array<Array<uint8>> files(env.allocator);
    for (uint32 index = 0; index < TextureCubeFaces; ++index) {
        REQUIRE(files.TryPushBack(SolidPng(env.allocator, 2, 2, {index, 0, 0, 255})));
    }

    AssetImportSource faces[TextureCubeFaces];
    for (usize index = 0; index < TextureCubeFaces; ++index) {
        faces[index] = Source(TextureCubeFaceRoles[index], files[index]);
    }

    Imported run(env, faces, nullptr);
    REQUIRE(Run(TextureImportKind::Cube, run));
    CHECK(run.context.outputType == TypeIdOf<TextureCube>() &&
          run.context.Imported()->source.numSlices == TextureCubeFaces &&
          run.context.Imported()->source.format == TextureSourceFormat::RGBA8);
    REQUIRE(run.context.bulk.Size() == 6 * 16);
    for (uint32 face = 0; face < TextureCubeFaces; ++face) {
        CHECK(run.context.bulk[face * 16] == face);
    }

    // 면 3이 다른 크기·면 4가 정사각이 아님.
    const Array<uint8> small = SolidPng(env.allocator, 1, 1, {1, 1, 1, 255});
    const Array<uint8> tall = SolidPng(env.allocator, 2, 4, {1, 1, 1, 255});
    AssetImportSource bad[TextureCubeFaces];
    for (usize index = 0; index < TextureCubeFaces; ++index) {
        bad[index] = faces[index];
    }

    bad[3] = Source("ny", small);
    Imported sized(env, bad, nullptr);
    CHECK(!Run(TextureImportKind::Cube, sized));
    CHECK(FailedWith(sized, AssetDiagnosticCode::InvalidImportOption, 3));
    AssetImportSource rect[TextureCubeFaces];
    for (usize index = 0; index < TextureCubeFaces; ++index) {
        rect[index] = Source(TextureCubeFaceRoles[index], tall);
    }

    Imported square(env, rect, nullptr);
    CHECK(!Run(TextureImportKind::Cube, square));
    CHECK(FailedWith(square, AssetDiagnosticCode::InvalidImportOption, 0));

    // 회색 규칙: 면 0만 색, 나머지 회색 → 모두 RGBA8. 모두 회색 → 모두 G8.
    const Array<uint8> grayFace = SolidPng(env.allocator, 2, 2, {9, 9, 9, 255});
    const Array<uint8> colorFace = SolidPng(env.allocator, 2, 2, {9, 1, 9, 255});
    AssetImportSource mixed[TextureCubeFaces];
    AssetImportSource allGray[TextureCubeFaces];
    for (usize index = 0; index < TextureCubeFaces; ++index) {
        mixed[index] = Source(TextureCubeFaceRoles[index], index == 0 ? colorFace : grayFace);
        allGray[index] = Source(TextureCubeFaceRoles[index], grayFace);
    }

    Imported mixedRun(env, mixed, nullptr);
    REQUIRE(Run(TextureImportKind::Cube, mixedRun));
    CHECK(mixedRun.context.Imported()->source.format == TextureSourceFormat::RGBA8);
    Imported grayRun(env, allGray, nullptr);
    REQUIRE(Run(TextureImportKind::Cube, grayRun));
    CHECK(grayRun.context.Imported()->source.format == TextureSourceFormat::G8 &&
          grayRun.context.bulk.Size() == 6 * 4);

    // 감지하지 않는 DDS 면(회색 RGBA8)이 섞이면 PNG 회색 면도 바꾸지 않는다(모두 RGBA8 — 성공).
    uint8 grayTexels[16] = {};
    for (usize texel = 0; texel < 4; ++texel) {
        grayTexels[texel * 4] = 9;
        grayTexels[texel * 4 + 1] = 9;
        grayTexels[texel * 4 + 2] = 9;
        grayTexels[texel * 4 + 3] = 255;
    }

    const Array<uint8> ddsFace =
        MakeDds(env.allocator, 2, 2, 1, DxgiRgba8, DdsTexture2D, 0, 1, grayTexels);
    allGray[2] = Source("py", ddsFace, "py.dds");
    Imported ddsMixed(env, allGray, nullptr);
    REQUIRE(Run(TextureImportKind::Cube, ddsMixed));
    CHECK(ddsMixed.context.Imported()->source.format == TextureSourceFormat::RGBA8 &&
          ddsMixed.context.bulk.Size() == 6 * 16);
}

// 가변 역할: Array는 번호 순(byte 순 `slice:10` < `slice:2`를 수로 정렬), 빈틈·앞 0·중복은 원본
// 번호 detail, Volume도 같다, CubeArray는 cube 번호 × 면 순이고 면이 빠지면(6의 배수 아님) 실패.
TEST(TextureImport_VariableRoles) {
    TextureEnv env;
    REQUIRE(env.ready);
    Array<Array<uint8>> files(env.allocator);
    for (uint32 index = 0; index < 12; ++index) {
        REQUIRE(files.TryPushBack(SolidPng(env.allocator, 1, 1, {index, 0, 0, 255})));
    }

    // 프레임워크가 주는 byte 순: slice:0, slice:1, slice:10, slice:11, slice:2, …
    constexpr StringView Roles[] = {"slice:0", "slice:1", "slice:10", "slice:11",
                                    "slice:2", "slice:3", "slice:4",  "slice:5",
                                    "slice:6", "slice:7", "slice:8",  "slice:9"};
    constexpr uint32 Numbers[] = {0, 1, 10, 11, 2, 3, 4, 5, 6, 7, 8, 9};
    AssetImportSource slices[12];
    for (usize index = 0; index < 12; ++index) {
        slices[index] = Source(Roles[index], files[Numbers[index]]);
    }

    for (TextureImportKind kind : {TextureImportKind::Array, TextureImportKind::Volume}) {
        Imported run(env, slices, nullptr);
        REQUIRE(Run(kind, run));
        REQUIRE(run.context.bulk.Size() == 12 * 4);
        for (uint32 slice = 0; slice < 12; ++slice) {
            CHECK(run.context.bulk[slice * 4] == slice);
        }

        CHECK(run.context.Imported()->source.numSlices == 12);
    }

    const AssetImportSource gap[] = {Source("slice:0", files[0]), Source("slice:2", files[1])};
    Imported gapRun(env, gap, nullptr);
    CHECK(!Run(TextureImportKind::Array, gapRun) &&
          FailedWith(gapRun, AssetDiagnosticCode::InvalidImportOption, 1));
    const AssetImportSource leading[] = {Source("slice:0", files[0]), Source("slice:01", files[1])};
    Imported leadingRun(env, leading, nullptr);
    CHECK(!Run(TextureImportKind::Array, leadingRun) &&
          FailedWith(leadingRun, AssetDiagnosticCode::InvalidImportOption, 1));

    // CubeArray: cube 1의 면이 cube 0보다 먼저 와도 cube × 면 순으로 놓는다.
    AssetImportSource cubes[12];
    constexpr StringView CubeRoles[] = {"cube:0.nx", "cube:0.ny", "cube:0.nz", "cube:0.px",
                                        "cube:0.py", "cube:0.pz", "cube:1.nx", "cube:1.ny",
                                        "cube:1.nz", "cube:1.px", "cube:1.py", "cube:1.pz"};
    constexpr uint32 CubeSlots[] = {1, 3, 5, 0, 2, 4, 7, 9, 11, 6, 8, 10};
    for (usize index = 0; index < 12; ++index) {
        cubes[index] = Source(CubeRoles[index], files[CubeSlots[index]]);
    }

    Imported cubeRun(env, cubes, nullptr);
    REQUIRE(Run(TextureImportKind::CubeArray, cubeRun));
    REQUIRE(cubeRun.context.bulk.Size() == 12 * 4);
    for (uint32 slot = 0; slot < 12; ++slot) {
        CHECK(cubeRun.context.bulk[slot * 4] == slot);
    }

    Imported partial(env, Span<const AssetImportSource>(cubes, 11), nullptr);
    CHECK(!Run(TextureImportKind::CubeArray, partial) &&
          FailedWith(partial, AssetDiagnosticCode::InvalidImportOption, 11));
    AssetImportSource badFace[6];
    for (usize index = 0; index < 6; ++index) {
        badFace[index] = cubes[index];
    }

    badFace[2] = Source("cube:0.zz", files[2]);
    Imported faceRun(env, badFace, nullptr);
    CHECK(!Run(TextureImportKind::CubeArray, faceRun) &&
          FailedWith(faceRun, AssetDiagnosticCode::InvalidImportOption, 2));
}

// 한 파일 Cube: HorizontalCross·VerticalCross(−Z 180° 되돌림)·Strip6의 면 순서, 칸이 나뉘지 않으면
// 실패, None은 DDS Cube만(PNG는 결과 차원 값 detail).
TEST(TextureImport_CubeLayouts) {
    TextureEnv env;
    REQUIRE(env.ready);
    // 칸마다 (면 번호 + 1) × 10 색을 칠한다(빈 칸은 0). 면 칸 위치는 12.3절 표.
    struct Layout {
        TextureCubeLayout layout;
        uint32 columns;
        uint32 rows;
        uint32 cells[6][2];
    };
    const Layout layouts[] = {
        {TextureCubeLayout::HorizontalCross,
         4,
         3,
         {{2, 1}, {0, 1}, {1, 0}, {1, 2}, {1, 1}, {3, 1}}},
        {TextureCubeLayout::VerticalCross, 3, 4, {{2, 1}, {0, 1}, {1, 0}, {1, 2}, {1, 1}, {1, 3}}},
        {TextureCubeLayout::Strip6, 6, 1, {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}}},
    };
    constexpr uint32 Face = 2;
    for (const Layout& item : layouts) {
        const uint32 width = item.columns * Face;
        const uint32 height = item.rows * Face;
        Array<uint32> texels(env.allocator);
        REQUIRE(texels.TryResize(usize{width} * height * 4));
        for (uint32& value : texels) {
            value = 255;
        }

        for (uint32 face = 0; face < 6; ++face) {
            for (uint32 y = 0; y < Face; ++y) {
                for (uint32 x = 0; x < Face; ++x) {
                    const usize at = ((usize{item.cells[face][1]} * Face + y) * width +
                                      item.cells[face][0] * Face + x) *
                                     4;
                    // r = 면 표지, g = 칸 안 위치(y × 2 + x — 회전 확인).
                    texels[at] = (face + 1) * 10;
                    texels[at + 1] = y * Face + x;
                }
            }
        }

        const Array<uint8> png =
            MakePng(env.allocator, width, height, PngColor::Rgba, 8, texels.View());
        const AssetImportSource sources[] = {Source(DefaultImportSourceRole, png)};
        TextureCubeFileOptions options;
        options.cubeLayout = item.layout;
        Imported run(env, sources, &options);
        REQUIRE(Run(TextureImportKind::CubeFile, run));
        CHECK(run.context.Imported()->source.sizeX == Face &&
              run.context.Imported()->source.numSlices == 6);
        REQUIRE(run.context.bulk.Size() == 6 * Face * Face * 4);
        for (uint32 face = 0; face < 6; ++face) {
            const uint8* first = run.context.bulk.Data() + usize{face} * Face * Face * 4;
            const bool rotated = item.layout == TextureCubeLayout::VerticalCross && face == 5;
            CHECK(first[0] == (face + 1) * 10 && first[1] == (rotated ? 3 : 0));
        }
    }

    const Array<uint8> uneven = SolidPng(env.allocator, 8, 5, {1, 1, 1, 255});
    const AssetImportSource unevenSources[] = {Source(DefaultImportSourceRole, uneven)};
    TextureCubeFileOptions cross;
    cross.cubeLayout = TextureCubeLayout::HorizontalCross;
    Imported unevenRun(env, unevenSources, &cross);
    CHECK(!Run(TextureImportKind::CubeFile, unevenRun) &&
          FailedWith(unevenRun, AssetDiagnosticCode::InvalidImportOption, 0));

    const Array<uint8> plain = SolidPng(env.allocator, 2, 2, {1, 1, 1, 255});
    const AssetImportSource plainSources[] = {Source(DefaultImportSourceRole, plain)};
    TextureCubeFileOptions none;
    Imported plainRun(env, plainSources, &none);
    CHECK(!Run(TextureImportKind::CubeFile, plainRun) &&
          FailedWith(plainRun, AssetDiagnosticCode::InvalidImportOption,
                     static_cast<uint64>(TextureDimension::Texture2D)));

    uint8 cubeData[6 * 4] = {};
    for (uint32 face = 0; face < 6; ++face) {
        cubeData[face * 4] = static_cast<uint8>(face);
        cubeData[face * 4 + 3] = 255;
    }

    const Array<uint8> dds =
        MakeDds(env.allocator, 1, 1, 1, DxgiRgba8, DdsTexture2D, DdsMiscCube, 1, cubeData);
    const AssetImportSource ddsSources[] = {Source(DefaultImportSourceRole, dds, "a.dds")};
    Imported ddsRun(env, ddsSources, &none);
    REQUIRE(Run(TextureImportKind::CubeFile, ddsRun));
    CHECK(ddsRun.context.Imported()->source.numSlices == 6 && ddsRun.context.bulk[20] == 5);

    // `texture`에 DDS Cube는 결과 차원 값 detail로 실패한다.
    Imported plainDds(env, ddsSources, nullptr);
    CHECK(!Run(TextureImportKind::Texture2D, plainDds) &&
          FailedWith(plainDds, AssetDiagnosticCode::InvalidImportOption,
                     static_cast<uint64>(TextureDimension::TextureCube)));
}

// 한 파일 Volume: 타일 2×2로 4×4 그림을 왼쪽 → 오른쪽, 위 → 아래 slice 4개로, 나뉘지 않는
// 타일·한쪽만 0인 타일은 실패, 타일 0×0은 DDS Volume만. Array·CubeArray 파일은 차원이 같아야 한다.
TEST(TextureImport_VolumeAndArrayFiles) {
    TextureEnv env;
    REQUIRE(env.ready);
    Array<uint32> texels(env.allocator);
    for (uint32 y = 0; y < 4; ++y) {
        for (uint32 x = 0; x < 4; ++x) {
            const uint32 tile = (y / 2) * 2 + x / 2;
            REQUIRE(texels.TryPushBack(tile) && texels.TryPushBack(0) && texels.TryPushBack(0) &&
                    texels.TryPushBack(255));
        }
    }

    const Array<uint8> png = MakePng(env.allocator, 4, 4, PngColor::Rgba, 8, texels.View());
    const AssetImportSource sources[] = {Source(DefaultImportSourceRole, png)};
    TextureVolumeFileOptions options;
    options.volumeTileX = 2;
    options.volumeTileY = 2;
    Imported run(env, sources, &options);
    REQUIRE(Run(TextureImportKind::VolumeFile, run));
    CHECK(run.context.Imported()->source.numSlices == 4 &&
          run.context.Imported()->source.sizeX == 2);
    REQUIRE(run.context.bulk.Size() == 64);
    for (uint32 slice = 0; slice < 4; ++slice) {
        for (uint32 texel = 0; texel < 4; ++texel) {
            CHECK(run.context.bulk[(slice * 4 + texel) * 4] == slice);
        }
    }

    TextureVolumeFileOptions uneven;
    uneven.volumeTileX = 3;
    uneven.volumeTileY = 2;
    Imported unevenRun(env, sources, &uneven);
    CHECK(!Run(TextureImportKind::VolumeFile, unevenRun) &&
          FailedWith(unevenRun, AssetDiagnosticCode::InvalidImportOption, 0));
    TextureVolumeFileOptions half;
    half.volumeTileX = 2;
    Imported halfRun(env, sources, &half);
    CHECK(!Run(TextureImportKind::VolumeFile, halfRun) &&
          FailedWith(halfRun, AssetDiagnosticCode::InvalidImportOption, 0));
    TextureVolumeFileOptions zero;
    Imported zeroRun(env, sources, &zero);
    CHECK(!Run(TextureImportKind::VolumeFile, zeroRun) &&
          FailedWith(zeroRun, AssetDiagnosticCode::InvalidImportOption,
                     static_cast<uint64>(TextureDimension::Texture2D)));

    uint8 voxels[2 * 4] = {0, 0, 0, 255, 1, 0, 0, 255};
    const Array<uint8> volume =
        MakeDds(env.allocator, 1, 1, 2, DxgiRgba8, DdsTexture3D, 0, 1, voxels);
    const AssetImportSource volumeSources[] = {Source(DefaultImportSourceRole, volume, "v.dds")};
    Imported volumeRun(env, volumeSources, &zero);
    REQUIRE(Run(TextureImportKind::VolumeFile, volumeRun));
    CHECK(volumeRun.context.Imported()->source.numSlices == 2);

    const Array<uint8> array =
        MakeDds(env.allocator, 1, 1, 1, DxgiRgba8, DdsTexture2D, 0, 2, voxels);
    const AssetImportSource arraySources[] = {Source(DefaultImportSourceRole, array, "r.dds")};
    Imported arrayRun(env, arraySources, nullptr);
    REQUIRE(Run(TextureImportKind::ArrayFile, arrayRun));
    CHECK(arrayRun.context.outputType == TypeIdOf<Texture2DArray>() &&
          arrayRun.context.Imported()->source.numSlices == 2);
    Imported wrongRun(env, arraySources, nullptr);
    CHECK(!Run(TextureImportKind::CubeArrayFile, wrongRun) &&
          FailedWith(wrongRun, AssetDiagnosticCode::InvalidImportOption,
                     static_cast<uint64>(TextureDimension::Texture2DArray)));
}

// 디코딩 실패는 디코더 진단 + 원본 번호 detail, 경고는 (원본 번호 << 32) | 디코더 detail.
TEST(TextureImport_DecoderDiagnostics) {
    TextureEnv env;
    REQUIRE(env.ready);
    const Array<uint8> good = SolidPng(env.allocator, 1, 1, {1, 2, 3, 255});
    const uint8 garbage[] = {1, 2, 3, 4};
    Array<uint8> broken(env.allocator);
    REQUIRE(broken.TryAppend(Span<const uint8>(garbage)));
    AssetImportSource faces[TextureCubeFaces];
    for (usize index = 0; index < TextureCubeFaces; ++index) {
        faces[index] = Source(TextureCubeFaceRoles[index], good);
    }

    faces[4] = Source("pz", broken, "broken.bin");
    Imported failed(env, faces, nullptr);
    CHECK(!Run(TextureImportKind::Cube, failed) &&
          FailedWith(failed, AssetDiagnosticCode::UnsupportedSourceFormat, 4));

    // gAMA 1.0(선형 표시)인 PNG는 NonSrgbSource 경고다.
    const uint32 channels[] = {1, 2, 3, 255};
    const Array<uint8> linear = MakePng(env.allocator, 1, 1, PngColor::Rgba, 8, channels, 100000);
    faces[4] = Source("pz", good);
    faces[2] = Source("py", linear);
    Imported warned(env, faces, nullptr);
    REQUIRE(Run(TextureImportKind::Cube, warned));
    REQUIRE(warned.context.warnings.Size() == 1);
    CHECK(warned.context.warnings[0].code == AssetDiagnosticCode::NonSrgbSource &&
          (warned.context.warnings[0].detail >> 32) == 2);
}

// 재임포트: 설정은 그대로(사용자 srgb 0 유지), 원본만 바뀐다. NoMipmaps·EditorIcon Texture2D가
// 2의 거듭제곱 원본을 받으면 FromTextureGroup·Default·neverStream 0으로 되돌린다. UI 그룹은 그대로,
// powerOfTwoMode ≠ None이면 2의 거듭제곱이 아니어도 되돌린다.
TEST(TextureImport_Reimport) {
    TextureEnv env;
    REQUIRE(env.ready);
    const Array<uint8> odd = SolidPng(env.allocator, 3, 5, {1, 2, 3, 255});
    const Array<uint8> pow2 = SolidPng(env.allocator, 4, 4, {1, 2, 3, 255});
    const Array<uint8> odd2 = SolidPng(env.allocator, 6, 6, {1, 2, 3, 255});
    struct Case {
        const Array<uint8>* next;
        TextureGroup group;
        TexturePowerOfTwoSetting mode;
        bool reverted;
    };
    const Case cases[] = {
        {&pow2, TextureGroup::World, TexturePowerOfTwoSetting::None, true},
        {&pow2, TextureGroup::UI, TexturePowerOfTwoSetting::None, false},
        {&odd2, TextureGroup::World, TexturePowerOfTwoSetting::None, false},
        {&odd2, TextureGroup::World, TexturePowerOfTwoSetting::PadToPowerOfTwo, true},
    };
    for (const Case& item : cases) {
        const AssetImportSource first[] = {Source(DefaultImportSourceRole, odd)};
        Imported created(env, first, nullptr);
        REQUIRE(Run(TextureImportKind::Texture2D, created));
        Texture* texture = created.context.Imported();
        REQUIRE(texture != nullptr);
        texture->srgb = 0;
        texture->neverStream = 1;
        texture->lodGroup = item.group;
        texture->powerOfTwoMode = item.mode;
        const AssetImportSource next[] = {Source(DefaultImportSourceRole, *item.next)};
        Imported again(env, next, nullptr, created.context.output);
        REQUIRE(Run(TextureImportKind::Texture2D, again));
        Texture* updated = again.context.Imported();
        CHECK(updated == texture && updated->srgb == 0 &&
              updated->source.sizeX == (item.next == &pow2 ? 4u : 6u));
        CHECK((updated->mipGenSettings == TextureMipGenSettings::FromTextureGroup) ==
              item.reverted);
        CHECK((updated->compressionSettings == TextureCompressionSettings::Default) ==
              item.reverted);
        CHECK((updated->neverStream == 0) == item.reverted);
    }
}

} // namespace texture_importer_tests_file
