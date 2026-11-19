#include "AssetCombination.h"

#include <TestSupport/Test.h>

// "지원하는 모든 조합" 왕복 시험이다(Docs/Design/AssetSystem.ko.md 14.1절).
// - FixtureAsset_CombinationRoundTrip: ComboAsset·ComboEditorAsset을 쿠킹·에디터 × seed 둘로 저장 →
//   다른 Registry에 로드 → 필드 단위 비교(참조는 대상 표의 같은 정체성, weak의 다른 패키지 대상은
//   null, 쿠킹의 EditorOnly는 기본값) → 다시 저장해 byte 동일.
// - FixtureAsset_CombinationRequired: Required 칸 하나씩 null로 두면 저장이 거부하고, 모두 채우면
//   왕복한다.
using namespace rocks;
using namespace rocks::asset_test;

namespace {

// 쓴 쪽 장면이다. 패키지 A(대표 에셋 + Body(ComboPartDerived) + Body.Socket(ComboPart)), 다른
// 패키지 B(TestAsset + Gear + Gear.Tooth). 없는 패키지 C는 Soft 값으로만 나온다.
struct ComboScene {
    explicit ComboScene(Allocator& allocator) noexcept : payload(allocator) {
    }

    RootRef<Package> a;
    RootRef<Package> b;
    ObjectHandle targets[ComboTargetCount] = {};
    Array<uint8> payload;
    TestBulkSource bulk;
};

constexpr uint64 ComboSelf = 300;
constexpr uint64 ComboOther = 301;
constexpr uint64 ComboAbsent = 302;

void SetIdentity(ComboContext& context, uint64 self) noexcept {
    context.self = FixedId(self);
    context.other = FixedId(ComboOther);
    context.absent = FixedId(ComboAbsent);
}

ObjectHandle& TargetSlot(ObjectHandle* targets, ComboTarget target) noexcept {
    return targets[static_cast<uint32>(target)];
}

template <class A>
bool BuildComboScene(AssetEnv& env, ComboScene& scene, PackageKind kind, uint64 self,
                     uint32 seed) noexcept {
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    auto a = NewPackage(registry, access, FixedId(self), kind);
    auto b = NewPackage(registry, access, FixedId(ComboOther), kind);
    if (!a || !b) {
        return false;
    }

    scene.a = Move(a.Value());
    scene.b = Move(b.Value());
    ObjectHandle* t = scene.targets;
    TargetSlot(t, ComboTarget::Other) =
        Make<TestAsset>(registry, TestAssetType(), scene.b.Handle(), "");
    TargetSlot(t, ComboTarget::Gear) =
        Make<TestPart>(registry, TestPartType(), TargetSlot(t, ComboTarget::Other), "Gear");
    TargetSlot(t, ComboTarget::Tooth) =
        Make<TestPart>(registry, TestPartType(), TargetSlot(t, ComboTarget::Gear), "Tooth");
    TargetSlot(t, ComboTarget::Self) = Make<A>(registry, IdOf<A>(), scene.a.Handle(), "");
    TargetSlot(t, ComboTarget::Body) = Make<ComboPartDerived>(
        registry, IdOf<ComboPartDerived>(), TargetSlot(t, ComboTarget::Self), "Body");
    TargetSlot(t, ComboTarget::Socket) =
        Make<ComboPart>(registry, IdOf<ComboPart>(), TargetSlot(t, ComboTarget::Body), "Socket");
    for (uint32 index = 1; index < ComboTargetCount; ++index) {
        if (t[index].IsNull()) {
            return false;
        }
    }

    ComboContext context(registry, env.types, env.names);
    SetIdentity(context, self);
    for (uint32 index = 0; index < ComboTargetCount; ++index) {
        context.targets[index] = t[index];
    }

    FillIncompressible(scene.payload, 1500 + seed, seed);
    context.owner = TargetSlot(t, ComboTarget::Self);
    if (!WithBorrow<A>(registry, context.owner, [&](A& asset) {
            asset.fBulk = scene.bulk.Add(7, scene.payload.View());
            return PutComboAsset(context, asset, seed);
        })) {
        return false;
    }

    context.owner = TargetSlot(t, ComboTarget::Body);
    if (!WithBorrow<ComboPartDerived>(registry, context.owner, [&](ComboPartDerived& body) {
            body.index = static_cast<int32>(seed);
            return PutObjectRefs(context, body.refs, seed + 500) &&
                   Put(context, body.watcher, seed + 501, ComboSlot::Value) &&
                   Label(body.note, "body", seed) &&
                   registry
                       .SetReference(access, context.owner, body.next,
                                     TargetSlot(t, ComboTarget::Socket))
                       .IsOk();
        })) {
        return false;
    }

    context.owner = TargetSlot(t, ComboTarget::Socket);
    return WithBorrow<ComboPart>(registry, context.owner, [&](ComboPart& socket) {
        socket.index = -static_cast<int32>(seed);
        return PutObjectRefs(context, socket.refs, seed + 600) &&
               Put(context, socket.watcher, seed + 601, ComboSlot::Value);
    });
}

// 읽은 쪽 대상 표를 패키지 handle에서 찾는다.
bool FindComboTargets(ObjectRegistry& registry, ObjectHandle a, ObjectHandle b,
                      ComboContext& context) noexcept {
    const ObjectAccessContext access = registry.OwnerAccess();
    struct Lookup {
        ComboTarget target;
        bool inA;
        StringView path;
    };

    const Lookup lookups[] = {
        {ComboTarget::Self, true, StringView()},    {ComboTarget::Body, true, "Body"},
        {ComboTarget::Socket, true, "Body.Socket"}, {ComboTarget::Other, false, StringView()},
        {ComboTarget::Gear, false, "Gear"},         {ComboTarget::Tooth, false, "Gear.Tooth"}};
    for (const Lookup& lookup : lookups) {
        auto found = registry.FindObject(access, lookup.inA ? a : b, lookup.path);
        if (!found) {
            return false;
        }

        context.targets[static_cast<uint32>(lookup.target)] = found.Value();
    }

    return true;
}

// A 클래스로 장면을 만들어 kind로 왕복하고 필드·byte를 비교한다.
template <class A>
void RunComboRoundTrip(PackageKind kind, uint32 seed) noexcept {
    AssetEnv env;
    REQUIRE(env.Ready());
    ComboScene scene(env.allocator);
    REQUIRE(BuildComboScene<A>(env, scene, kind, ComboSelf, seed));
    PackageWriteOptions options = kind == PackageKind::Cooked ? CookedOptions() : EditorOptions();
    options.bulkSource = &scene.bulk;
    options.bulkCodec = seed % 2 == 0 ? BulkCodec::Zstd : BulkCodec::None;
    Array<uint8> aBytes = Write(env.allocator, *env.source, scene.a.Handle(), options);
    Array<uint8> bBytes = Write(env.allocator, *env.source, scene.b.Handle(), options);
    REQUIRE(!aBytes.IsEmpty() && !bBytes.IsEmpty());
    const Array<uint8> original = Copy(env.allocator, aBytes.View());

    // 같은 상태를 다시 쓰면 같은 byte다(결정성).
    Array<uint8> twice = Write(env.allocator, *env.source, scene.a.Handle(), options);
    CHECK(SameBytes(twice.View(), original.View()));

    // A → B는 순환이 아니므로 B를 먼저 올리고 resolver로 잇는다.
    TestResolver resolver;
    RootRef<Object> bRoot;
    {
        Array<uint8> files[1] = {Move(bBytes)};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        REQUIRE(LoadAll(env, *env.target, files, kind, &resolver, roots, handles));
        bRoot = Move(roots[0]);
        resolver.Add(FixedId(ComboOther), handles[0]);
    }

    Array<uint8> files[1] = {Move(aBytes)};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    REQUIRE(LoadAll(env, *env.target, files, kind, &resolver, roots, handles));

    ObjectRegistry& target = *env.target;
    ComboContext context(target, env.types, env.names);
    SetIdentity(context, ComboSelf);
    context.loaded = true;
    REQUIRE(FindComboTargets(target, handles[0], resolver.handles[0], context));
    context.expectDefault = IsSameV<A, ComboEditorAsset> && kind == PackageKind::Cooked;
    A* asset = Pointer<A>(target, TargetSlot(context.targets, ComboTarget::Self));
    REQUIRE(asset != nullptr);
    CHECK(SameComboAsset(context, *asset, seed));
    if (!context.expectDefault) {
        // 다른 seed의 기대값과는 다르다(비교기가 실제로 값을 본다).
        CHECK(!SameComboAsset(context, *asset, seed + 1));
    }

    Package* package = Pointer<Package>(target, handles[0]);
    REQUIRE(package != nullptr);
    if (!context.expectDefault) {
        auto bytes = package->BulkBytes(asset->fBulk);
        REQUIRE(bytes);
        CHECK(SameBytes(bytes.Value(), scene.payload.View()));
    } else {
        CHECK(asset->fBulk.IsEmpty());
        CHECK(package->BulkCount() == 0);
    }

    context.expectDefault = false;
    ComboPartDerived* body =
        Pointer<ComboPartDerived>(target, TargetSlot(context.targets, ComboTarget::Body));
    ComboPart* socket =
        Pointer<ComboPart>(target, TargetSlot(context.targets, ComboTarget::Socket));
    REQUIRE(body != nullptr && socket != nullptr);
    CHECK(body->index == static_cast<int32>(seed));
    CHECK(SameObjectRefs(context, body->refs, seed + 500));
    CHECK(Same(context, body->watcher, seed + 501));
    CHECK(LabelIs(body->note.View(), "body", seed));
    CHECK(body->next.Handle() == TargetSlot(context.targets, ComboTarget::Socket));
    CHECK(socket->index == -static_cast<int32>(seed));
    CHECK(SameObjectRefs(context, socket->refs, seed + 600));
    CHECK(Same(context, socket->watcher, seed + 601));

    // 로드한 상태를 다시 쓰면 같은 byte다.
    PackageBulkReader reader(target, handles[0]);
    options.bulkSource = &reader;
    Array<uint8> again = Write(env.allocator, target, handles[0], options);
    CHECK(SameBytes(again.View(), original.View()));
}

} // namespace

// 모든 조합 왕복: 쿠킹·에디터 × 홀수·짝수 seed(Optional 두 상태, bulk None·Zstd) × 두 클래스.
TEST(FixtureAsset_CombinationRoundTrip) {
    const PackageKind kinds[] = {PackageKind::Cooked, PackageKind::Editor};
    const uint32 seeds[] = {3, 4};
    for (PackageKind kind : kinds) {
        for (uint32 seed : seeds) {
            RunComboRoundTrip<ComboAsset>(kind, seed);
            RunComboRoundTrip<ComboEditorAsset>(kind, seed);
        }
    }
}

namespace {

// Required 칸 수다(아래 FillRequired의 cell 번호 0..RequiredCells-1).
constexpr usize RequiredCells = 17;

// Required 칸을 모두 non-null로 채운다. broken이 IndexNone이 아니면 그 칸 하나를 null로 둔다.
bool FillRequired(ComboContext& context, ComboRequiredAsset& asset, usize broken) noexcept {
    ObjectRegistry& registry = context.registry;
    const ObjectAccessContext access = registry.OwnerAccess();
    const ObjectHandle body = TargetSlot(context.targets, ComboTarget::Body);
    const ObjectHandle other = TargetSlot(context.targets, ComboTarget::Other);
    auto pick = [&](usize cell, ObjectHandle value) noexcept {
        return cell == broken ? ObjectHandle{} : value;
    };
    auto kind = [&](usize cell, uint32 index) noexcept {
        return ClassRef<Asset>(cell == broken ? TypeId{} : ClassOf(index));
    };
    auto soft = [&](usize cell, SoftObjectRef<Object>& out, uint32 index) noexcept {
        const SoftEntry entry = cell == broken ? SoftEntry{} : SoftOf(context, index);
        return out.TryAssign(entry.asset, entry.path).IsOk();
    };

    asset.classRef = kind(4, 1);
    asset.classFixed[0] = ClassRef<Asset>(ClassOf(2));
    asset.classFixed[1] = kind(6, 3);
    bool ok =
        registry.SetReference(access, context.owner, asset.ref, pick(0, body)).IsOk() &&
        registry.AddReference(access, context.owner, asset.refArray, other).IsOk() &&
        registry.AddReference(access, context.owner, asset.refArray, pick(1, body)).IsOk() &&
        registry.SetReference(access, context.owner, asset.refFixed[0], body).IsOk() &&
        registry.SetReference(access, context.owner, asset.refFixed[1], pick(2, other)).IsOk() &&
        registry.SetMapReference(access, context.owner, asset.refMap, 1, pick(3, body)).IsOk() &&
        asset.classArray.TryPushBack(ClassRef<Asset>(ClassOf(4))).IsOk() &&
        asset.classArray.TryPushBack(kind(5, 5)).IsOk() &&
        asset.classMap.Insert(7, kind(7, 1)).IsOk() &&
        asset.classSet.Add(ClassRef<Asset>(ClassOf(2))).IsOk() &&
        asset.classSet.Add(kind(8, 3)).IsOk() && soft(9, asset.soft, 1);
    SoftObjectRef<Object> element(context.allocator);
    ok = ok && soft(10, element, 2) && asset.softArray.TryPushBack(Move(element)).IsOk();
    ok = ok && soft(11, asset.softFixed[0], 3) && soft(RequiredCells, asset.softFixed[1], 4);
    SoftObjectRef<Object> mapped(context.allocator);
    ok = ok && soft(12, mapped, 5) && asset.softMap.Insert(3, Move(mapped)).IsOk();
    SoftObjectRef<Object> member(context.allocator);
    ok = ok && soft(13, member, 1) && asset.softSet.Add(Move(member)).IsOk();
    asset.need.kind = kind(14, 2);
    ok = ok && soft(15, asset.need.asset, 2);
    auto need = asset.needs.TryEmplaceBack(context.allocator);
    ok = ok && need && soft(RequiredCells, need.Value()->asset, 3);
    if (need) {
        need.Value()->kind = ClassRef<Asset>(ClassOf(4));
    }

    return ok && registry.SetReference(access, context.owner, asset.needLink.target, pick(16, body))
                     .IsOk();
}

} // namespace

// Required: 칸 하나라도 null이면 쿠킹·에디터 저장 모두 거부하고(InvalidArgument), 모두 채우면
// 왕복한다(쿠킹 로드의 Required 실효 검사 통과).
TEST(FixtureAsset_CombinationRequired) {
    const PackageKind kinds[] = {PackageKind::Cooked, PackageKind::Editor};
    for (PackageKind kind : kinds) {
        for (usize broken = 0; broken <= RequiredCells; ++broken) {
            AssetEnv env;
            REQUIRE(env.Ready());
            ObjectRegistry& registry = *env.source;
            const ObjectAccessContext access = registry.OwnerAccess();
            auto a = NewPackage(registry, access, FixedId(310), kind);
            auto b = NewPackage(registry, access, FixedId(ComboOther), kind);
            REQUIRE(a && b);
            ComboContext context(registry, env.types, env.names);
            SetIdentity(context, 310);
            context.owner = Make<ComboRequiredAsset>(registry, IdOf<ComboRequiredAsset>(),
                                                     a.Value().Handle(), "");
            TargetSlot(context.targets, ComboTarget::Body) =
                Make<ComboPart>(registry, IdOf<ComboPart>(), context.owner, "Body");
            TargetSlot(context.targets, ComboTarget::Other) =
                Make<TestAsset>(registry, TestAssetType(), b.Value().Handle(), "");
            const usize cell = broken == RequiredCells ? IndexNone : broken;
            REQUIRE(WithBorrow<ComboRequiredAsset>(
                registry, context.owner,
                [&](ComboRequiredAsset& asset) { return FillRequired(context, asset, cell); }));
            PackageWriteOptions options =
                kind == PackageKind::Cooked ? CookedOptions() : EditorOptions();
            MemoryPackageSink sink(env.allocator);
            auto written =
                WritePackage(env.allocator, registry, access, a.Value().Handle(), options, sink);
            if (cell != IndexNone) {
                CHECK(!written);
                CHECK(!written && written.Error() == ErrorCode::InvalidArgument);
                continue;
            }

            REQUIRE(written);
            Array<uint8> bBytes = Write(env.allocator, registry, b.Value().Handle(), options);
            Array<uint8> files[2] = {sink.Take(), Move(bBytes)};
            RootRef<Object> roots[2];
            ObjectHandle handles[2];
            CHECK(LoadAll(env, *env.target, files, kind, nullptr, roots, handles));
        }
    }
}
