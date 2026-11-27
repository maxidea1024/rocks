#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Tests/AssetTestSupport.h>
#include <Base/InitializerList.h>
#include <Core/Encoding/Sha256.h>
#include <CurveAssets/ColorCurveAsset.generated.h>
#include <CurveAtlas/ColorCurveAtlas.generated.h>
#include <CurveAtlas/CurveAtlasModule.h>
#include <CurveAtlasTools/ColorCurveAtlasBake.h>
#include <CurveAtlasTools/CurveAtlasTypeHooks.h>
#include <TestSupport/Test.h>
#include <TextureTools/TextureImport.h>
#include <string.h>

// 색 커브 atlas 타입 hook 시험이다(Docs/Design/CurveAssets 4.2·4.3절 — CA6): 원본 재구성(Edit·New·
// Open이 굽고 같으면 쓰지 않음, 커브가 바뀌면 다시 굽기, Propagate 무시, 원본 칸과 bulk byte),
// 실패(크기 상한·풀리지 않는 커브 — 진단과 원본 불변), Build 의존(커브 패키지들 — null·자기 패키지
// 제외, 중복 합침), 정규화(너비 올림·조정 항등값·크로마 키), 등록, RowOf.
using namespace rocks;

namespace curve_atlas_hook_tests_file {

namespace {

constexpr uint64 AtlasPackageId = 0x7300;
constexpr uint64 CurveAPackageId = 0x7301;
constexpr uint64 CurveBPackageId = 0x7302;

// 원본 재구성 sink 대역이다(더한 bulk를 붙잡고 진단·표시를 남긴다).
class FakeRebuildSink final : public AssetSourceRebuildSink {
public:
    FakeRebuildSink(Allocator& allocator, AssetSourceRebuildCause cause) noexcept
        : added(allocator), cause_(cause) {
    }

    Result<Span<const uint8>> ReadBulk(ObjectHandle, const BulkDataRef&) noexcept override {
        return Result<Span<const uint8>>::Failure(ErrorCode::NotFound);
    }

    Result<BulkDataRef> AddBulk(Array<uint8>&& raw) noexcept override {
        ++adds;
        BulkDataRef ref;
        ref.bulkId = adds;
        ref.rawBytes = raw.Size();
        ref.sha256 = Sha256::Hash(raw.View());
        added = Move(raw);
        return Result<BulkDataRef>::Success(ref);
    }

    bool HasImportRecord() const noexcept override {
        return false;
    }

    AssetSourceRebuildCause Cause() const noexcept override {
        return cause_;
    }

    ObjectHandle ChangedSource() const noexcept override {
        return ObjectHandle{};
    }

    Result<void> Fail(AssetDiagnosticCode code, ErrorCode error, uint64 detail) noexcept override {
        failCode = code;
        failError = error;
        failDetail = detail;
        return Result<void>::Failure(error);
    }

    Result<void> Warn(AssetDiagnosticCode, uint64) noexcept override {
        ++warnings;
        return Result<void>::Success();
    }

    void MarkRebuilt() noexcept override {
        rebuilt = true;
    }

    Array<uint8> added;
    uint32 adds = 0;
    uint32 warnings = 0;
    bool rebuilt = false;
    AssetDiagnosticCode failCode = AssetDiagnosticCode::None;
    ErrorCode failError = ErrorCode::None;
    uint64 failDetail = 0;

private:
    AssetSourceRebuildCause cause_;
};

// atlas 패키지(자기 안에 커브 Inner 하나)와 커브 패키지 둘, Texture·atlas hook을 건 표다.
struct HookScene {
    HookScene() noexcept : hooks(env.allocator, env.types), tokens(env.allocator) {
        // stage는 처음 실패한 준비 단계다(0 = 모두 됨 — 시험이 CHECK로 보여 준다).
        stage = !env.Ready()                                                                     ? 1
                : !RegisterCurveAtlasTypes(env.modules)                                          ? 2
                : !RegisterTextureTypeHooks(nullptr, hooks, tokens)                              ? 3
                : !RegisterCurveAtlasTypeHooks(nullptr, hooks, tokens)                           ? 4
                : (atlas = MakeAsset(AtlasPackageId, TypeIdOf<ColorCurveAtlas>(), 0)).IsNull()   ? 5
                : (curveA = MakeAsset(CurveAPackageId, TypeIdOf<ColorCurveAsset>(), 1)).IsNull() ? 6
                : (curveB = MakeAsset(CurveBPackageId, TypeIdOf<ColorCurveAsset>(), 2)).IsNull() ? 7
                : (inner = asset_test::Make<ColorCurveAsset>(
                       Registry(), TypeIdOf<ColorCurveAsset>(), atlas, "Inner"))
                        .IsNull()
                    ? 8
                : !SetKey(curveA, 0.25f) || !SetKey(curveB, 0.5f) ? 9
                                                                  : 0;
        CHECK(stage == 0);
        ready = stage == 0;
    }

    ObjectRegistry& Registry() noexcept {
        return *env.source;
    }

    ObjectAccessContext Access() noexcept {
        return env.source->OwnerAccess();
    }

    ObjectHandle MakeAsset(uint64 id, TypeId type, usize slot) noexcept {
        auto made = NewPackage(Registry(), Access(), asset_test::FixedId(id), PackageKind::Editor);
        if (!made) {
            return ObjectHandle{};
        }

        packages[slot] = Move(made.Value());
        return asset_test::Make<Asset>(Registry(), type, packages[slot].Handle(), "");
    }

    // curve의 R 채널을 상수 value 하나로 둔다.
    bool SetKey(ObjectHandle curve, float value) noexcept {
        auto borrowed = Registry().ResolveForWrite<ColorCurveAsset>(Access(), curve);
        if (!borrowed) {
            return false;
        }

        auto pointer = borrowed.Value().Get();
        if (!pointer) {
            return false;
        }

        ColorCurveAsset& asset = *pointer.Value();
        while (asset.r.KeyCount() > 0) {
            if (!asset.r.TryRemoveKey(0)) {
                return false;
            }
        }

        return static_cast<bool>(asset.r.TryAddKey(0.0f, value));
    }

    template <class Fn>
    bool EditAtlas(Fn&& fn) noexcept {
        auto borrowed = Registry().ResolveForWrite<ColorCurveAtlas>(Access(), atlas);
        if (!borrowed) {
            return false;
        }

        auto pointer = borrowed.Value().Get();
        return pointer && fn(*pointer.Value());
    }

    bool SetCurves(std::initializer_list<ObjectHandle> list) noexcept {
        return EditAtlas([&](ColorCurveAtlas& value) noexcept {
            if (!Registry().ClearReferences(Access(), atlas, value.curves)) {
                return false;
            }

            for (ObjectHandle handle : list) {
                if (!Registry().AddReference(Access(), atlas, value.curves, handle)) {
                    return false;
                }
            }

            return true;
        });
    }

    TextureSource Source() noexcept {
        TextureSource out;
        (void)EditAtlas([&](ColorCurveAtlas& value) noexcept {
            out = value.source;
            return true;
        });
        return out;
    }

    Result<void> Rebuild(FakeRebuildSink& sink) noexcept {
        return hooks.RebuildSource(Registry(), Access(), atlas, TypeIdOf<ColorCurveAtlas>(),
                                   asset_test::FixedId(AtlasPackageId), {}, sink);
    }

    // 지금 커브로 구운 기대 byte다.
    bool Expected(Span<const ObjectHandle> curves, uint32 width, bool raw,
                  ColorCurveAtlasPixels& out) noexcept {
        Array<Borrow<ColorCurveAsset>> holds(env.allocator);
        Array<const ColorCurveAsset*> pointers(env.allocator);
        for (ObjectHandle handle : curves) {
            if (handle.IsNull()) {
                if (!pointers.TryPushBack(nullptr)) {
                    return false;
                }

                continue;
            }

            auto borrowed = Registry().Resolve<ColorCurveAsset>(Access(), handle);
            if (!borrowed) {
                return false;
            }

            auto pointer = borrowed.Value().Get();
            if (!pointer || !pointers.TryPushBack(pointer.Value()) ||
                !holds.TryPushBack(Move(borrowed.Value()))) {
                return false;
            }
        }

        return static_cast<bool>(BakeColorCurveAtlas(pointers.View(), width, raw, out));
    }

    asset_test::AssetEnv env;
    AssetTypeHookTable hooks;
    Array<AssetTypeHookRegistration> tokens;
    RootRef<Package> packages[4];
    ObjectHandle atlas{};
    ObjectHandle curveA{};
    ObjectHandle curveB{};
    ObjectHandle inner{};
    int stage = 0;
    bool ready = false;
};

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.IsEmpty() || memcmp(left.Data(), right.Data(), left.Size()) == 0);
}

} // namespace

// Edit·New·Open은 굽고(원본 RGBA16F 너비 × 행, slice·mip·layer 1, bulk = 구운 byte) MarkRebuilt
// 한다. 같은 입력이면 bulk를 더하지 않고 표시도 없다(A5j2c). 커브가 바뀌면 다시 굽는다. Propagate는
// 낡아 있어도 아무것도 하지 않는다.
TEST(CurveAtlasHooks_RebuildBakesOnlyWhenChanged) {
    HookScene scene;
    REQUIRE(scene.ready);
    REQUIRE(scene.SetCurves({scene.curveA, ObjectHandle{}}));
    REQUIRE(scene.EditAtlas([](ColorCurveAtlas& value) noexcept {
        value.width = 3;
        return true;
    }));
    const ObjectHandle rows[] = {scene.curveA, ObjectHandle{}};
    {
        FakeRebuildSink sink(scene.env.allocator, AssetSourceRebuildCause::Edit);
        REQUIRE(scene.Rebuild(sink));
        CHECK(sink.rebuilt && sink.adds == 1);
        ColorCurveAtlasPixels expected(scene.env.allocator);
        REQUIRE(scene.Expected(Span<const ObjectHandle>(rows), 3, false, expected));
        CHECK(SameBytes(sink.added.View(), expected.bytes.View()));
        const TextureSource source = scene.Source();
        CHECK(source.format == TextureSourceFormat::RGBA16F && source.sizeX == 3 &&
              source.sizeY == 2 && source.numSlices == 1 && source.numMips == 1 &&
              source.numLayers == 1 && source.bulk.bulkId == 1 &&
              source.bulk.rawBytes == expected.bytes.Size());
    }

    for (AssetSourceRebuildCause cause :
         {AssetSourceRebuildCause::Edit, AssetSourceRebuildCause::Open}) {
        FakeRebuildSink same(scene.env.allocator, cause);
        REQUIRE(scene.Rebuild(same));
        CHECK(!same.rebuilt && same.adds == 0 && same.warnings == 0);
    }

    // New는 width를 대입했으면 같은 입력이다(대입하지 않으면 팩터리 너비 — NewFactoryWidth 시험).
    {
        const StringView assigned[] = {"width"};
        FakeRebuildSink same(scene.env.allocator, AssetSourceRebuildCause::New);
        REQUIRE(scene.hooks.RebuildSource(
            scene.Registry(), scene.Access(), scene.atlas, TypeIdOf<ColorCurveAtlas>(),
            asset_test::FixedId(AtlasPackageId), Span<const StringView>(assigned), same));
        CHECK(!same.rebuilt && same.adds == 0);
    }

    // 커브가 바뀌면 Propagate는 그대로 두고 Open은 다시 굽는다.
    REQUIRE(scene.SetKey(scene.curveA, 0.75f));
    {
        FakeRebuildSink propagate(scene.env.allocator, AssetSourceRebuildCause::Propagate);
        REQUIRE(scene.Rebuild(propagate));
        CHECK(!propagate.rebuilt && propagate.adds == 0 && scene.Source().bulk.bulkId == 1);
    }

    FakeRebuildSink open(scene.env.allocator, AssetSourceRebuildCause::Open);
    REQUIRE(scene.Rebuild(open));
    CHECK(open.rebuilt && open.adds == 1);
    ColorCurveAtlasPixels fresh(scene.env.allocator);
    REQUIRE(scene.Expected(Span<const ObjectHandle>(rows), 3, false, fresh));
    CHECK(SameBytes(open.added.View(), fresh.bytes.View()));
    CHECK(scene.Source().bulk.sha256 == Sha256::Hash(fresh.bytes.View()));

    // 커브 조정(밝기)을 바꾸면 조정 적용 값으로 다시 굽고, disableAdjustments를 켜면 원값으로 다시
    // 굽는다.
    {
        auto borrowed =
            scene.Registry().ResolveForWrite<ColorCurveAsset>(scene.Access(), scene.curveA);
        REQUIRE(borrowed);
        auto pointer = borrowed.Value().Get();
        REQUIRE(pointer);
        pointer.Value()->adjustBrightness = 0.5f;
    }

    FakeRebuildSink adjusted(scene.env.allocator, AssetSourceRebuildCause::Edit);
    REQUIRE(scene.Rebuild(adjusted));
    ColorCurveAtlasPixels dimmed(scene.env.allocator);
    REQUIRE(scene.Expected(Span<const ObjectHandle>(rows), 3, false, dimmed));
    CHECK(adjusted.rebuilt && SameBytes(adjusted.added.View(), dimmed.bytes.View()));
    CHECK(!SameBytes(dimmed.bytes.View(), fresh.bytes.View()));
    REQUIRE(scene.EditAtlas([](ColorCurveAtlas& value) noexcept {
        value.disableAdjustments = true;
        return true;
    }));
    FakeRebuildSink raw(scene.env.allocator, AssetSourceRebuildCause::Edit);
    REQUIRE(scene.Rebuild(raw));
    ColorCurveAtlasPixels unadjusted(scene.env.allocator);
    REQUIRE(scene.Expected(Span<const ObjectHandle>(rows), 3, true, unadjusted));
    CHECK(raw.rebuilt && SameBytes(raw.added.View(), unadjusted.bytes.View()));
}

// 실패: 너비 상한 초과는 InvalidSetting / CapacityExceeded(detail = 굽는 너비), 행 수 초과도 같다
// (detail = 커브 수 — 앞의 커브 하나 + 빈 원소).
// 풀리지 않는 커브(패키지를 버린 커브)는 MissingDependency / 그 Resolve 오류(detail = 원소
// 번호). 원본은 그대로이고 bulk를 더하지 않는다.
TEST(CurveAtlasHooks_RebuildFailures) {
    HookScene scene;
    REQUIRE(scene.ready);
    REQUIRE(scene.SetCurves({scene.curveA}));
    REQUIRE(scene.EditAtlas([](ColorCurveAtlas& value) noexcept {
        value.width = MaxColorCurveAtlasDimension + 1;
        return true;
    }));
    {
        FakeRebuildSink sink(scene.env.allocator, AssetSourceRebuildCause::Edit);
        CHECK(scene.Rebuild(sink).Error() == ErrorCode::CapacityExceeded);
        CHECK(sink.failCode == AssetDiagnosticCode::InvalidSetting &&
              sink.failError == ErrorCode::CapacityExceeded &&
              sink.failDetail == MaxColorCurveAtlasDimension + 1);
        CHECK(sink.adds == 0 && !sink.rebuilt && scene.Source().bulk.IsEmpty());
    }

    // 행 수 상한 초과: detail = 커브 수.
    REQUIRE(scene.EditAtlas([&](ColorCurveAtlas& value) noexcept {
        value.width = 2;
        for (uint32 at = 0; at <= MaxColorCurveAtlasDimension; ++at) {
            if (!scene.Registry().AddReference(scene.Access(), scene.atlas, value.curves,
                                               ObjectHandle{})) {
                return false;
            }
        }

        return true;
    }));
    {
        FakeRebuildSink rows(scene.env.allocator, AssetSourceRebuildCause::New);
        const StringView width[] = {"Width"};
        CHECK(scene.hooks
                  .RebuildSource(scene.Registry(), scene.Access(), scene.atlas,
                                 TypeIdOf<ColorCurveAtlas>(), asset_test::FixedId(AtlasPackageId),
                                 Span<const StringView>(width), rows)
                  .Error() == ErrorCode::CapacityExceeded);
        CHECK(rows.failCode == AssetDiagnosticCode::InvalidSetting &&
              rows.failDetail == uint64{MaxColorCurveAtlasDimension} + 2 && rows.adds == 0);
    }

    // 커브 패키지를 버리면(Discard) 그 커브는 풀리지 않는다.
    const ObjectHandle doomed = scene.MakeAsset(0x7303, TypeIdOf<ColorCurveAsset>(), 3);
    REQUIRE(!doomed.IsNull());
    REQUIRE(scene.SetCurves({scene.curveA, doomed}));
    // 패키지 정책 Registry에서 소속 객체는 Discard로만 죽는다(RequestDestroy는 Unsupported).
    const ObjectHandle doomedPackage = scene.packages[3].Handle();
    scene.packages[3].Reset();
    REQUIRE(scene.Registry().Discard(scene.Access(), doomedPackage));
    FakeRebuildSink missing(scene.env.allocator, AssetSourceRebuildCause::Open);
    const Result<void> refused = scene.Rebuild(missing);
    CHECK(!refused && refused.Error() == missing.failError);
    CHECK(missing.failCode == AssetDiagnosticCode::MissingDependency && missing.failDetail == 1 &&
          missing.failError != ErrorCode::None);
    CHECK(missing.adds == 0 && scene.Source().bulk.IsEmpty());
}

// Build 의존: 커브 패키지마다 하나(id 순, 같은 패키지는 하나), null·자기 패키지(Inner) 제외 —
// 저장할 id가 달라도(복제·ID 수정) 자기 패키지는 outermost로 가른다.
TEST(CurveAtlasHooks_BuildDependencies) {
    HookScene scene;
    REQUIRE(scene.ready);
    REQUIRE(
        scene.SetCurves({scene.curveB, ObjectHandle{}, scene.curveA, scene.inner, scene.curveA}));
    AssetDependencySink sink(scene.env.allocator);
    REQUIRE(scene.hooks.CollectBuildDependencies(scene.Registry(), scene.Access(), scene.atlas,
                                                 TypeIdOf<ColorCurveAtlas>(),
                                                 asset_test::FixedId(AtlasPackageId), sink));
    REQUIRE(sink.Dependencies().Size() == 2);
    CHECK(sink.Dependencies()[0].asset == asset_test::FixedId(CurveAPackageId));
    CHECK(sink.Dependencies()[1].asset == asset_test::FixedId(CurveBPackageId));
    CHECK(sink.Dependencies()[0].kind == PackageDependencyKind::Build);

    // 복제·ID 수정은 context.asset이 새 id이고 object는 원본이다 — 자기 패키지 커브는 여전히
    // 빠진다.
    AssetDependencySink copy(scene.env.allocator);
    REQUIRE(scene.hooks.CollectBuildDependencies(scene.Registry(), scene.Access(), scene.atlas,
                                                 TypeIdOf<ColorCurveAtlas>(),
                                                 asset_test::FixedId(0x73FF), copy));
    REQUIRE(copy.Dependencies().Size() == 2);
    CHECK(copy.Dependencies()[0].asset == asset_test::FixedId(CurveAPackageId) &&
          copy.Dependencies()[1].asset == asset_test::FixedId(CurveBPackageId));

    // 커브가 없으면 의존이 없다.
    REQUIRE(scene.SetCurves({}));
    AssetDependencySink none(scene.env.allocator);
    REQUIRE(scene.hooks.CollectBuildDependencies(scene.Registry(), scene.Access(), scene.atlas,
                                                 TypeIdOf<ColorCurveAtlas>(),
                                                 asset_test::FixedId(AtlasPackageId), none));
    CHECK(none.Dependencies().IsEmpty());
}

// 정규화(Texture 정규화 뒤): 너비 0·1 → 2(큰 값은 그대로), disableAdjustments면 조정값 UE 항등값과
// 크로마 키 끔, 꺼져 있으면 조정값을 건드리지 않는다. 두 번 불러도 같다.
TEST(CurveAtlasHooks_Normalize) {
    HookScene scene;
    REQUIRE(scene.ready);
    auto normalize = [&]() noexcept {
        return scene.hooks.NormalizeSettings(scene.Registry(), scene.Access(), scene.atlas,
                                             TypeIdOf<ColorCurveAtlas>(),
                                             asset_test::FixedId(AtlasPackageId), {});
    };
    REQUIRE(scene.EditAtlas([](ColorCurveAtlas& value) noexcept {
        value.width = 1;
        value.adjustBrightness = 0.5f;
        value.adjustHue = 30.0f;
        value.chromaKeyTexture = true;
        return true;
    }));
    REQUIRE(normalize());
    CHECK(scene.EditAtlas([](ColorCurveAtlas& value) noexcept {
        return value.width == 2 && value.adjustBrightness == 0.5f && value.adjustHue == 30.0f &&
               value.chromaKeyTexture;
    }));
    REQUIRE(scene.EditAtlas([](ColorCurveAtlas& value) noexcept {
        value.width = 300;
        value.disableAdjustments = true;
        value.adjustBrightnessCurve = 2.0f;
        value.adjustVibrance = 0.5f;
        value.adjustSaturation = 0.5f;
        value.adjustRGBCurve = 2.0f;
        value.adjustMinAlpha = 0.25f;
        value.adjustMaxAlpha = 0.75f;
        return true;
    }));
    for (int pass = 0; pass < 2; ++pass) {
        REQUIRE(normalize());
        CHECK(scene.EditAtlas([](ColorCurveAtlas& value) noexcept {
            return value.width == 300 && value.adjustBrightness == 1.0f &&
                   value.adjustBrightnessCurve == 1.0f && value.adjustVibrance == 0.0f &&
                   value.adjustSaturation == 1.0f && value.adjustRGBCurve == 1.0f &&
                   value.adjustHue == 0.0f && value.adjustMinAlpha == 0.0f &&
                   value.adjustMaxAlpha == 1.0f && !value.chromaKeyTexture;
        }));
    }
}

// asset-new(까닭 New)에서 width를 대입하지 않았으면 UE 팩터리 너비 64로 굽는다. 대입했으면(대소문자
// 무시) 그 값이고, Edit·Open은 너비를 바꾸지 않는다.
TEST(CurveAtlasHooks_NewFactoryWidth) {
    HookScene scene;
    REQUIRE(scene.ready);
    {
        FakeRebuildSink sink(scene.env.allocator, AssetSourceRebuildCause::New);
        REQUIRE(scene.Rebuild(sink));
        CHECK(sink.rebuilt && scene.Source().sizeX == NewColorCurveAtlasWidth &&
              scene.Source().sizeY == 1);
        CHECK(scene.EditAtlas([](ColorCurveAtlas& value) noexcept {
            return value.width == NewColorCurveAtlasWidth;
        }));
    }

    REQUIRE(scene.EditAtlas([](ColorCurveAtlas& value) noexcept {
        value.width = 8;
        return true;
    }));
    for (AssetSourceRebuildCause cause :
         {AssetSourceRebuildCause::Edit, AssetSourceRebuildCause::Open}) {
        FakeRebuildSink sink(scene.env.allocator, cause);
        REQUIRE(scene.Rebuild(sink));
        CHECK(scene.Source().sizeX == 8);
    }

    const StringView assigned[] = {"WIDTH"};
    REQUIRE(scene.EditAtlas([](ColorCurveAtlas& value) noexcept {
        value.width = 16;
        return true;
    }));
    FakeRebuildSink assignedNew(scene.env.allocator, AssetSourceRebuildCause::New);
    REQUIRE(scene.hooks.RebuildSource(
        scene.Registry(), scene.Access(), scene.atlas, TypeIdOf<ColorCurveAtlas>(),
        asset_test::FixedId(AtlasPackageId), Span<const StringView>(assigned), assignedNew));
    CHECK(scene.Source().sizeX == 16);
}

// 등록은 한 번이다(다시 등록 AlreadyExists). RowOf는 첫 같은 원소, 없거나 null이면 IndexNone.
TEST(CurveAtlasHooks_RegistrationAndRows) {
    HookScene scene;
    REQUIRE(scene.ready);
    Array<AssetTypeHookRegistration> again(scene.env.allocator);
    CHECK(RegisterCurveAtlasTypeHooks(nullptr, scene.hooks, again).Error() ==
          ErrorCode::AlreadyExists);
    REQUIRE(scene.SetCurves({scene.curveB, scene.curveA, scene.curveB}));
    CHECK(scene.EditAtlas([&](ColorCurveAtlas& value) noexcept {
        return value.RowOf(scene.curveB) == 0 && value.RowOf(scene.curveA) == 1 &&
               value.RowOf(scene.inner) == IndexNone && value.RowOf(ObjectHandle{}) == IndexNone;
    }));
}

} // namespace curve_atlas_hook_tests_file
