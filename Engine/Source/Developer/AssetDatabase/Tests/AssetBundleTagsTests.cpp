#include "../../../Runtime/AssetRuntime/Tests/AssetTestSupport.h"

#include <AssetDatabase/AssetBundleTags.h>
#include <AssetTestFixtures/BundleAssets.generated.h>
#include <AssetTestFixtures/BundleAssets.h>
#include <Core/Text/StringBuilder.h>
#include <TestSupport/NFailAllocator.h>

// bundle 태그 수집 시험이다(Docs/Design/AssetSystem 10.2절 "A4e 구현 세부" — A4e2b): 지정자
// AssetBundles의 실효 bundle(경로 목록의 교집합 — 단일·배열·struct 사슬·Map 키·InstancedStruct
// 내용), 지정 없음·null·EditorOnly 경로(필드·InstancedStruct)는 태그 없음, bundle 대상이 없는
// 타입은 순회하지 않음, 태그 형식(`Bundle.<이름>` = AssetId 표준 문자열, 같은 쌍 합침), 할당 실패.
using namespace rocks;
namespace asset_bundle_tags_tests_file {

using namespace rocks::asset_test;
using asset_fixture::BundleAsset;
using asset_fixture::BundleSlot;

namespace {

// 구성원 후보 에셋 id다(태그 값으로만 쓴다 — 대상 패키지는 없어도 된다).
constexpr uint64 PortraitId = 11;
constexpr uint64 LevelA = 12;
constexpr uint64 LevelB = 13;
constexpr uint64 IconId = 14;
constexpr uint64 AnyId = 15;
constexpr uint64 PlainId = 16;
constexpr uint64 WeightId = 17;
constexpr uint64 ExtraIcon = 18;
constexpr uint64 ExtraAny = 19;
constexpr uint64 EditorIcon = 20;
constexpr uint64 EditorSoft = 21;

bool SetSoft(SoftObjectRef<Asset>& ref, uint64 id) noexcept {
    return static_cast<bool>(ref.TryAssign(FixedId(id), ""));
}

// 모든 칸을 채운 BundleAsset 장면이다.
struct BundleScene {
    explicit BundleScene(AssetEnv& sceneEnv) noexcept : env(sceneEnv) {
        ObjectRegistry& registry = *env.source;
        auto made = NewPackage(registry, registry.OwnerAccess(), FixedId(1), PackageKind::Editor);
        if (!made) {
            return;
        }

        package = Move(made.Value());
        asset = Make<BundleAsset>(registry, GeneratedType<BundleAsset>::Id(), package.Handle(), "");
        BundleAsset* value = Pointer<BundleAsset>(registry, asset);
        if (value == nullptr) {
            return;
        }

        SoftObjectRef<Asset> levelA(registry.ObjectAllocator());
        SoftObjectRef<Asset> levelB(registry.ObjectAllocator());
        SoftObjectRef<Asset> weight(registry.ObjectAllocator());
        ready = SetSoft(value->portrait, PortraitId) && SetSoft(levelA, LevelA) &&
                SetSoft(levelB, LevelB) && value->levels.TryPushBack(Move(levelA)) &&
                value->levels.TryPushBack(Move(levelB)) && SetSoft(value->slot.icon, IconId) &&
                SetSoft(value->slot.any, AnyId) && SetSoft(value->plain, PlainId) &&
                SetSoft(weight, WeightId) && value->weights.Insert(Move(weight), int32{3}) &&
                value->extra.Emplace<BundleSlot>(env.types, registry.ObjectAllocator()) &&
                value->editorExtra.Emplace<BundleSlot>(env.types, registry.ObjectAllocator()) &&
                SetSoft(value->extra.Get<BundleSlot>()->icon, ExtraIcon) &&
                SetSoft(value->extra.Get<BundleSlot>()->any, ExtraAny) &&
                SetSoft(value->editorExtra.Get<BundleSlot>()->icon, EditorIcon) &&
                SetSoft(value->editorSoft, EditorSoft);
    }

    Result<void> Collect(Allocator& allocator, AssetTagSink& sink) noexcept {
        ObjectRegistry& registry = *env.source;
        return CollectAssetBundleTags(registry, registry.OwnerAccess(), asset, allocator, sink);
    }

    AssetEnv& env;
    RootRef<Package> package;
    ObjectHandle asset{};
    bool ready = false;
};

// sink에 (Bundle.<name>, id 표준 문자열) 태그가 있는지다.
bool HasTag(const AssetTagSink& sink, StringView name, uint64 id) noexcept {
    char key[64] = {};
    char value[64] = {};
    FixedStringBuilder keyText{Span<char>(key)};
    FixedStringBuilder valueText{Span<char>(value)};
    if (!keyText.TryAppendFormat("{}{}", AssetBundleTagPrefix, name) ||
        !valueText.TryAppendFormat("{}", FixedId(id))) {
        return false;
    }

    for (const PackageTag& tag : sink.Tags()) {
        if (tag.key.View() == keyText.View() && tag.value.View() == valueText.View()) {
            return true;
        }
    }

    return false;
}

// id를 값으로 가진 태그가 하나라도 있는지다.
bool HasMember(const AssetTagSink& sink, uint64 id) noexcept {
    char value[64] = {};
    FixedStringBuilder valueText{Span<char>(value)};
    if (!valueText.TryAppendFormat("{}", FixedId(id))) {
        return true;
    }

    for (const PackageTag& tag : sink.Tags()) {
        if (tag.value.View() == valueText.View()) {
            return true;
        }
    }

    return false;
}

} // namespace

// 실효 bundle: portrait {UI, Game}, levels {Game}, slot.icon {UI,Game}∩{UI} = {UI}, slot.any {UI},
// weights 키 {Audio}, extra 내용 icon {UI,Game}∩{Game} = {Game}·any {Game}. plain(지정 없음)·
// editorExtra·editorSoft(EditorOnly 경로)는 태그가 없다. 태그는 10개다.
TEST(AssetBundleTags_EffectiveBundles) {
    AssetEnv env;
    REQUIRE(env.Ready());
    BundleScene scene(env);
    REQUIRE(scene.ready);
    AssetTagSink sink(env.allocator);
    REQUIRE(scene.Collect(env.allocator, sink));
    CHECK(HasTag(sink, "UI", PortraitId) && HasTag(sink, "Game", PortraitId));
    CHECK(HasTag(sink, "Game", LevelA) && HasTag(sink, "Game", LevelB) &&
          !HasTag(sink, "UI", LevelA));
    CHECK(HasTag(sink, "UI", IconId) && !HasTag(sink, "Game", IconId));
    CHECK(HasTag(sink, "UI", AnyId) && !HasTag(sink, "Game", AnyId));
    CHECK(HasTag(sink, "Audio", WeightId));
    CHECK(HasTag(sink, "Game", ExtraIcon) && !HasTag(sink, "UI", ExtraIcon));
    CHECK(HasTag(sink, "Game", ExtraAny));
    CHECK(!HasMember(sink, PlainId) && !HasMember(sink, EditorIcon) &&
          !HasMember(sink, EditorSoft));
    CHECK(sink.Tags().Size() == 10);

    // 같은 쌍은 합친다(두 번 모아도 그대로).
    REQUIRE(scene.Collect(env.allocator, sink));
    CHECK(sink.Tags().Size() == 10);
}

// null 참조는 태그가 없고, 교집합이 비면 태그가 없다(icon을 비우고 바깥 slot 목록과 겹치지 않는
// 경우는 fixture로 만들 수 없어 extra 내용의 any를 null로 둔다). bundle 대상이 없는 타입(TestAsset
// — 목록도 InstancedStruct도 없다)은 순회하지 않는다.
TEST(AssetBundleTags_NullAndSkippedTypes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    BundleScene scene(env);
    REQUIRE(scene.ready);
    ObjectRegistry& registry = *env.source;
    BundleAsset* value = Pointer<BundleAsset>(registry, scene.asset);
    REQUIRE(value != nullptr);
    REQUIRE(value->portrait.TryAssign(AssetId{}, ""));
    REQUIRE(value->extra.Get<BundleSlot>()->any.TryAssign(AssetId{}, ""));
    AssetTagSink sink(env.allocator);
    REQUIRE(scene.Collect(env.allocator, sink));
    CHECK(!HasMember(sink, PortraitId) && !HasMember(sink, ExtraAny) && HasTag(sink, "UI", IconId));
    CHECK(sink.Tags().Size() == 7);

    auto other = NewPackage(registry, registry.OwnerAccess(), FixedId(2), PackageKind::Editor);
    REQUIRE(other);
    const ObjectHandle plain =
        Make<TestAsset>(registry, TestAssetType(), other.Value().Handle(), "");
    REQUIRE(!plain.IsNull());
    REQUIRE(Pointer<TestAsset>(registry, plain)->soft.TryAssign(FixedId(PlainId), ""));
    AssetTagSink none(env.allocator);
    REQUIRE(CollectAssetBundleTags(registry, registry.OwnerAccess(), plain, env.allocator, none));
    CHECK(none.Tags().Size() == 0);
}

// 오류: 사라진 객체는 타입 조회 오류, 할당 실패는 모든 지점에서 OutOfMemory(다 지나면 기준과 같다).
TEST(AssetBundleTags_Failures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    BundleScene scene(env);
    REQUIRE(scene.ready);
    ObjectRegistry& registry = *env.source;
    AssetTagSink stale(env.allocator);
    CHECK(!CollectAssetBundleTags(registry, registry.OwnerAccess(), ObjectHandle{}, env.allocator,
                                  stale));

    // 상한은 무한 반복을 막는 장치다(이 장면의 할당은 수십 번이다).
    constexpr uint64 MaxAttempts = 10000;
    bool succeeded = false;
    for (uint64 failAt = 1; !succeeded && failAt < MaxAttempts; ++failAt) {
        test::NFailAllocator failing(env.allocator);
        failing.FailOnAttempt(failAt);
        AssetTagSink sink(failing);
        auto collected = scene.Collect(failing, sink);
        if (collected) {
            succeeded = true;
            CHECK(sink.Tags().Size() == 10);
        } else {
            CHECK(collected.Error() == ErrorCode::OutOfMemory);
        }
    }

    CHECK(succeeded);
}

} // namespace asset_bundle_tags_tests_file
