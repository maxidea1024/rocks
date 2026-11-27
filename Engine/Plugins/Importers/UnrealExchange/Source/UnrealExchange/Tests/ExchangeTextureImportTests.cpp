#include "../../../../../../Source/Developer/AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "ExchangeTestSupport.h"

#include <AssetCommandlets/AssetEditCommandlet.h>
#include <Core/Commandlet/Commandlet.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UnrealExchange/ExchangeCommand.h>
#include <UnrealExchange/ExchangePaths.h>
#include <UnrealExchange/ExchangeServiceKind.h>
#include <UnrealExchange/UnrealExchangeImporterSet.h>

// 교환 텍스처 임포트 시험이다(Docs/Design/UnrealExchange 4.6절 7번, 10.6절): 디스크 프로젝트에 합성
// 묶음을 쓰고 명령 함수를 이 프로세스에서 부른다. asset-import-unreal의 새 임포트(원본·설정·참조·
// 기록 원본 역할), 재임포트(id 유지), 일반 asset-reimport, 이름 바꾸기 두 번의 FormerPaths 누적,
// 그리고 asset-import로 A2f 임포터의 거부(검사 순서 10.6절 6번)와
// 경고(GammaMismatch·UnsupportedSetting).
using namespace rocks;
using namespace rocks::asset_database_test;
namespace exchange_texture_import_tests_file {

namespace {

// 2×2 BGRA8 원본 16 byte다.
constexpr uint8 PixelBytes[] = {10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 1, 2, 3, 4};

// 표준 출력·오류를 모은다.
struct Captured {
    explicit Captured(Allocator& allocator) noexcept : out(allocator), error(allocator) {
    }

    static Result<void> Write(void* context, CommandletStream stream, StringView text) noexcept {
        Captured& captured = *static_cast<Captured*>(context);
        return (stream == CommandletStream::Output ? captured.out : captured.error).TryAppend(text);
    }

    String out;
    String error;
};

// 텍스처 임포터를 건 명령 문맥이다.
struct Tool {
    explicit Tool(Allocator& allocator) noexcept : captured(allocator) {
        assets.allocator = &allocator;
        assets.modules = &test::ProcessModules();
        assets.importers = {&RegisterUnrealExchangeImporters, &importers};
        unreal.assets = &assets;
        unreal.kinds = {&RegisterExchangeServiceKinds, &kind};
    }

    int32 Call(Result<int32> (*command)(void*, Span<const StringView>, CommandletOutput&),
               void* context, Span<const StringView> args) noexcept {
        captured.out.Clear();
        captured.error.Clear();
        CommandletOutput output{&Captured::Write, &captured};
        auto ran = command(context, args, output);
        return ran ? ran.Value() : -1;
    }

    int32 Unreal(Span<const StringView> args) noexcept {
        return Call(&RunAssetImportUnrealCommand, &unreal, args);
    }

    int32 Import(Span<const StringView> args) noexcept {
        return Call(&commandlet_host::RunAssetImportCommand, &assets, args);
    }

    int32 Reimport(Span<const StringView> args) noexcept {
        return Call(&commandlet_host::RunAssetReimportCommand, &assets, args);
    }

    int32 Info(Span<const StringView> args) noexcept {
        return Call(&commandlet_host::RunAssetInfoCommand, &assets, args);
    }

    bool Out(StringView text) const noexcept {
        return StringView(captured.out.View()).Contains(text, StringComparison::CaseSensitive);
    }

    bool Error(StringView text) const noexcept {
        return StringView(captured.error.View()).Contains(text, StringComparison::CaseSensitive);
    }

    UnrealExchangeImporterSet importers;
    ExchangeServiceKind kind;
    commandlet_host::AssetCommandContext assets;
    UnrealExchangeCommandContext unreal;
    Captured captured;
};

// 합성 묶음의 텍스처 하나다. settings·dependencies·diagnostics는 JSON 원문이다.
struct TextureSpec {
    StringView objectPath;
    StringView className;
    StringView settings;
    StringView dependencies;
    StringView gamma;
    StringView diagnostics;
    // 원본 slice 수와 픽셀 씨앗이다(씨앗 0·slice 1이면 PixelBytes).
    uint32 slices;
    uint32 seed;
};

constexpr TextureSpec Spec(StringView objectPath, StringView className = "Texture2D",
                           StringView settings = "{\"SRGB\": true}", StringView dependencies = "[]",
                           StringView gamma = "sRGB", StringView diagnostics = "[]",
                           uint32 slices = 1, uint32 seed = 0) noexcept {
    return TextureSpec{objectPath, className,   settings, dependencies,
                       gamma,      diagnostics, slices,   seed};
}

// 2×2 BGRA8 slice 하나의 byte 수다.
constexpr usize SliceBytes = sizeof(PixelBytes);

// spec의 원본 byte다(씨앗 0·slice 1은 PixelBytes, 그 밖은 씨앗으로 정한 값).
bool PixelsOf(const TextureSpec& spec, Array<uint8>& out) noexcept {
    out.Clear();
    for (usize index = 0; index < SliceBytes * spec.slices; ++index) {
        const uint8 value = spec.seed == 0 && spec.slices == 1
                                ? PixelBytes[index]
                                : static_cast<uint8>(spec.seed * 37 + index * 7 + 1);
        if (!out.TryPushBack(value)) {
            return false;
        }
    }

    return true;
}

struct RedirectSpec {
    StringView from;
    StringView to;
};

// 디스크 프로젝트와 묶음 작성기다.
struct Scene {
    explicit Scene(Allocator& allocator, const char* tag) noexcept
        : allocator(allocator), root(tag), project(allocator) {
        const Sha256Digest digest = Sha256::Hash(Span<const uint8>(PixelBytes));
        digest.ToHex(hex);
        ready = root.made && project.TryAppendFormat("--project={}", root.Root()) &&
                WriteText("Scene.rocksproject", "{}\n");
    }

    bool WriteBytes(StringView relative, Span<const uint8> bytes) noexcept {
        char path[1024] = {};
        FixedStringBuilder builder{Span<char>(path)};
        if (!builder.TryAppendFormat("{}/{}", root.Root(), relative)) {
            return false;
        }

        const StringView full = builder.View();
        usize slash = full.ByteCount();
        while (slash != 0 && full.Data()[slash - 1] != '/') {
            --slash;
        }

        platform::File file;
        return (slash == 0 || platform::MakeDirectoryTree(platform::PhysicalFileSystem(),
                                                          full.Data(), slash - 1)) &&
               static_cast<bool>(file.Open(platform::PhysicalFileSystem(), full.Data(),
                                           full.ByteCount(),
                                           platform::FileOpenMode::CreateAlways)) &&
               static_cast<bool>(file.WriteAll(bytes.Data(), bytes.Size()).status) &&
               static_cast<bool>(file.Close());
    }

    bool WriteText(StringView relative, StringView text) noexcept {
        return WriteBytes(relative, Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()),
                                                      text.ByteCount()));
    }

    // 텍스처 에셋 JSON 원문이다.
    bool AssetText(const TextureSpec& spec, StringBuilderBase& out) const noexcept {
        Array<uint8> pixels(allocator);
        if (!PixelsOf(spec, pixels)) {
            return false;
        }

        char digest[Sha256Digest::HexBytes + 1];
        Sha256::Hash(pixels.View()).ToHex(digest);
        return static_cast<bool>(out.TryAppendFormat(
            "{{\"objectPath\": \"{}\", \"class\": \"{}\", \"ueClass\": \"/Script/Engine.{}\", "
            "\"dependencies\": {}, \"diagnostics\": {}, \"settings\": {}, \"data\": {{\"source\": "
            "{{\"format\": \"BGRA8\", \"compression\": \"None\", \"lossyOrigin\": null, "
            "\"longLat\": false, \"numLayers\": 1, \"numBlocks\": 1, \"layers\": [{{\"format\": "
            "\"BGRA8\", \"gammaSpace\": \"{}\"}}], \"blocks\": [{{\"blockX\": 0, \"blockY\": 0, "
            "\"sizeX\": 2, \"sizeY\": 2, \"numSlices\": {}, \"numMips\": 1, \"mips\": "
            "[{{\"sizeX\": "
            "2, \"sizeY\": 2, \"numSlices\": {}, \"pixels\": [{{\"blob\": \"{}\", \"bytes\": {}, "
            "\"layout\": \"u8\", \"components\": 4, \"count\": {}}}]}}]}}]}}}}}}\n",
            spec.objectPath, spec.className, spec.className, spec.dependencies, spec.diagnostics,
            spec.settings, spec.gamma, spec.slices, spec.slices,
            StringView(digest, Sha256Digest::HexBytes), pixels.Size(), pixels.Size() / 4));
    }

    // directory(프로젝트 안 상대)에 묶음을 쓴다. textures는 objectPath 순이어야 한다.
    bool WriteBundle(StringView directory, Span<const TextureSpec> textures,
                     Span<const RedirectSpec> redirects = {}) noexcept {
        StringBuilder<1024> manifest(allocator);
        bool ok = static_cast<bool>(manifest.TryAppend(
            "{\"formatVersion\": 1, \"exporter\": {\"plugin\": \"RocksExport\", \"pluginVersion\": "
            "\"1.0.0\", \"engineVersion\": \"5.8.4\", \"engineCommit\": \"0\"}, "
            "\"projectSettings\": {\"substrate\": false, \"retainJpegFormat\": true}, "
            "\"assets\": ["));
        for (usize index = 0; ok && index < textures.Size(); ++index) {
            const TextureSpec& spec = textures[index];
            const StringView package = UnrealPackagePath(spec.objectPath);
            StringBuilder<1024> text(allocator);
            StringBuilder<256> file(allocator);
            ok = (index == 0 || manifest.TryAppend(", ")) &&
                 manifest.TryAppendFormat(
                     "{{\"objectPath\": \"{}\", \"class\": \"{}\", \"file\": \"assets{}.json\"}}",
                     spec.objectPath, spec.className, package) &&
                 AssetText(spec, text) &&
                 file.TryAppendFormat("{}/assets{}.json", directory, package) &&
                 WriteText(file.View(), text.View()) && WriteBlob(directory, spec);
        }

        ok = ok && manifest.TryAppend("], \"redirects\": [");
        for (usize index = 0; ok && index < redirects.Size(); ++index) {
            ok = (index == 0 || manifest.TryAppend(", ")) &&
                 manifest.TryAppendFormat("{{\"from\": \"{}\", \"to\": \"{}\"}}",
                                          redirects[index].from, redirects[index].to);
        }

        StringBuilder<256> bundleFile(allocator);
        return ok && manifest.TryAppend("]}\n") &&
               bundleFile.TryAppendFormat("{}/bundle.json", directory) &&
               WriteText(bundleFile.View(), manifest.View());
    }

    // spec의 blob 파일을 directory 아래에 쓴다.
    bool WriteBlob(StringView directory, const TextureSpec& spec) noexcept {
        Array<uint8> pixels(allocator);
        if (!PixelsOf(spec, pixels)) {
            return false;
        }

        char digest[Sha256Digest::HexBytes + 1];
        Sha256::Hash(pixels.View()).ToHex(digest);
        StringBuilder<256> blobFile(allocator);
        return blobFile.TryAppendFormat("{}/blobs/{}/{}.bin", directory, StringView(digest, 2),
                                        StringView(digest, Sha256Digest::HexBytes)) &&
               WriteBytes(blobFile.View(), pixels.View());
    }

    // 명령 결과 출력(표준 출력)을 copy에 남긴다.
    static bool Keep(const Tool& tool, String& copy) noexcept {
        return static_cast<bool>(copy.TryAssign(tool.captured.out.View()));
    }

    // 프로젝트 안 상대 경로의 명령 인자 텍스트(`<root>/<relative>`)다.
    bool PathOf(StringView relative, StringBuilderBase& out) const noexcept {
        return static_cast<bool>(out.TryAppendFormat("{}/{}", root.Root(), relative));
    }

    Allocator& allocator;
    TempProject root;
    StringBuilder<512> project;
    char hex[Sha256Digest::HexBytes + 1] = {};
    bool ready = false;
};

const TextureSpec RockAndArray[] = {
    Spec(
        "/Game/T/Arr.Arr", "Texture2DArray",
        R"({"SourceTextures": [{"objectPath": "/Game/T/Rock.Rock"}], "AddressZ": "TA_Mirror", "SRGB": true})",
        R"([{"objectPath": "/Game/T/Rock.Rock", "class": "Texture2D", "kind": "hard"}])"),
    Spec("/Game/T/Rock.Rock", "Texture2D",
         R"({"SRGB": true, "LODGroup": "TEXTUREGROUP_UI", "AddressX": "TA_Clamp"})"),
};

bool StartExchangeModule() noexcept {
    return test::ProcessModules().Load("UnrealExchange").IsOk();
}

} // namespace

// asset-import-unreal: 새 임포트(원본 형식·B·R 교환은 원본 변환 시험이 본다 — 여기는
// 형식·설정·참조· 기록 원본 역할), 다시 실행하면 재임포트, 일반 asset-reimport. 이름을 두 번 바꾼
// 묶음(redirect가 처음 이름만 안다)도 같은 에셋으로 이어진다(FormerPaths 누적).
TEST(UnrealExchangeTexture_ImportCommand) {
    SystemAllocator allocator;
    {
        REQUIRE(StartExchangeModule());
        Scene scene(allocator, "ux2b-command");
        REQUIRE(scene.ready);
        REQUIRE(scene.WriteBundle("Out/One.rxbundle", Span<const TextureSpec>(RockAndArray)));
        StringBuilder<512> bundle(allocator);
        REQUIRE(scene.PathOf("Out/One.rxbundle", bundle));
        Tool tool(allocator);
        const StringView run[] = {bundle.View(), scene.project.View()};
        CHECK(tool.Unreal(run) == 0);
        CHECK(
            tool.Out("asset-import-unreal: created /Game/T/Rock.Rock /Game/Unreal/Game/T/Rock ") &&
            tool.Out("asset-import-unreal: created /Game/T/Arr.Arr /Game/Unreal/Game/T/Arr ") &&
            tool.Out("asset-import-unreal: 2 created, 0 reimported, 0 failed, 0 skipped"));

        const StringView rockInfo[] = {"/Game/Unreal/Game/T/Rock", scene.project.View()};
        CHECK(tool.Info(rockInfo) == 0);
        CHECK(tool.Out("source.format=RGBA8") && tool.Out("lodGroup=UI") &&
              tool.Out("addressX=Clamp"));
        const StringView arrInfo[] = {"/Game/Unreal/Game/T/Arr", scene.project.View()};
        CHECK(tool.Info(arrInfo) == 0);
        CHECK(tool.Out("addressZ=Mirror") && tool.Out("/Game/Unreal/Game/T/Rock"));
        const StringView importInfo[] = {"/Game/Unreal/Game/T/Rock", "--import",
                                         scene.project.View()};
        CHECK(tool.Info(importInfo) == 0);
        CHECK(tool.Out("import.importer=unreal_texture_2d 1 current") &&
              tool.Out("import.source.asset=") && tool.Out("import.source.blob:"));

        CHECK(tool.Unreal(run) == 0);
        CHECK(tool.Out("asset-import-unreal: 0 created, 2 reimported, 0 failed, 0 skipped"));

        // 일반 asset-reimport는 같은 결과다(필드 출력이 같다).
        CHECK(tool.Info(rockInfo) == 0);
        String before(allocator);
        REQUIRE(Scene::Keep(tool, before));
        const StringView plain[] = {"/Game/Unreal/Game/T/Rock", scene.project.View()};
        CHECK(tool.Reimport(plain) == 0);
        CHECK(tool.Info(rockInfo) == 0);
        CHECK(StringView(tool.captured.out.View()) == before.View());

        // 이름 바꾸기: Rock → Stone(redirect Rock → Stone), 다시 Stone → Pebble(redirect는 Rock만
        // 안다).
        const TextureSpec stone[] = {Spec("/Game/T/Stone.Stone")};
        const RedirectSpec rockToStone[] = {{"/Game/T/Rock.Rock", "/Game/T/Stone.Stone"}};
        REQUIRE(scene.WriteBundle("Out/Two.rxbundle", Span<const TextureSpec>(stone),
                                  Span<const RedirectSpec>(rockToStone)));
        StringBuilder<512> two(allocator);
        REQUIRE(scene.PathOf("Out/Two.rxbundle", two));
        const StringView runTwo[] = {two.View(), scene.project.View()};
        CHECK(tool.Unreal(runTwo) == 0);
        CHECK(tool.Out(
            "asset-import-unreal: reimported /Game/T/Stone.Stone /Game/Unreal/Game/T/Rock "));

        const TextureSpec pebble[] = {Spec("/Game/T/Pebble.Pebble")};
        const RedirectSpec rockToPebble[] = {{"/Game/T/Rock.Rock", "/Game/T/Pebble.Pebble"}};
        REQUIRE(scene.WriteBundle("Out/Three.rxbundle", Span<const TextureSpec>(pebble),
                                  Span<const RedirectSpec>(rockToPebble)));
        StringBuilder<512> three(allocator);
        REQUIRE(scene.PathOf("Out/Three.rxbundle", three));
        const StringView runThree[] = {three.View(), scene.project.View()};
        CHECK(tool.Unreal(runThree) == 0);
        CHECK(tool.Out(
            "asset-import-unreal: reimported /Game/T/Pebble.Pebble /Game/Unreal/Game/T/Rock "));

        // Stone은 이제 옛 ObjectPath를 이어받은 FormerPaths에만 있다(PreviousTags 경로).
        const TextureSpec gravel[] = {Spec("/Game/T/Gravel.Gravel")};
        const RedirectSpec stoneToGravel[] = {{"/Game/T/Stone.Stone", "/Game/T/Gravel.Gravel"}};
        REQUIRE(scene.WriteBundle("Out/Four.rxbundle", Span<const TextureSpec>(gravel),
                                  Span<const RedirectSpec>(stoneToGravel)));
        StringBuilder<512> four(allocator);
        REQUIRE(scene.PathOf("Out/Four.rxbundle", four));
        const StringView runFour[] = {four.View(), scene.project.View()};
        CHECK(tool.Unreal(runFour) == 0);
        CHECK(tool.Out(
            "asset-import-unreal: reimported /Game/T/Gravel.Gravel /Game/Unreal/Game/T/Rock "));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 차원 클래스(TextureCube 6 slice·TextureCubeArray 12·VolumeTexture 2 — 주소·타일 설정), 비ASCII
// objectPath와 옛 경로(escape가 UTF-8을 쪼개지 않는다), 묶음 밖 기존 에셋 참조(ExternalTarget),
// 재임포트의 blob 집합 교체와 감마 경고.
TEST(UnrealExchangeTexture_ImportVariants) {
    SystemAllocator allocator;
    {
        REQUIRE(StartExchangeModule());
        Scene scene(allocator, "ux2b-variants");
        REQUIRE(scene.ready);
        Tool tool(allocator);
        const TextureSpec kinds[] = {
            Spec("/Game/T/Cube.Cube", "TextureCube", "{\"SRGB\": true}", "[]", "sRGB", "[]", 6, 1),
            Spec("/Game/T/CubeArr.CubeArr", "TextureCubeArray", "{\"SRGB\": true}", "[]", "sRGB",
                 "[]", 12, 2),
            Spec("/Game/T/Vol.Vol", "VolumeTexture",
                 R"({"SRGB": false, "AddressMode": "TA_Clamp", "Source2DTileSizeX": 4})", "[]",
                 "Linear", "[]", 2, 3),
        };
        REQUIRE(scene.WriteBundle("Out/Kinds.rxbundle", Span<const TextureSpec>(kinds)));
        StringBuilder<512> kindsBundle(allocator);
        REQUIRE(scene.PathOf("Out/Kinds.rxbundle", kindsBundle));
        const StringView runKinds[] = {kindsBundle.View(), scene.project.View()};
        CHECK(tool.Unreal(runKinds) == 0);
        CHECK(tool.Out("asset-import-unreal: 3 created, 0 reimported, 0 failed, 0 skipped"));
        const StringView cubeInfo[] = {"/Game/Unreal/Game/T/Cube", scene.project.View()};
        CHECK(tool.Info(cubeInfo) == 0 && tool.Out("source.numSlices=6"));
        const StringView cubeArrayInfo[] = {"/Game/Unreal/Game/T/CubeArr", scene.project.View()};
        CHECK(tool.Info(cubeArrayInfo) == 0 && tool.Out("source.numSlices=12"));
        const StringView volumeInfo[] = {"/Game/Unreal/Game/T/Vol", scene.project.View()};
        CHECK(tool.Info(volumeInfo) == 0 && tool.Out("source.numSlices=2") &&
              tool.Out("addressMode=Clamp") && tool.Out("source2DTileSizeX=4"));

        // 비ASCII 이름과 옛 경로.
        const TextureSpec named[] = {Spec("/Game/T/돌.돌")};
        const RedirectSpec oldName[] = {{"/Game/T/옛돌.옛돌", "/Game/T/돌.돌"}};
        REQUIRE(scene.WriteBundle("Out/Named.rxbundle", Span<const TextureSpec>(named),
                                  Span<const RedirectSpec>(oldName)));
        StringBuilder<512> namedBundle(allocator);
        REQUIRE(scene.PathOf("Out/Named.rxbundle", namedBundle));
        const StringView runNamed[] = {namedBundle.View(), scene.project.View()};
        CHECK(tool.Unreal(runNamed) == 0);
        CHECK(tool.Out("asset-import-unreal: 1 created, 0 reimported, 0 failed, 0 skipped"));

        // 묶음 밖 기존 에셋 참조: Rock을 먼저 만들고 Arr만 든 묶음이 그것을 잇는다.
        const TextureSpec rockOnly[] = {Spec("/Game/T/Rock.Rock")};
        REQUIRE(scene.WriteBundle("Out/Rock.rxbundle", Span<const TextureSpec>(rockOnly)));
        StringBuilder<512> rockBundle(allocator);
        REQUIRE(scene.PathOf("Out/Rock.rxbundle", rockBundle));
        const StringView runRock[] = {rockBundle.View(), scene.project.View()};
        CHECK(tool.Unreal(runRock) == 0);
        const TextureSpec arrayOnly[] = {
            Spec("/Game/T/Arr.Arr", "Texture2DArray",
                 R"({"SourceTextures": [{"objectPath": "/Game/T/Rock.Rock"}], "SRGB": true})",
                 R"([{"objectPath": "/Game/T/Rock.Rock", "class": "Texture2D", "kind": "hard"}])")};
        REQUIRE(scene.WriteBundle("Out/Arr.rxbundle", Span<const TextureSpec>(arrayOnly)));
        StringBuilder<512> arrayBundle(allocator);
        REQUIRE(scene.PathOf("Out/Arr.rxbundle", arrayBundle));
        const StringView runArray[] = {arrayBundle.View(), scene.project.View()};
        CHECK(tool.Unreal(runArray) == 0 && !tool.Error("MissingUnrealDependency"));
        const StringView arrayInfo[] = {"/Game/Unreal/Game/T/Arr", scene.project.View()};
        CHECK(tool.Info(arrayInfo) == 0 && tool.Out("/Game/Unreal/Game/T/Rock"));

        // 재임포트: blob 집합이 바뀐다(옛 blob 역할은 버린다), 감마 불일치는 경고다.
        const TextureSpec changed[] = {
            Spec("/Game/T/Rock.Rock", "Texture2D", "{\"SRGB\": true}", "[]", "Linear", "[]", 1, 9)};
        REQUIRE(scene.WriteBundle("Out/Changed.rxbundle", Span<const TextureSpec>(changed)));
        StringBuilder<512> changedBundle(allocator);
        REQUIRE(scene.PathOf("Out/Changed.rxbundle", changedBundle));
        const StringView runChanged[] = {changedBundle.View(), scene.project.View()};
        CHECK(tool.Unreal(runChanged) == 0);
        CHECK(tool.Out("asset-import-unreal: 0 created, 1 reimported, 0 failed, 0 skipped") &&
              tool.Error("GammaMismatch"));
        Array<uint8> pixels(allocator);
        REQUIRE(PixelsOf(changed[0], pixels));
        char newHex[Sha256Digest::HexBytes + 1];
        Sha256::Hash(pixels.View()).ToHex(newHex);
        StringBuilder<128> newRole(allocator);
        StringBuilder<128> oldRole(allocator);
        REQUIRE(newRole.TryAppendFormat("import.source.blob:{}=",
                                        StringView(newHex, Sha256Digest::HexBytes)) &&
                oldRole.TryAppendFormat("import.source.blob:{}=",
                                        StringView(scene.hex, Sha256Digest::HexBytes)));
        const StringView importInfo[] = {"/Game/Unreal/Game/T/Rock", "--import",
                                         scene.project.View()};
        CHECK(tool.Info(importInfo) == 0 && tool.Out(newRole.View()) && !tool.Out(oldRole.View()));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// A2f 임포터 거부와 경고(asset-import로 원본을 직접 준다 — 10.6절 6번 순서, 4.6절 4번).
TEST(UnrealExchangeTexture_ImporterRejects) {
    SystemAllocator allocator;
    {
        REQUIRE(StartExchangeModule());
        Scene scene(allocator, "ux2b-rejects");
        REQUIRE(scene.ready);
        StringBuilder<2048> rock(allocator);
        REQUIRE(scene.AssetText(Spec("/Game/T/Rock.Rock"), rock));
        REQUIRE(scene.WriteText("Src/rock.json", rock.View()));
        REQUIRE(scene.WriteBytes("Src/blob.bin", Span<const uint8>(PixelBytes)));
        const uint8 other[] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
        REQUIRE(scene.WriteBytes("Src/other.bin", Span<const uint8>(other)));
        const Sha256Digest otherDigest = Sha256::Hash(Span<const uint8>(other));
        char otherHex[Sha256Digest::HexBytes + 1];
        otherDigest.ToHex(otherHex);

        StringBuilder<1024> asset(allocator);
        StringBuilder<1024> blob(allocator);
        StringBuilder<1024> wrongBlob(allocator);
        StringBuilder<1024> extraBlob(allocator);
        REQUIRE(asset.TryAppendFormat("--source.asset={}/Src/rock.json", scene.root.Root()) &&
                blob.TryAppendFormat("--source.blob:{}={}/Src/blob.bin",
                                     StringView(scene.hex, Sha256Digest::HexBytes),
                                     scene.root.Root()) &&
                wrongBlob.TryAppendFormat("--source.blob:{}={}/Src/other.bin",
                                          StringView(scene.hex, Sha256Digest::HexBytes),
                                          scene.root.Root()) &&
                extraBlob.TryAppendFormat("--source.blob:{}={}/Src/other.bin",
                                          StringView(otherHex, Sha256Digest::HexBytes),
                                          scene.root.Root()));
        constexpr StringView Importer2D = "--importer=unreal_texture_2d";
        constexpr StringView RockPath = "--objectPath=/Game/T/Rock.Rock";
        Tool tool(allocator);

        // 정상(경고 없음).
        const StringView good[] = {"/Game/X/Good", asset.View(), blob.View(),
                                   Importer2D,     RockPath,     scene.project.View()};
        CHECK(tool.Import(good) == 0 && !tool.Error("warning"));

        struct Case {
            StringView path;
            StringView importer;
            StringView objectPath;
            StringView blob;
            StringView extra;
            StringView code;
        };
        const Case cases[] = {
            {"/Game/X/Cube",
             "--importer=unreal_texture_cube",
             RockPath,
             blob.View(),
             {},
             "InvalidSource"},
            {"/Game/X/Path",
             Importer2D,
             "--objectPath=/Game/T/Other.Other",
             blob.View(),
             {},
             "InvalidImportOption"},
            {"/Game/X/NoBlob", Importer2D, RockPath, {}, {}, "MissingSource"},
            {"/Game/X/Extra", Importer2D, RockPath, blob.View(), extraBlob.View(),
             "InvalidImportOption"},
            {"/Game/X/Changed", Importer2D, RockPath, wrongBlob.View(), {}, "CorruptBundle"},
        };
        for (const Case& item : cases) {
            Array<StringView> args(allocator);
            REQUIRE(args.TryPushBack(item.path) && args.TryPushBack(asset.View()) &&
                    args.TryPushBack(item.importer) && args.TryPushBack(item.objectPath) &&
                    args.TryPushBack(scene.project.View()));
            if (!item.blob.IsEmpty()) {
                REQUIRE(args.TryPushBack(item.blob));
            }

            if (!item.extra.IsEmpty()) {
                REQUIRE(args.TryPushBack(item.extra));
            }

            CHECK(tool.Import(args.View()) == 1);
            CHECK(tool.Error(item.code));
        }

        // 문서 손상·플러그인 error 진단·모르는 enum 값·감마 불일치·Rocks 필드 없는 UE 설정.
        struct DocumentCase {
            StringView path;
            StringView text;
            int32 exit;
            StringView expected;
        };
        StringBuilder<2048> exporterError(allocator);
        REQUIRE(scene.AssetText(
            Spec("/Game/T/Rock.Rock", "Texture2D", "{\"SRGB\": true}", "[]", "sRGB",
                 R"([{"code": "NoSourceData", "severity": "error", "message": "x"}])"),
            exporterError));
        StringBuilder<2048> badEnum(allocator);
        REQUIRE(scene.AssetText(
            Spec("/Game/T/Rock.Rock", "Texture2D", R"({"LODGroup": "TEXTUREGROUP_Nope"})"),
            badEnum));
        StringBuilder<2048> gamma(allocator);
        REQUIRE(scene.AssetText(
            Spec("/Game/T/Rock.Rock", "Texture2D", R"({"SRGB": true})", "[]", "Linear"), gamma));
        StringBuilder<2048> userData(allocator);
        REQUIRE(scene.AssetText(Spec("/Game/T/Rock.Rock", "Texture2D",
                                     R"({"SRGB": true, "AssetUserData": [{"subobject": "U"}]})"),
                                userData));
        const DocumentCase documents[] = {
            {"/Game/X/Syntax", "{", 1, "CorruptBundle"},
            {"/Game/X/Exporter", exporterError.View(), 1, "InvalidSource"},
            {"/Game/X/Enum", badEnum.View(), 1, "InvalidSource"},
            {"/Game/X/Gamma", gamma.View(), 0, "GammaMismatch"},
            {"/Game/X/UserData", userData.View(), 0, "UnsupportedSetting"},
        };
        for (const DocumentCase& item : documents) {
            REQUIRE(scene.WriteText("Src/case.json", item.text));
            StringBuilder<1024> source(allocator);
            REQUIRE(source.TryAppendFormat("--source.asset={}/Src/case.json", scene.root.Root()));
            const StringView args[] = {item.path,  source.View(), blob.View(),
                                       Importer2D, RockPath,      scene.project.View()};
            CHECK(tool.Import(args) == item.exit);
            CHECK(tool.Error(item.expected));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_texture_import_tests_file
