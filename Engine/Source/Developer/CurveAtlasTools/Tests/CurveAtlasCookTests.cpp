#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "../../AssetDatabase/Tests/EditorPackageSetTestSupport.h"

#include <AssetCooker/CookProcessor.h>
#include <AssetDatabase/AssetDatabaseLock.h>
#include <AssetDatabase/EditorPackageSet.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/FilePackageBulkStore.h>
#include <AssetRuntime/PackageReader.h>
#include <AssetRuntime/PackageWriter.h>
#include <Base/InitializerList.h>
#include <CurveAssets/ColorCurveAsset.generated.h>
#include <CurveAtlas/ColorCurveAtlas.generated.h>
#include <CurveAtlas/CurveAtlasModule.h>
#include <CurveAtlasTools/ColorCurveAtlasBake.h>
#include <CurveAtlasTools/CurveAtlasTypeHooks.h>
#include <TargetProfiles/TargetProfile.h>
#include <TestSupport/Test.h>
#include <TextureTools/TextureCook.h>
#include <TextureTools/TextureImport.h>
#include <string.h>

// 색 커브 atlas 쿠킹 시험이다(Docs/Design/CurveAssets 4.3절 — CA6, AssetSystem 12.14절 A5j2c):
// asset-set 흐름(원본 재구성 → 정규화 → 저장)이 원본을 저장하고 커브를 Build 의존으로 적는다.
// 커브만 바꿔 저장하면 atlas 파일은 낡는다. hook 표 없이 열면 낡은 원본, hook 표로 열면(쿠커와 같은
// EditorPackageSet::Open) 다시 구운 원본(dirty, 파일 그대로)이고, 텍스처 처리기가 파생 타입 atlas를
// 쿠킹하며 결과가 새 커브를 따른다(다시 구운 원본을 저장한 뒤 연 쿠킹과 byte가 같다).
using namespace rocks;

namespace curve_atlas_cook_tests_file {

using namespace rocks::editor_set_test;

namespace {

constexpr uint64 CurvePackageId = 0x7400;
constexpr uint64 AtlasPackageId = 0x7401;
constexpr uint32 AtlasWidth = 4;
constexpr float FirstValue = 0.25f;
constexpr float SecondValue = 0.75f;

struct AtlasCookScene {
    AtlasCookScene() noexcept
        : root("curve-atlas-cook"), project(env.allocator, root.Root()),
          hooks(env.allocator, env.types), tokens(env.allocator), profile(env.allocator) {
        ready = env.Ready() && RegisterCurveAtlasTypes(env.modules).IsOk() && root.made &&
                project.ready && project.files.Ready() && project.Rebuild() &&
                static_cast<bool>(lock.Acquire(project.layout, 0)) &&
                RegisterTextureTypeHooks(nullptr, hooks, tokens).IsOk() &&
                RegisterCurveAtlasTypeHooks(nullptr, hooks, tokens).IsOk() &&
                static_cast<bool>(profile.id.TryAssign("Windows"));
    }

    EditorPackageSetConfig SetConfig(bool withHooks) noexcept {
        EditorPackageSetConfig config = Config(env, project);
        config.hooks = withHooks ? &hooks : nullptr;
        return config;
    }

    // set의 열린 id 패키지의 대표 에셋이다.
    static ObjectHandle Representative(EditorPackageSet& set, uint64 id) noexcept {
        const EditorPackageInfo* info = set.Find(asset_test::FixedId(id));
        if (info == nullptr) {
            return ObjectHandle{};
        }

        auto child = set.Registry().FindChild(set.Access(), info->package, "");
        return child ? child.Value() : ObjectHandle{};
    }

    static bool SetCurveValue(EditorPackageSet& set, float value) noexcept {
        auto borrowed = set.Registry().ResolveForWrite<ColorCurveAsset>(
            set.Access(), Representative(set, CurvePackageId));
        if (!borrowed) {
            return false;
        }

        auto pointer = borrowed.Value().Get();
        if (!pointer) {
            return false;
        }

        ColorCurveAsset& curve = *pointer.Value();
        while (curve.r.KeyCount() > 0) {
            if (!curve.r.TryRemoveKey(0)) {
                return false;
            }
        }

        return static_cast<bool>(curve.r.TryAddKey(0.0f, value));
    }

    // 상수 value 커브 한 행을 AtlasWidth로 구운 byte다.
    Array<uint8> Expected(float value) noexcept {
        ColorCurveAsset curve(env.allocator);
        CHECK(curve.r.TryAddKey(0.0f, value));
        const ColorCurveAsset* curves[] = {&curve};
        ColorCurveAtlasPixels pixels(env.allocator);
        CHECK(BakeColorCurveAtlas(Span<const ColorCurveAsset* const>(curves), AtlasWidth, false,
                                  pixels));
        return Move(pixels.bytes);
    }

    // 열린 atlas 원본 bulk의 raw byte다(패키지 bulk 저장소 — 저장 전 bulk 포함).
    static Span<const uint8> SourceBytes(EditorPackageSet& set) noexcept {
        const EditorPackageInfo* info = set.Find(asset_test::FixedId(AtlasPackageId));
        auto borrowed = set.Registry().Resolve<ColorCurveAtlas>(
            set.Access(), Representative(set, AtlasPackageId));
        if (info == nullptr || info->store == nullptr || !borrowed) {
            return Span<const uint8>();
        }

        auto pointer = borrowed.Value().Get();
        if (!pointer) {
            return Span<const uint8>();
        }

        auto read = info->store->Read(pointer.Value()->source.bulk, nullptr);
        return read ? read.Value() : Span<const uint8>();
    }

    // 열린 atlas를 텍스처 처리기로 쿠킹한다(쿠커 CookRun과 같은 문맥 — 패키지 bulk 저장소).
    bool Cook(EditorPackageSet& set, Array<uint8>& out) noexcept {
        const EditorPackageInfo* info = set.Find(asset_test::FixedId(AtlasPackageId));
        auto entry = project.index.FindById(asset_test::FixedId(AtlasPackageId));
        if (info == nullptr || !entry) {
            return false;
        }

        TextureCookProcessor processor;
        MemoryPackageSink sink(env.allocator);
        AssetDiagnostic failure;
        Array<CookDiagnostic> warnings(env.allocator);
        CookContext context;
        context.allocator = &env.allocator;
        context.registry = &set.Registry();
        context.access = set.Access();
        context.package = info->package;
        context.entry = entry.Value();
        context.profile = &profile;
        context.cookerVersion = 1;
        context.bulkSource = info->store;
        context.sink = &sink;
        context.failure = &failure;
        context.warnings = &warnings;
        if (auto cooked = processor.Cook(context); !cooked) {
            CHECK(cooked.Error() == ErrorCode::None);
            CHECK(failure.code == AssetDiagnosticCode::None);
            return false;
        }

        out.Clear();
        return static_cast<bool>(out.TryAppend(sink.Bytes()));
    }

    asset_test::AssetEnv env;
    asset_database_test::TempProject root;
    Project project;
    AssetTypeHookTable hooks;
    Array<AssetTypeHookRegistration> tokens;
    AssetDatabaseLock lock;
    TargetProfile profile;
    bool ready = false;
};

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.IsEmpty() || memcmp(left.Data(), right.Data(), left.Size()) == 0);
}

} // namespace

TEST(CurveAtlasCook_OpenRebakesStaleSource) {
    AtlasCookScene scene;
    REQUIRE(scene.ready);
    Allocator& allocator = scene.env.allocator;
    const AssetId curveId = asset_test::FixedId(CurvePackageId);
    const AssetId atlasId = asset_test::FixedId(AtlasPackageId);
    {
        // 커브와 atlas를 만들고 asset-set 흐름(원본 재구성 → 정규화 → 저장)으로 저장한다.
        EditorPackageSet set(allocator, scene.SetConfig(true));
        REQUIRE(set.Initialize());
        auto curvePath = ParseLogicalPath(allocator, "/Game/Curve");
        auto atlasPath = ParseLogicalPath(allocator, "/Game/Atlas");
        REQUIRE(curvePath && atlasPath);
        REQUIRE(set.Create(curvePath.Value(), TypeIdOf<ColorCurveAsset>(), curveId));
        REQUIRE(AtlasCookScene::SetCurveValue(set, FirstValue));
        REQUIRE(set.Save(curveId, scene.lock, EditorSaveOptions{}));
        REQUIRE(set.Create(atlasPath.Value(), TypeIdOf<ColorCurveAtlas>(), atlasId));
        const ObjectHandle atlas = AtlasCookScene::Representative(set, AtlasPackageId);
        const ObjectHandle curve = AtlasCookScene::Representative(set, CurvePackageId);
        {
            auto borrowed = set.Registry().ResolveForWrite<ColorCurveAtlas>(set.Access(), atlas);
            REQUIRE(borrowed);
            auto pointer = borrowed.Value().Get();
            REQUIRE(pointer);
            pointer.Value()->width = AtlasWidth;
            REQUIRE(
                set.Registry().AddReference(set.Access(), atlas, pointer.Value()->curves, curve));
        }

        const StringView assigned[] = {"width", "curves"};
        bool rebuilt = false;
        REQUIRE(set.RebuildSource(atlas, atlasId, Span<const StringView>(assigned),
                                  AssetSourceRebuildCause::Edit, ObjectHandle{}, nullptr, nullptr,
                                  &rebuilt));
        CHECK(rebuilt);
        REQUIRE(set.NormalizeSettings(atlas, atlasId, Span<const StringView>(assigned)));
        REQUIRE(set.MarkDirty(atlasId));
        REQUIRE(set.Save(atlasId, scene.lock, EditorSaveOptions{}));
        const Array<uint8> first = scene.Expected(FirstValue);
        CHECK(SameBytes(AtlasCookScene::SourceBytes(set), first.View()));

        // 저장한 atlas는 커브를 Build 의존으로 적는다(커브가 바뀌면 쿠킹 키가 바뀐다).
        PackageReadOptions read;
        read.engineVersion = asset_test::TestEngineVersion;
        read.expectedKind = PackageKind::Editor;
        auto contents =
            ReadPackage(allocator, scene.project.files.Bytes("Atlas.rasset"), read, nullptr);
        REQUIRE(contents);
        bool build = false;
        for (const PackageDependencyEntry& dependency : contents.Value().dependencies) {
            build = build || (dependency.asset == curveId &&
                              dependency.kind == PackageDependencyKind::Build);
        }

        CHECK(build);

        // 커브만 바꿔 저장한다(atlas 파일은 낡는다 — 즉시 전파 없음).
        REQUIRE(AtlasCookScene::SetCurveValue(set, SecondValue));
        REQUIRE(set.MarkDirty(curveId));
        REQUIRE(set.Save(curveId, scene.lock, EditorSaveOptions{}));
    }

    REQUIRE(scene.project.Rebuild());
    const Array<uint8> staleFile =
        asset_test::Copy(allocator, scene.project.files.Bytes("Atlas.rasset"));
    const Array<uint8> first = scene.Expected(FirstValue);
    const Array<uint8> second = scene.Expected(SecondValue);
    Array<uint8> staleCook(allocator);
    {
        // hook 표 없이 열면 낡은 원본 그대로다(열 때 재구성은 hook 표가 있을 때만).
        EditorPackageSet set(allocator, scene.SetConfig(false));
        REQUIRE(set.Initialize());
        REQUIRE(set.Open(atlasId));
        CHECK(!set.Find(atlasId)->dirty);
        CHECK(SameBytes(AtlasCookScene::SourceBytes(set), first.View()));
        REQUIRE(scene.Cook(set, staleCook));
    }

    Array<uint8> freshCook(allocator);
    {
        // 쿠커처럼 hook 표로 열면 새 커브로 다시 굽고 dirty다. 파일은 그대로다(저장하지 않는다).
        EditorPackageSet set(allocator, scene.SetConfig(true));
        REQUIRE(set.Initialize());
        Array<EditorWarning> warnings(allocator);
        REQUIRE(set.Open(atlasId, &warnings));
        CHECK(warnings.IsEmpty());
        CHECK(set.Find(atlasId)->dirty && !set.Find(curveId)->dirty);
        CHECK(SameBytes(AtlasCookScene::SourceBytes(set), second.View()));
        REQUIRE(scene.Cook(set, freshCook));
        CHECK(!SameBytes(freshCook.View(), staleCook.View()));
        CHECK(SameBytes(scene.project.files.Bytes("Atlas.rasset"), staleFile.View()));
        // 다시 구운 원본을 저장한다(다음 열기는 같은 원본이라 깨끗하다).
        REQUIRE(set.Save(atlasId, scene.lock, EditorSaveOptions{}));
    }

    REQUIRE(scene.project.Rebuild());
    {
        EditorPackageSet set(allocator, scene.SetConfig(true));
        REQUIRE(set.Initialize());
        REQUIRE(set.Open(atlasId));
        CHECK(!set.Find(atlasId)->dirty);
        CHECK(SameBytes(AtlasCookScene::SourceBytes(set), second.View()));
        Array<uint8> savedCook(allocator);
        REQUIRE(scene.Cook(set, savedCook));
        CHECK(SameBytes(savedCook.View(), freshCook.View()));
    }
}

} // namespace curve_atlas_cook_tests_file
