#include "../../../Runtime/AssetRuntime/Tests/AssetTestSupport.h"
#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "../../AssetDatabase/Tests/MemoryFileSystem.h"
#include "TextureImportTestSupport.h"

#include <AssetCooker/AssetCooker.module.generated.h>
#include <AssetCooker/CookProject.h>
#include <AssetDatabase/AssetService.h>
#include <AssetRuntime/CookedManifest.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/PackageLoad.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/StringBuilder.h>
#include <Object/ConstructionContext.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/ObjectRegistryOptions.h>
#include <ReflectionText/TextAssignments.h>
#include <TestSupport/Test.h>
#include <Texture/Texture.generated.h>
#include <TextureTools/TextureCook.h>
#include <TextureTools/TextureImport.h>

// 텍스처 시나리오다(Docs/Design/AssetSystem 12.15절 A5k — 14절 1·2·10·11·13·21·24번의 텍스처 경로):
// 제품 부품(AssetService·TextureTools 임포터·타입 hook·AssetCooker
// CookProject·TextureCookProcessor· 패키지 로드)을 이어 PNG 임포트 → 다시 열기(같은 id·픽셀) → 묶음
// 배열 → 설정 편집(Masks·LODBias — SRGB 정규화) → 원본 삭제 → 쿠킹 → 쿠킹 패키지 로드
// 대조(형식·mip·R·mip 0 상자 평균·EditorOnly 부재) → 다시 쿠킹(전부 재사용·같은 매니페스트 byte).
using namespace rocks;
namespace texture_scenario_tests_file {

using rocks::asset_database_test::ManualAssetClock;
using rocks::asset_database_test::MemoryFileSystem;
using rocks::asset_database_test::TempProject;
using texture_import_test::MakePng;
using texture_import_test::PngColor;

namespace {

constexpr int64 ScenarioStartTicks = 639000000000000000ll;
constexpr uint32 Side = 16;
constexpr uint8 Alpha = 200;
constexpr StringView CookedRoot = "Saved/Cooked/Windows";
constexpr StringView ManifestPath = "Saved/Cooked/Windows/CookedManifest.rman";

// texel (x, y)의 채널 c 값이다(seed마다 다르다 — 알파는 0이 아닌 고정값이라 PNG 채움을 피한다).
uint8 Texel(uint32 x, uint32 y, uint32 c, uint32 seed) noexcept {
    if (c == 3) {
        return Alpha;
    }

    return static_cast<uint8>((x * 13 + y * 7 + c * 50 + seed * 31) & 0xFF);
}

Array<uint8> Png(Allocator& allocator, uint32 seed) noexcept {
    Array<uint32> texels(allocator);
    CHECK(texels.TryResize(usize{Side} * Side * 4));
    for (uint32 y = 0; y < Side; ++y) {
        for (uint32 x = 0; x < Side; ++x) {
            for (uint32 c = 0; c < 4; ++c) {
                texels[(usize{y} * Side + x) * 4 + c] = Texel(x, y, c, seed);
            }
        }
    }

    return MakePng(allocator, Side, Side, PngColor::Rgba, 8, texels.View());
}

// 선형 8비트 2×2 상자 평균의 UE 식이다(12.5절 — ((A + B) + C) + D) × 0.25, (uint8)(0.5 + v × 255)).
uint8 BoxAverage(uint8 a, uint8 b, uint8 c, uint8 d) noexcept {
    const float value = ((((a / 255.0f) + (b / 255.0f)) + (c / 255.0f)) + (d / 255.0f)) * 0.25f;
    return static_cast<uint8>(0.5f + value * 255.0f);
}

// 대입 목록이다.
struct Assignments {
    explicit Assignments(Allocator& allocator) noexcept : list(allocator) {
    }

    Assignments& Add(StringView text) noexcept {
        reflection_text::TextAssignment assignment;
        CHECK(reflection_text::MakeTextAssignment(text, static_cast<uint32>(list.Size()),
                                                  assignment));
        CHECK(list.TryPushBack(assignment));
        return *this;
    }

    Array<reflection_text::TextAssignment> list;
};

// 시나리오 장면이다: 타입(AssetCooker 집계 — Texture·AssetRuntime 포함), 프로젝트(임시 루트 +
// 메모리 파일 시스템), 텍스처 임포터·hook, 서비스, 쿠킹 처리기 표, 쿠킹 로드 Registry.
struct ScenarioScene {
    ScenarioScene() noexcept
        : types(allocator), registration(types, allocator), names(allocator),
          root("texture-scenario"), layout(allocator), files(allocator, Initialized(layout, root)),
          hooks(allocator, types), hookTokens(allocator), importers(allocator, types),
          importerTokens(allocator), clock(ScenarioStartTicks),
          processors(allocator, types, fallback), profile(allocator), settings(allocator) {
        ready =
            static_cast<bool>(generated::AssetCooker::RegisterModuleTypes(registration)) &&
            static_cast<bool>(names.Initialize()) && root.made && files.Ready() &&
            files.AddDirectory("Content") && files.AddDirectory("Config") &&
            files.AddDirectory("SourceArt") &&
            static_cast<bool>(RegisterTextureTypeHooks(nullptr, hooks, hookTokens)) &&
            static_cast<bool>(
                RegisterTextureImporters(&textureImporters, importers, importerTokens)) &&
            static_cast<bool>(RegisterTextureCookProcessors(&textureCook, processors)) &&
            static_cast<bool>(lock.Acquire(layout, 0)) &&
            static_cast<bool>(cookLock.Acquire(allocator, layout.SavedDirectory(), "Windows", 0)) &&
            static_cast<bool>(profile.id.TryAssign("Windows"));
        String everything(allocator);
        ready = ready && everything.TryAssign("/Game/") &&
                settings.alwaysCook.TryPushBack(Move(everything));
        RegistryOptions options;
        options.names = &names;
        options.policy.packageNativeType = NativeTypeKey<Package>();
        options.policy.rejectWorldOnly = true;
        auto made = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                       ContextId{0x54584353ull, 2}, options);
        if (made) {
            cooked = Move(made.Value());
        }

        ready = ready && cooked.Get() != nullptr;
        if (ready) {
            auto created = UniquePtr<AssetService>::TryMake(allocator, allocator, layout, Config());
            if (created) {
                service = Move(created.Value());
            }

            ready = service.Get() != nullptr && static_cast<bool>(service->Initialize());
        }
    }

    ~ScenarioScene() noexcept {
        loaded.Clear();
        service.Reset();
        cooked.Reset();
        hookTokens.Clear();
        importerTokens.Clear();
        CHECK(registration.Unregister());
    }

    static StringView Initialized(ProjectLayout& layout, const TempProject& project) noexcept {
        return layout.Initialize(project.Root()) ? layout.Root() : project.Root();
    }

    AssetServiceConfig Config() noexcept {
        AssetServiceConfig config;
        config.packages.types = &types;
        config.packages.files = &files;
        config.packages.hooks = &hooks;
        config.packages.read.engineVersion = asset_test::TestEngineVersion;
        config.packages.registryContext = ContextId{0x54584353ull, 1};
        config.packages.registryOptions.names = &names;
        config.clock = &clock;
        config.importers = &importers;
        return config;
    }

    String Absolute(StringView relative) noexcept {
        String out(allocator);
        CHECK(out.TryAppend(layout.Root()) && out.TryAppend("/") && out.TryAppend(relative));
        return out;
    }

    Result<AssetId> Import(StringView path, StringView source, uint32 seed) noexcept {
        const Array<uint8> png = Png(allocator, seed);
        files.Remove(source);
        if (!files.AddFile(source, png.View())) {
            return Result<AssetId>::Failure(ErrorCode::OutOfMemory);
        }

        const String absolute = Absolute(source);
        const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, absolute.View()}};
        reflection_text::TextDiagnostics diagnostics(allocator);
        AssetImportResult result(allocator);
        if (auto imported = service->Import(lock, path, inputs, {}, {}, diagnostics, result);
            !imported) {
            return Result<AssetId>::Failure(imported.Error());
        }

        return Result<AssetId>::Success(result.id);
    }

    CookProjectRequest Request() noexcept {
        CookProjectRequest request;
        request.files = &files;
        request.layout = &layout;
        request.lock = &lock;
        request.cookLock = &cookLock;
        request.index = &service->Index();
        request.packages = &service->Packages();
        request.types = &types;
        request.processors = &processors;
        request.profile = &profile;
        request.profileSettings =
            Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("W"), 1));
        request.settings = &settings;
        request.cookSettings =
            Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("S"), 1));
        request.cookedBy = asset_test::TestEngineVersion;
        return request;
    }

    // 쿠킹 패키지 하나를 쿠킹 Registry에 로드하고 대표 텍스처를 준다(null이면 실패).
    const Texture* Load(AssetId asset, const Sha256Digest& sha256) noexcept {
        StringBuilder<256> path(allocator);
        if (!path.TryAppend(CookedRoot) || !path.TryAppend("/") ||
            !AppendCookedPackagePath(path, asset, sha256)) {
            return nullptr;
        }

        Array<uint8> bytes(allocator);
        if (!bytes.TryAppend(files.Bytes(path.View()))) {
            return nullptr;
        }

        PackageLoadOptions options;
        options.read.expectedKind = PackageKind::Cooked;
        auto made = UniquePtr<PackageLoadState>::TryMake(allocator, allocator, *cooked, types,
                                                         cooked->OwnerAccess(), options);
        if (!made) {
            return nullptr;
        }

        UniquePtr<PackageLoadState> state = Move(made.Value());
        if (!state->Prepare(Move(bytes), nullptr)) {
            return nullptr;
        }

        ConstructionContext transaction(*cooked, cooked->OwnerAccess());
        PackageLoadState* pointers[1] = {state.Get()};
        ObjectHandle handles[1] = {};
        RootRef<Object> roots[1];
        if (!InstantiatePackages(transaction, Span<PackageLoadState* const>(pointers, 1), nullptr,
                                 Span<ObjectHandle>(handles), nullptr) ||
            !transaction.Commit(Span<ObjectHandle>(handles), Span<RootRef<Object>>(roots))) {
            return nullptr;
        }

        if (!loaded.TryPushBack(Move(roots[0]))) {
            return nullptr;
        }

        auto child = cooked->FindChild(cooked->OwnerAccess(), handles[0], "");
        if (!child) {
            return nullptr;
        }

        lastPackage = handles[0];
        auto borrowed = cooked->Resolve<Texture>(cooked->OwnerAccess(), child.Value());
        if (!borrowed) {
            return nullptr;
        }

        auto value = borrowed.Value().Get();
        return value ? value.Value() : nullptr;
    }

    // 마지막으로 로드한 패키지의 bulk byte다.
    Span<const uint8> Bulk(const BulkDataRef& ref) noexcept {
        auto borrowed = cooked->Resolve<Package>(cooked->OwnerAccess(), lastPackage);
        if (!borrowed) {
            return {};
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return {};
        }

        auto bytes = value.Value()->BulkBytes(ref);
        return bytes ? bytes.Value() : Span<const uint8>{};
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration registration;
    NamePool names;
    TempProject root;
    ProjectLayout layout;
    MemoryFileSystem files;
    AssetTypeHookTable hooks;
    Array<AssetTypeHookRegistration> hookTokens;
    AssetImporterTable importers;
    Array<AssetImporterRegistration> importerTokens;
    TextureImporterSet textureImporters;
    ManualAssetClock clock;
    AssetDatabaseLock lock;
    CookRootLock cookLock;
    DefaultCookProcessor fallback;
    TextureCookProcessor textureCook;
    CookProcessorTable processors;
    TargetProfile profile;
    CookSettings settings;
    UniquePtr<ObjectRegistry> cooked;
    UniquePtr<AssetService> service;
    Array<RootRef<Object>> loaded{allocator};
    ObjectHandle lastPackage{};
    bool ready = false;
};

const CookedManifestEntry* EntryOf(const CookedManifest& manifest, AssetId asset) noexcept {
    for (const CookedManifestEntry& entry : manifest.entries) {
        if (entry.asset == asset) {
            return &entry;
        }
    }

    return nullptr;
}

} // namespace

TEST(TextureScenario_ImportEditCookLoad) {
    ScenarioScene scene;
    REQUIRE(scene.ready);
    reflection_text::TextDiagnostics diagnostics(scene.allocator);

    // 1. 임포트와 다시 열기(같은 id·원본 픽셀).
    auto wall = scene.Import("/Game/T/Wall", "SourceArt/Wall.png", 1);
    auto wall2 = scene.Import("/Game/T/Wall2", "SourceArt/Wall2.png", 2);
    REQUIRE(wall && wall2);
    REQUIRE(scene.service->Packages().Close(wall.Value()));
    auto reopened = scene.service->Packages().Open(wall.Value());
    REQUIRE(reopened);
    {
        auto child = scene.service->Packages().Registry().FindChild(
            scene.service->Packages().Access(), reopened.Value(), "");
        REQUIRE(child);
        auto borrowed = scene.service->Packages().Registry().Resolve<Texture>(
            scene.service->Packages().Access(), child.Value());
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        const Texture& texture = *value.Value();
        CHECK(texture.source.format == TextureSourceFormat::RGBA8 && texture.source.sizeX == Side &&
              texture.source.sizeY == Side && texture.srgb == 1);
        auto bytes = scene.service->Packages().Find(wall.Value())->store->Read(texture.source.bulk);
        REQUIRE(bytes && bytes.Value().Size() == usize{Side} * Side * 4);
        bool same = true;
        for (uint32 y = 0; y < Side; ++y) {
            for (uint32 x = 0; x < Side; ++x) {
                for (uint32 c = 0; c < 4; ++c) {
                    same =
                        same && bytes.Value()[(usize{y} * Side + x) * 4 + c] == Texel(x, y, c, 1);
                }
            }
        }

        CHECK(same);
    }

    REQUIRE(scene.service->Packages().Close(wall.Value()));

    // 2. 묶음 배열(원소 둘 — 처음 만들기 설정).
    Assignments bundle(scene.allocator);
    bundle.Add("sourceTextures=/Game/T/Wall,/Game/T/Wall2");
    auto array = scene.service->New(scene.lock, "Texture2DArray", "/Game/T/Arr", bundle.list.View(),
                                    diagnostics);
    REQUIRE(array);

    // 3. 설정 편집(Masks — 정규화가 SRGB를 끈다, LODBias 1).
    Assignments edits(scene.allocator);
    edits.Add("compressionSettings=Masks").Add("lodBias=1");
    REQUIRE(scene.service->Set(scene.lock, "/Game/T/Wall", edits.list.View(), false, diagnostics,
                               nullptr, nullptr));

    // 4. 원본 삭제 — 에디터 패키지만으로 쿠킹한다.
    scene.files.Remove("SourceArt/Wall.png");

    // 5. 쿠킹.
    CookProjectResult result(scene.allocator);
    REQUIRE(CookProject(scene.allocator, scene.Request(), result));
    CHECK(result.published && !result.HasErrors() && result.cooked == 3);
    auto manifest = ReadCookedManifest(scene.allocator, scene.files.Bytes(ManifestPath));
    REQUIRE(manifest && manifest.Value().entries.Size() == 3);
    Array<uint8> firstManifest(scene.allocator);
    REQUIRE(firstManifest.TryAppend(scene.files.Bytes(ManifestPath)));

    // 6. 로드 대조.
    {
        const CookedManifestEntry* entry = EntryOf(manifest.Value(), wall.Value());
        REQUIRE(entry != nullptr);
        const Texture* texture = scene.Load(entry->asset, entry->packageSha256);
        REQUIRE(texture != nullptr);
        const TexturePlatformData& data = texture->platformData;
        CHECK(data.pixelFormat == static_cast<uint8>(PixelFormat::RGBA8Unorm) && !data.srgb &&
              data.numMips == 4 && data.sizeX == Side / 2 && data.residentTailMips == 4);
        CHECK(texture->source.bulk.IsEmpty() && texture->source.sizeX == 0);
        const Span<const uint8> tail = scene.Bulk(data.tailBulk);
        REQUIRE(tail.Size() == 8 * 8 * 4 + 4 * 4 * 4 + 2 * 2 * 4 + 4);
        // mip 0 = 원본 mip 1(2×2 상자 평균 — 8비트 선형 디코딩 식은 빌드 때 UE와 대조하므로 1
        // 차이를 허용한다).
        bool close = true;
        for (uint32 j = 0; j < Side / 2; ++j) {
            for (uint32 i = 0; i < Side / 2; ++i) {
                for (uint32 c = 0; c < 4; ++c) {
                    const uint8 expected = BoxAverage(
                        Texel(2 * i, 2 * j, c, 1), Texel(2 * i + 1, 2 * j, c, 1),
                        Texel(2 * i, 2 * j + 1, c, 1), Texel(2 * i + 1, 2 * j + 1, c, 1));
                    const int32 actual = tail[(usize{j} * (Side / 2) + i) * 4 + c];
                    const int32 difference = actual - static_cast<int32>(expected);
                    close = close && difference >= -1 && difference <= 1;
                }
            }
        }

        CHECK(close);
    }

    {
        const CookedManifestEntry* entry = EntryOf(manifest.Value(), wall2.Value());
        REQUIRE(entry != nullptr);
        const Texture* texture = scene.Load(entry->asset, entry->packageSha256);
        REQUIRE(texture != nullptr);
        CHECK(texture->platformData.pixelFormat ==
                  static_cast<uint8>(PixelFormat::RGBA8UnormSrgb) &&
              texture->platformData.srgb && texture->platformData.numMips == 5 &&
              texture->platformData.residentTailMips == 5);
    }

    {
        const CookedManifestEntry* entry = EntryOf(manifest.Value(), array.Value());
        REQUIRE(entry != nullptr);
        const Texture* texture = scene.Load(entry->asset, entry->packageSha256);
        REQUIRE(texture != nullptr);
        const TexturePlatformData& data = texture->platformData;
        CHECK(data.dimension == TextureDimension::Texture2DArray &&
              data.pixelFormat == static_cast<uint8>(PixelFormat::RGBA8UnormSrgb) &&
              data.numSlices == 2 && data.numMips == 1 && data.residentTailMips == 1 &&
              data.tailBulk.rawBytes == usize{Side} * Side * 4 * 2);
        CHECK(static_cast<const Texture2DArray*>(texture)->sourceTextures.Size() == 0 &&
              texture->source.bulk.IsEmpty());
    }

    // 7. 다시 쿠킹 — 전부 재사용, 같은 매니페스트 byte.
    CookProjectResult again(scene.allocator);
    REQUIRE(CookProject(scene.allocator, scene.Request(), again));
    CHECK(again.published && again.cooked == 0 && again.reused == 3);
    CHECK(asset_test::SameBytes(scene.files.Bytes(ManifestPath), firstManifest.View()));
}

} // namespace texture_scenario_tests_file
