#include "../../../../../../Source/Developer/AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "ExchangeTestSupport.h"

#include <AssetCommandlets/AssetEditCommandlet.h>
#include <Core/Commandlet/Commandlet.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UnrealExchange/ExchangeCommand.h>
#include <UnrealExchange/ExchangeServiceKind.h>
#include <UnrealExchange/UnrealExchangeImporterSet.h>
#include <stdio.h>

// 교환 정적 메시 임포트 시험이다(Docs/Design/UnrealExchange 5.5절 12번 — UX3b2): 저장소 golden
// 묶음의 정적 메시 둘을 디스크 프로젝트에 asset-import-unreal로 들이고(원본 LOD·슬롯·충돌·태그·
// 기록 옵션), 다시 실행하면 재임포트(id 유지), 일반 asset-reimport, --mesh-normals=rocks, 그리고
// asset-import로 A2f 임포터 unreal_static_mesh의 거부(10.6절 6번 순서·변환 실패)와 경고.
using namespace rocks;
using namespace rocks::asset_database_test;
using namespace rocks::exchange_test;
namespace exchange_static_mesh_import_tests_file {

namespace {

constexpr StringView CubePath = "/Engine/BasicShapes/Cube.Cube";
constexpr StringView DegeneratePath = "/Game/RxGolden/SM_Degenerate.SM_Degenerate";
constexpr StringView CubeAsset = "/Game/Unreal/Engine/BasicShapes/Cube";
constexpr StringView DegenerateAsset = "/Game/Unreal/Game/RxGolden/SM_Degenerate";

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

// 교환 임포터(텍스처·정적 메시)를 건 명령 문맥이다(에셋 타입 모듈은 기본 목록).
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

    void Print(const char* label) const noexcept {
        printf("  [%s] out:\n%.*s  error:\n%.*s\n", label,
               static_cast<int>(captured.out.View().ByteCount()), captured.out.View().Data(),
               static_cast<int>(captured.error.View().ByteCount()), captured.error.View().Data());
    }

    UnrealExchangeImporterSet importers;
    ExchangeServiceKind kind;
    commandlet_host::AssetCommandContext assets;
    UnrealExchangeCommandContext unreal;
    Captured captured;
};

// 디스크 프로젝트다.
struct Scene {
    explicit Scene(Allocator& allocator, const char* tag) noexcept
        : allocator(allocator), root(tag), project(allocator), bundle(allocator) {
        ready = root.made && project.TryAppendFormat("--project={}", root.Root()) &&
                WriteText("Scene.rocksproject", "{}\n") &&
                FixtureSourcePath("Golden.rxbundle", bundle);
    }

    bool WriteText(StringView relative, StringView text) noexcept {
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
               static_cast<bool>(file.WriteAll(text.Data(), text.ByteCount()).status) &&
               static_cast<bool>(file.Close());
    }

    Allocator& allocator;
    TempProject root;
    StringBuilder<512> project;
    StringBuilder<512> bundle;
    bool ready = false;
};

bool StartExchangeModule() noexcept {
    return test::ProcessModules().Load("UnrealExchange").IsOk();
}

bool Keep(const Tool& tool, String& copy) noexcept {
    return static_cast<bool>(copy.TryAssign(tool.captured.out.View()));
}

// golden 정적 메시 하나의 asset-import 원본 인자(`--source.asset=…`, blob마다 `--source.blob:…`)다.
struct SourceArgs {
    explicit SourceArgs(Allocator& allocator) noexcept : texts(allocator), bundle(allocator) {
    }

    Array<String> texts;
    ExchangeBundle bundle;
};

bool MakeSourceArgs(Allocator& allocator, StringView directory, StringView objectPath,
                    SourceArgs& out) noexcept {
    ExchangeDiagnostics diagnostics(allocator);
    if (!ReadExchangeBundle(platform::PhysicalFileSystem(), directory, {}, out.bundle,
                            diagnostics)) {
        return false;
    }

    for (const ExchangeAsset& asset : out.bundle.Assets()) {
        if (asset.objectPath != objectPath) {
            continue;
        }

        String document(allocator);
        StringBuilder<1024> text(allocator);
        if (!text.TryAppendFormat("--source.asset={}/{}", directory, asset.file) ||
            !document.TryAssign(text.View()) || !out.texts.TryPushBack(Move(document))) {
            return false;
        }

        for (usize index = 0; index < asset.blobs.Size(); ++index) {
            // 원본 역할은 digest마다 하나다(같은 byte를 다른 배치로 가리키는 참조가 있다).
            const ExchangeBlobRef& blob = asset.blobs[index];
            bool repeated = false;
            for (usize earlier = 0; earlier < index; ++earlier) {
                repeated = repeated || asset.blobs[earlier].digest == blob.digest;
            }

            if (repeated) {
                continue;
            }

            char hex[Sha256Digest::HexBytes + 1];
            blob.digest.ToHex(hex);
            StringBuilder<1024> argument(allocator);
            String copy(allocator);
            if (!argument.TryAppendFormat("--source.blob:{}=",
                                          StringView(hex, Sha256Digest::HexBytes)) ||
                !AppendExchangeBlobPath(directory, blob.digest, argument) ||
                !copy.TryAssign(argument.View()) || !out.texts.TryPushBack(Move(copy))) {
                return false;
            }
        }

        return true;
    }

    return false;
}

} // namespace

// asset-import-unreal: golden 정적 메시 둘의 새 임포트(원본 LOD·슬롯·충돌·태그·기록 옵션), 다시
// 실행하면 재임포트, 일반 asset-reimport는 같은 결과, --mesh-normals=rocks는 기록 옵션을 바꾼다.
TEST(UnrealExchangeStaticMesh_ImportCommand) {
    SystemAllocator allocator;
    {
        REQUIRE(StartExchangeModule());
        Scene scene(allocator, "ux3b-command");
        REQUIRE(scene.ready);
        Tool tool(allocator);
        StringBuilder<256> onlyCube(allocator);
        StringBuilder<256> onlyDegenerate(allocator);
        REQUIRE(onlyCube.TryAppendFormat("--only={}", CubePath) &&
                onlyDegenerate.TryAppendFormat("--only={}", DegeneratePath));
        const StringView run[] = {scene.bundle.View(), onlyCube.View(), onlyDegenerate.View(),
                                  scene.project.View()};
        CHECK(tool.Unreal(run) == 0);
        CHECK(tool.Out("asset-import-unreal: 2 created, 0 reimported, 0 failed, 0 skipped"));
        // Cube(옛 에셋)의 bUseBackwardsCompatibleF16TruncUVs는 Rocks 필드가 없다.
        CHECK(tool.Error("UnsupportedSetting"));

        const StringView cubeInfo[] = {CubeAsset, scene.project.View()};
        CHECK(tool.Info(cubeInfo) == 0);
        // 설정·슬롯(묶음 밖 머티리얼은 비움)·충돌, Unreal 모드: UE가 계산한 원본이라 다시 계산하지
        // 않고 lightmap 번호는 preparedBuild다.
        CHECK(tool.Out("lightMapResolution=64") && tool.Out("lightMapCoordinateIndex=1") &&
              tool.Out("materialSlots[0].slotName=WorldGridMaterial") &&
              tool.Out("materialSlots[0].material=none") &&
              tool.Out("simpleCollision.boxes[0].size.x=100") &&
              tool.Out("sourceLods[0].build.recomputeTangents=false") &&
              tool.Out("sourceLods[0].build.dstLightmapIndex=2"));
        const StringView importInfo[] = {CubeAsset, "--import", scene.project.View()};
        CHECK(tool.Info(importInfo) == 0);
        CHECK(tool.Out("import.importer=unreal_static_mesh 1 current") &&
              tool.Out("import.source.asset=") && tool.Out("import.source.blob:"));
        const StringView degenerateInfo[] = {DegenerateAsset, scene.project.View()};
        CHECK(tool.Info(degenerateInfo) == 0);

        // 다시 실행하면 재임포트다(같은 에셋 — 필드가 같다).
        CHECK(tool.Info(cubeInfo) == 0);
        String before(allocator);
        REQUIRE(Keep(tool, before));
        CHECK(tool.Unreal(run) == 0);
        CHECK(tool.Out("asset-import-unreal: 0 created, 2 reimported, 0 failed, 0 skipped"));
        CHECK(tool.Info(cubeInfo) == 0);
        CHECK(StringView(tool.captured.out.View()) == before.View());

        // 일반 asset-reimport는 같은 결과다.
        const StringView plain[] = {CubeAsset, scene.project.View()};
        CHECK(tool.Reimport(plain) == 0);
        CHECK(tool.Info(cubeInfo) == 0);
        CHECK(StringView(tool.captured.out.View()) == before.View());

        // --mesh-normals=rocks: 순수 원본과 UE 빌드 설정 그대로다(Cube는 recomputeTangents 참).
        const StringView rocks[] = {scene.bundle.View(), onlyCube.View(), "--mesh-normals=rocks",
                                    scene.project.View()};
        CHECK(tool.Unreal(rocks) == 0);
        CHECK(tool.Out("asset-import-unreal: 0 created, 1 reimported, 0 failed, 0 skipped"));
        CHECK(tool.Info(cubeInfo) == 0);
        CHECK(tool.Out("sourceLods[0].build.recomputeTangents=true") &&
              tool.Out("sourceLods[0].build.dstLightmapIndex=2"));
        // 일반 asset-reimport는 기록 옵션(Rocks)을 다시 쓴다.
        CHECK(tool.Reimport(plain) == 0);
        CHECK(tool.Info(cubeInfo) == 0);
        CHECK(tool.Out("sourceLods[0].build.recomputeTangents=true"));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// A2f 임포터 unreal_static_mesh의 거부와 경고(asset-import로 golden 원본을 직접 준다).
TEST(UnrealExchangeStaticMesh_ImporterRejects) {
    SystemAllocator allocator;
    {
        REQUIRE(StartExchangeModule());
        Scene scene(allocator, "ux3b-rejects");
        REQUIRE(scene.ready);
        SourceArgs sources(allocator);
        REQUIRE(MakeSourceArgs(allocator, scene.bundle.View(), CubePath, sources));
        REQUIRE(sources.texts.Size() > 2);
        constexpr StringView Importer = "--importer=unreal_static_mesh";
        StringBuilder<256> objectPath(allocator);
        REQUIRE(objectPath.TryAppendFormat("--objectPath={}", CubePath));
        Tool tool(allocator);

        const auto withSources = [&](StringView path, Array<StringView>& args, usize skip) {
            bool ok = args.TryPushBack(path) && args.TryPushBack(scene.project.View());
            for (usize index = 0; index < sources.texts.Size(); ++index) {
                ok = ok && (index == skip || args.TryPushBack(sources.texts[index].View()));
            }

            return ok;
        };

        // 정상: 경고는 Rocks 필드가 없는 설정 하나다.
        {
            Array<StringView> args(allocator);
            REQUIRE(withSources("/Game/X/Good", args, NumericLimits<usize>::Max()) &&
                    args.TryPushBack(Importer) && args.TryPushBack(objectPath.View()));
            CHECK(tool.Import(args.View()) == 0);
            CHECK(tool.Error("UnsupportedSetting"));
        }

        // 다른 클래스 임포터(텍스처), objectPath 불일치, blob 원본 하나 없음.
        struct Case {
            StringView path;
            StringView importer;
            StringView objectPath;
            usize skip;
            StringView code;
        };
        const Case cases[] = {
            {"/Game/X/Texture", "--importer=unreal_texture_2d", objectPath.View(),
             NumericLimits<usize>::Max(), "InvalidSource"},
            {"/Game/X/Path", Importer, "--objectPath=/Game/T/Other.Other",
             NumericLimits<usize>::Max(), "InvalidImportOption"},
            {"/Game/X/NoBlob", Importer, objectPath.View(), 1, "MissingSource"},
        };
        for (const Case& item : cases) {
            Array<StringView> args(allocator);
            REQUIRE(withSources(item.path, args, item.skip) && args.TryPushBack(item.importer) &&
                    args.TryPushBack(item.objectPath));
            CHECK(tool.Import(args.View()) == 1);
            CHECK(tool.Error(item.code));
            if (!tool.Error(item.code)) {
                tool.Print("case");
            }
        }

        // 변환 실패: SourceModels 수(빈 LOD 설정을 앞에 하나 더함)와 data.lods 수가 다른
        // 문서(blob은 그대로)다 — 정적 메시 변환의 CorruptBundle이다.
        Array<char> original(allocator);
        REQUIRE(
            ReadRepositoryFile(allocator,
                               "/Plugins/Importers/UnrealExchange/Tests/Bundles/Golden.rxbundle/"
                               "assets/Engine/BasicShapes/Cube.json",
                               original));
        const StringView text(original.Data(), original.Size());
        constexpr StringView Marker = "\"SourceModels\": [";
        const usize at = text.Find(Marker, StringComparison::CaseSensitive);
        REQUIRE(at != StringView::NotFound);
        const usize split = at + Marker.ByteCount();
        StringBuilder<> edited(allocator);
        REQUIRE(edited.TryAppend(text.SliceBytes(0, split)) && edited.TryAppend("{}, ") &&
                edited.TryAppend(text.SliceBytes(split, text.ByteCount() - split)));
        REQUIRE(scene.WriteText("Src/cube.json", edited.View()));
        StringBuilder<1024> document(allocator);
        REQUIRE(document.TryAppendFormat("--source.asset={}/Src/cube.json", scene.root.Root()));
        Array<StringView> args(allocator);
        REQUIRE(args.TryPushBack("/Game/X/Lods") && args.TryPushBack(scene.project.View()) &&
                args.TryPushBack(document.View()) && args.TryPushBack(Importer) &&
                args.TryPushBack(objectPath.View()));
        for (usize index = 1; index < sources.texts.Size(); ++index) {
            REQUIRE(args.TryPushBack(sources.texts[index].View()));
        }

        CHECK(tool.Import(args.View()) == 1);
        CHECK(tool.Error("CorruptBundle"));
        if (!tool.Error("CorruptBundle")) {
            tool.Print("lods");
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_static_mesh_import_tests_file
