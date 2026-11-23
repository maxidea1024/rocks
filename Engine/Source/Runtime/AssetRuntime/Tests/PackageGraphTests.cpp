#include "AssetFixtureValues.h"

#include <Platform/Thread.h>

// 패키지 그래프 시험이다(Docs/Design/AssetSystem 11.3절, 14절 7·28): 3노드 하드 순환을 어떤
// 순서로 넘겨도 한 트랜잭션에 로드되고 둘만으로는 MissingDependency, 다이아몬드(A → B·C → D)의
// 의존 표·아래부터 로드·한 묶음 로드, 자기 참조(대표·하위 객체·Soft)는 의존을 만들지 않음, 실패
// 전파(묶음 하나의 실패는 묶음 전체를 공개하지 않음), 복합 에셋(GameDataAsset — 하위 객체 여러 개·
// 다른 에셋 Hard 배열·Soft·weak·커브·bulk), 스레드 규칙(Prepare는 다른 스레드, 쓰기는 소유 스레드).
using namespace rocks;
namespace package_graph_tests_file {

using namespace rocks::asset_test;

namespace {

// 대표 에셋(TestAsset) 하나짜리 패키지들을 만들고 hard 간선을 잇는다.
struct Chain {
    RootRef<Package> packages[4];
    ObjectHandle assets[4] = {};
};

bool MakeChain(ObjectRegistry& registry, Chain& chain, usize count, uint64 firstId) noexcept {
    const ObjectAccessContext access = registry.OwnerAccess();
    for (usize index = 0; index < count; ++index) {
        auto package = NewPackage(registry, access, FixedId(firstId + index), PackageKind::Cooked);
        if (!package) {
            return false;
        }

        chain.packages[index] = Move(package.Value());
        chain.assets[index] =
            Make<TestAsset>(registry, TestAssetType(), chain.packages[index].Handle(), "");
        TestAsset* asset = Pointer<TestAsset>(registry, chain.assets[index]);
        if (asset == nullptr) {
            return false;
        }

        asset->value = static_cast<int32>(firstId + index);
    }

    return true;
}

bool Link(ObjectRegistry& registry, ObjectHandle from, ObjectHandle to) noexcept {
    TestAsset* asset = Pointer<TestAsset>(registry, from);
    return asset != nullptr &&
           registry.SetReference(registry.OwnerAccess(), from, asset->hard, to).IsOk();
}

bool AddListed(ObjectRegistry& registry, ObjectHandle from, ObjectHandle to) noexcept {
    return WithBorrow<TestAsset>(registry, from, [&](TestAsset& asset) {
        return registry.AddReference(registry.OwnerAccess(), from, asset.hardList, to).IsOk();
    });
}

// 로드한 패키지 handle의 대표 에셋이다.
TestAsset* Representative(ObjectRegistry& registry, ObjectHandle package,
                          ObjectHandle* handle = nullptr) noexcept {
    auto found = registry.FindObject(registry.OwnerAccess(), package, StringView());
    if (!found) {
        return nullptr;
    }

    if (handle != nullptr) {
        *handle = found.Value();
    }

    return Pointer<TestAsset>(registry, found.Value());
}

} // namespace

// 3노드 하드 순환 A → B → C → A: 어떤 순서로 넘겨도 한 트랜잭션에서 서로를 가리키고, 둘만 넘기면
// 빠진 하나가 MissingDependency이며 아무 객체도 남지 않는다(AS 11.3절, 14절 28).
TEST(AssetPackage_ThreeNodeCycleLoadsInAnyOrder) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Chain chain;
    REQUIRE(MakeChain(*env.source, chain, 3, 400));
    REQUIRE(Link(*env.source, chain.assets[0], chain.assets[1]));
    REQUIRE(Link(*env.source, chain.assets[1], chain.assets[2]));
    REQUIRE(Link(*env.source, chain.assets[2], chain.assets[0]));
    Array<uint8> bytes[3] = {
        Write(env.allocator, *env.source, chain.packages[0].Handle(), CookedOptions()),
        Write(env.allocator, *env.source, chain.packages[1].Handle(), CookedOptions()),
        Write(env.allocator, *env.source, chain.packages[2].Handle(), CookedOptions())};
    for (const Array<uint8>& file : bytes) {
        REQUIRE(!file.IsEmpty());
    }

    const usize orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (const auto& order : orders) {
        AssetEnv load;
        REQUIRE(load.Ready());
        Array<uint8> files[3] = {Copy(load.allocator, bytes[order[0]].View()),
                                 Copy(load.allocator, bytes[order[1]].View()),
                                 Copy(load.allocator, bytes[order[2]].View())};
        RootRef<Object> roots[3];
        ObjectHandle handles[3];
        REQUIRE(LoadAll(load, *load.target, files, PackageKind::Cooked, nullptr, roots, handles));
        ObjectHandle byId[3] = {};
        TestAsset* assets[3] = {};
        for (usize position = 0; position < 3; ++position) {
            assets[order[position]] =
                Representative(*load.target, handles[position], &byId[order[position]]);
            REQUIRE(assets[order[position]] != nullptr);
        }

        for (usize index = 0; index < 3; ++index) {
            CHECK(assets[index]->value == static_cast<int32>(400 + index));
            CHECK(assets[index]->hard.Handle() == byId[(index + 1) % 3]);
        }
    }

    // 둘만: 빠진 하나(세 번째)가 MissingDependency이고 트랜잭션은 아무것도 남기지 않는다.
    for (usize missing = 0; missing < 3; ++missing) {
        AssetEnv load;
        REQUIRE(load.Ready());
        Array<uint8> files[2] = {Copy(load.allocator, bytes[(missing + 1) % 3].View()),
                                 Copy(load.allocator, bytes[(missing + 2) % 3].View())};
        RootRef<Object> roots[2];
        ObjectHandle handles[2];
        AssetDiagnostic failure;
        CHECK(!LoadAll(load, *load.target, files, PackageKind::Cooked, nullptr, roots, handles,
                       &failure));
        CHECK(failure.code == AssetDiagnosticCode::MissingDependency);
        CHECK(failure.related == FixedId(400 + missing));
        CHECK(load.target->Stats().liveObjects == 0);
    }
}

// 다이아몬드 A → B, A → C, B → D, C → D: A의 의존은 B·C뿐이고(전이 의존 D는 없다), 아래부터
// resolver로 잇거나 넷을 한 묶음으로 넘겨도 같은 그래프다. D 없이 B만 올리면 MissingDependency다.
TEST(AssetPackage_DiamondDependencies) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Chain chain;
    REQUIRE(MakeChain(*env.source, chain, 4, 410));
    ObjectRegistry& source = *env.source;
    REQUIRE(Link(source, chain.assets[0], chain.assets[1]));
    REQUIRE(AddListed(source, chain.assets[0], chain.assets[2]));
    REQUIRE(Link(source, chain.assets[1], chain.assets[3]));
    REQUIRE(Link(source, chain.assets[2], chain.assets[3]));
    Array<uint8> bytes[4] = {Array<uint8>(env.allocator), Array<uint8>(env.allocator),
                             Array<uint8>(env.allocator), Array<uint8>(env.allocator)};
    for (usize index = 0; index < 4; ++index) {
        bytes[index] =
            Write(env.allocator, source, chain.packages[index].Handle(), CookedOptions());
        REQUIRE(!bytes[index].IsEmpty());
    }

    auto contents = ReadPackage(env.allocator, bytes[0].View(), PackageReadOptions{});
    REQUIRE(contents);
    REQUIRE(contents.Value().dependencies.Size() == 2);
    CHECK(contents.Value().dependencies[0].asset == FixedId(411));
    CHECK(contents.Value().dependencies[1].asset == FixedId(412));
    CHECK(contents.Value().dependencies[0].kind == PackageDependencyKind::Hard);
    CHECK(contents.Value().dependencies[1].kind == PackageDependencyKind::Hard);

    // 아래부터: D → B·C → A.
    {
        AssetEnv load;
        REQUIRE(load.Ready());
        TestResolver resolver;
        RootRef<Object> keep[4];
        const usize order[4] = {3, 1, 2, 0};
        ObjectHandle packages[4] = {};
        for (usize step = 0; step < 4; ++step) {
            const usize index = order[step];
            Array<uint8> files[1] = {Copy(load.allocator, bytes[index].View())};
            RootRef<Object> roots[1];
            ObjectHandle handles[1];
            REQUIRE(
                LoadAll(load, *load.target, files, PackageKind::Cooked, &resolver, roots, handles));
            keep[index] = Move(roots[0]);
            packages[index] = handles[0];
            resolver.Add(FixedId(410 + index), handles[0]);
        }

        ObjectHandle d{};
        ObjectHandle b{};
        ObjectHandle c{};
        TestAsset* a = Representative(*load.target, packages[0]);
        TestAsset* bAsset = Representative(*load.target, packages[1], &b);
        TestAsset* cAsset = Representative(*load.target, packages[2], &c);
        REQUIRE(Representative(*load.target, packages[3], &d) != nullptr);
        REQUIRE(a != nullptr && bAsset != nullptr && cAsset != nullptr);
        CHECK(a->hard.Handle() == b);
        REQUIRE(a->hardList.Size() == 1);
        CHECK(a->hardList[0] == c);
        CHECK(bAsset->hard.Handle() == d);
        CHECK(cAsset->hard.Handle() == d);
    }

    // 한 묶음.
    {
        AssetEnv load;
        REQUIRE(load.Ready());
        Array<uint8> files[4] = {
            Copy(load.allocator, bytes[0].View()), Copy(load.allocator, bytes[1].View()),
            Copy(load.allocator, bytes[2].View()), Copy(load.allocator, bytes[3].View())};
        RootRef<Object> roots[4];
        ObjectHandle handles[4];
        REQUIRE(LoadAll(load, *load.target, files, PackageKind::Cooked, nullptr, roots, handles));
        ObjectHandle d{};
        REQUIRE(Representative(*load.target, handles[3], &d) != nullptr);
        CHECK(Representative(*load.target, handles[1])->hard.Handle() == d);
        CHECK(Representative(*load.target, handles[2])->hard.Handle() == d);
    }

    // D 없이 B: MissingDependency(related D).
    {
        AssetEnv load;
        REQUIRE(load.Ready());
        Array<uint8> files[1] = {Copy(load.allocator, bytes[1].View())};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        AssetDiagnostic failure;
        CHECK(!LoadAll(load, *load.target, files, PackageKind::Cooked, nullptr, roots, handles,
                       &failure));
        CHECK(failure.code == AssetDiagnosticCode::MissingDependency);
        CHECK(failure.related == FixedId(413));
    }
}

// 자기 참조: 대표 에셋·하위 객체로의 Hard, 자기 패키지 Soft는 import·의존을 만들지
// 않고(ObjectImports· Dependencies 섹션 생략) 왕복·재저장 byte가 같다.
TEST(AssetPackage_SelfReferencesMakeNoDependency) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    Chain chain;
    REQUIRE(MakeChain(source, chain, 1, 420));
    const ObjectHandle body = Make<TestPart>(source, TestPartType(), chain.assets[0], "Body");
    REQUIRE(!body.IsNull());
    REQUIRE(Link(source, chain.assets[0], chain.assets[0]));
    REQUIRE(AddListed(source, chain.assets[0], body));
    REQUIRE(AddListed(source, chain.assets[0], chain.assets[0]));
    REQUIRE(Pointer<TestAsset>(source, chain.assets[0])->soft.TryAssign(FixedId(420), "Body"));
    REQUIRE(source.SetReference(source.OwnerAccess(), body,
                                Pointer<TestPart>(source, body)->sibling, body));
    const Array<uint8> bytes =
        Write(env.allocator, source, chain.packages[0].Handle(), CookedOptions());
    REQUIRE(!bytes.IsEmpty());
    auto contents = ReadPackage(env.allocator, bytes.View(), PackageReadOptions{});
    REQUIRE(contents);
    CHECK(contents.Value().imports.IsEmpty());
    CHECK(contents.Value().dependencies.IsEmpty());
    usize row = 0;
    uint64 offset = 0;
    uint64 size = 0;
    CHECK(!FindSection(bytes.View(), PackageSectionKind::ObjectImports, row, offset, size));
    CHECK(!FindSection(bytes.View(), PackageSectionKind::Dependencies, row, offset, size));

    Array<uint8> files[1] = {Copy(env.allocator, bytes.View())};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    REQUIRE(LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots, handles));
    ObjectHandle self{};
    TestAsset* asset = Representative(*env.target, handles[0], &self);
    auto loadedBody = env.target->FindObject(env.target->OwnerAccess(), handles[0], "Body");
    REQUIRE(asset != nullptr && loadedBody);
    CHECK(asset->hard.Handle() == self);
    REQUIRE(asset->hardList.Size() == 2);
    CHECK(asset->hardList[0] == loadedBody.Value());
    CHECK(asset->hardList[1] == self);
    CHECK(asset->soft.asset == FixedId(420));
    CHECK(Pointer<TestPart>(*env.target, loadedBody.Value())->sibling.Handle() ==
          loadedBody.Value());
    Array<uint8> again = Write(env.allocator, *env.target, handles[0], CookedOptions());
    CHECK(SameBytes(again.View(), bytes.View()));
}

// 실패 전파: 순환 묶음의 한 패키지가 import 타입 불일치면 묶음 전체가 실패하고 아무 객체도 공개되지
// 않는다. 묶음 밖 의존이 실패한(로드되지 않은) 패키지를 참조하면 MissingDependency다.
TEST(AssetPackage_FailurePropagatesThroughBatch) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    Chain chain;
    REQUIRE(MakeChain(source, chain, 2, 430));
    REQUIRE(Link(source, chain.assets[0], chain.assets[1]));
    REQUIRE(Link(source, chain.assets[1], chain.assets[0]));
    Array<uint8> a = Write(env.allocator, source, chain.packages[0].Handle(), CookedOptions());
    // 같은 id(431)로 TestAsset 파생이 아닌 대표 에셋(TestRequiredAsset)을 만들어 B'로 쓴다.
    auto other = NewPackage(source, source.OwnerAccess(), FixedId(431), PackageKind::Cooked);
    REQUIRE(other);
    const ObjectHandle wrong =
        Make<TestRequiredAsset>(source, TestRequiredType(), other.Value().Handle(), "");
    REQUIRE(source.SetReference(source.OwnerAccess(), wrong,
                                Pointer<TestRequiredAsset>(source, wrong)->target,
                                chain.assets[0]));
    Array<uint8> b = Write(env.allocator, source, other.Value().Handle(), CookedOptions());
    REQUIRE(!a.IsEmpty() && !b.IsEmpty());
    {
        Array<uint8> files[2] = {Copy(env.allocator, b.View()), Copy(env.allocator, a.View())};
        RootRef<Object> roots[2];
        ObjectHandle handles[2];
        AssetDiagnostic failure;
        CHECK(!LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots, handles,
                       &failure));
        CHECK(failure.code == AssetDiagnosticCode::TypeMismatch);
        CHECK(env.target->Stats().liveObjects == 0);
    }

    // 손상된 B는 Prepare에서 실패하고, 그것 없이 A만으로는 MissingDependency다.
    {
        Array<uint8> broken = Copy(env.allocator, b.View());
        broken[0] = 'X';
        PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                               LoadOptions(PackageKind::Cooked));
        AssetDiagnostic failure;
        CHECK(!state.Prepare(Move(broken), &failure));
        CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
        Array<uint8> files[1] = {Copy(env.allocator, a.View())};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        AssetDiagnostic missing;
        CHECK(!LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots, handles,
                       &missing));
        CHECK(missing.code == AssetDiagnosticCode::MissingDependency);
        CHECK(missing.related == FixedId(431));
        CHECK(env.target->Stats().liveObjects == 0);
    }
}

namespace {

// 복합 에셋 장면: GameDataAsset(Body 셋 — GameComponent 사슬) → 다른 패키지 둘(Hard 배열),
// 하위 객체 material → 다른 패키지, Soft fallback, weak focus(같은 패키지 하위 객체), 커브, bulk,
// struct 배열(Soft 멤버), Map, 검증 hook struct.
bool BuildGameData(AssetEnv& env, RootRef<Package>& game, RootRef<Package> (&others)[2],
                   TestBulkSource& bulk, Array<uint8>& table) noexcept {
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    ObjectHandle otherAssets[2] = {};
    for (usize index = 0; index < 2; ++index) {
        auto package = NewPackage(registry, access, FixedId(441 + index), PackageKind::Cooked);
        if (!package) {
            return false;
        }

        others[index] = Move(package.Value());
        otherAssets[index] = Make<TestAsset>(registry, TestAssetType(), others[index].Handle(), "");
    }

    auto package = NewPackage(registry, access, FixedId(440), PackageKind::Cooked);
    if (!package) {
        return false;
    }

    game = Move(package.Value());
    const ObjectHandle asset =
        Make<GameDataAsset>(registry, IdOf<GameDataAsset>(), game.Handle(), "");
    ObjectHandle components[3] = {};
    const StringView names[3] = {"Alpha", "Beta", "Gamma"};
    for (usize index = 0; index < 3; ++index) {
        components[index] =
            Make<GameComponent>(registry, IdOf<GameComponent>(), asset, names[index]);
        if (components[index].IsNull()) {
            return false;
        }
    }

    FillCompressible(table, 9000, 5);
    for (usize index = 0; index < 3; ++index) {
        const ObjectHandle component = components[index];
        if (!WithBorrow<GameComponent>(registry, component, [&](GameComponent& value) {
                value.slot = static_cast<int32>(index);
                return registry
                           .SetReference(access, component, value.next, components[(index + 1) % 3])
                           .IsOk() &&
                       registry
                           .SetReference(access, component, value.material, otherAssets[index % 2])
                           .IsOk() &&
                       value.response.TryAddKey(0.0f, static_cast<float>(index)).IsOk();
            })) {
            return false;
        }
    }

    return WithBorrow<GameDataAsset>(registry, asset, [&](GameDataAsset& value) {
        value.focus = WeakObjectRef<GameComponent>(components[1]);
        value.table = bulk.Add(3, table.View());
        value.window = FixtureRange{-2, 8};
        bool ok = Label(value.title, "game", 1) &&
                  value.fallback.TryAssign(FixedId(443), "Missing").IsOk() &&
                  value.byLevel.Insert(1u, 1.5f).IsOk() && value.byLevel.Insert(9u, -4.0f).IsOk() &&
                  value.balance.TryAddKey(0.0f, 1.0f).IsOk() &&
                  value.balance.TryAddKey(10.0f, 3.0f).IsOk();
        for (usize index = 0; ok && index < 2; ++index) {
            ok =
                registry.AddReference(access, asset, value.dependencies, otherAssets[index]).IsOk();
        }

        for (usize index = 0; ok && index < 3; ++index) {
            ok = registry.AddReference(access, asset, value.components, components[index]).IsOk();
            auto entry = value.entries.TryEmplaceBack(registry.ObjectAllocator());
            ok = ok && entry && Label(entry.Value()->id, "entry", static_cast<uint32>(index)) &&
                 entry.Value()->icon.TryAssign(FixedId(441), StringView()).IsOk();
            if (ok) {
                entry.Value()->cost = static_cast<int32>(index * 10);
                entry.Value()->weight = static_cast<float>(index) * 0.5f;
            }
        }

        return ok;
    });
}

} // namespace

// 복합 에셋: 하위 객체 여러 개의 순환(Alpha → Beta → Gamma → Alpha), 다른 에셋 Hard 배열과 하위
// 객체의 다른 패키지 참조, Soft·weak·커브·bulk·struct 배열·Map·hook struct가 함께 왕복하고 다시
// 쓰면 byte가 같다.
TEST(AssetPackage_CompositeGameDataRoundTrip) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RootRef<Package> game;
    RootRef<Package> others[2];
    TestBulkSource bulk;
    Array<uint8> table(env.allocator);
    REQUIRE(BuildGameData(env, game, others, bulk, table));
    PackageWriteOptions options = CookedOptions();
    options.bulkSource = &bulk;
    options.bulkCodec = BulkCodec::Zstd;
    const Array<uint8> original = Write(env.allocator, *env.source, game.Handle(), options);
    REQUIRE(!original.IsEmpty());
    auto contents = ReadPackage(env.allocator, original.View(), PackageReadOptions{});
    REQUIRE(contents);
    // 의존: 441·442 Hard(배열·하위 객체 material), 443 Soft, 441 Soft(entries.icon)는 Hard와 같은
    // 에셋이라 Hard·Soft 두 항목.
    REQUIRE(contents.Value().dependencies.Size() == 4);
    CHECK(contents.Value().exports.Size() == 4);

    TestResolver resolver;
    RootRef<Object> keep[2];
    for (usize index = 0; index < 2; ++index) {
        Array<uint8> files[1] = {
            Write(env.allocator, *env.source, others[index].Handle(), CookedOptions())};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        REQUIRE(LoadAll(env, *env.target, files, PackageKind::Cooked, &resolver, roots, handles));
        keep[index] = Move(roots[0]);
        resolver.Add(FixedId(441 + index), handles[0]);
    }

    Array<uint8> files[1] = {Copy(env.allocator, original.View())};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    REQUIRE(LoadAll(env, *env.target, files, PackageKind::Cooked, &resolver, roots, handles));
    ObjectRegistry& target = *env.target;
    const ObjectAccessContext access = target.OwnerAccess();
    auto assetHandle = target.FindObject(access, handles[0], StringView());
    REQUIRE(assetHandle);
    GameDataAsset* asset = Pointer<GameDataAsset>(target, assetHandle.Value());
    REQUIRE(asset != nullptr);
    CHECK(LabelIs(asset->title.View(), "game", 1));
    REQUIRE(asset->components.Size() == 3);
    REQUIRE(asset->dependencies.Size() == 2);
    CHECK(asset->focus.Handle() == asset->components[1]);
    for (usize index = 0; index < 3; ++index) {
        GameComponent* component = Pointer<GameComponent>(target, asset->components[index]);
        REQUIRE(component != nullptr);
        CHECK(component->slot == static_cast<int32>(index));
        CHECK(component->next.Handle() == asset->components[(index + 1) % 3]);
        CHECK(component->material.Handle() == asset->dependencies[index % 2]);
        CHECK(component->response.KeyCount() == 1);
    }

    CHECK(asset->fallback.asset == FixedId(443));
    CHECK(asset->fallback.objectPath.View() == StringView("Missing"));
    REQUIRE(asset->entries.Size() == 3);
    CHECK(asset->entries[2].cost == 20);
    CHECK(asset->entries[1].icon.asset == FixedId(441));
    CHECK(asset->byLevel.Count() == 2);
    CHECK(asset->balance.KeyCount() == 2);
    CHECK(asset->window.low == -2 && asset->window.high == 8);
    Package* package = Pointer<Package>(target, handles[0]);
    REQUIRE(package != nullptr);
    auto bytes = package->BulkBytes(asset->table);
    REQUIRE(bytes);
    CHECK(SameBytes(bytes.Value(), table.View()));
    PackageBulkReader reader(target, handles[0]);
    options.bulkSource = &reader;
    Array<uint8> again = Write(env.allocator, target, handles[0], options);
    CHECK(SameBytes(again.View(), original.View()));
}

namespace {

struct PrepareJob {
    PackageLoadState* state = nullptr;
    Array<uint8>* bytes = nullptr;
    AssetDiagnostic failure{};
    bool prepared = false;
};

void RunPrepare(void* context) noexcept {
    PrepareJob& job = *static_cast<PrepareJob*>(context);
    job.prepared = job.state->Prepare(Move(*job.bytes), &job.failure).IsOk();
}

struct WriteJob {
    AssetEnv* env = nullptr;
    ObjectHandle package{};
    ErrorCode error = ErrorCode::None;
};

void RunWrite(void* context) noexcept {
    WriteJob& job = *static_cast<WriteJob*>(context);
    MemoryPackageSink sink(job.env->allocator);
    auto written = WritePackage(job.env->allocator, *job.env->source,
                                job.env->source->OwnerAccess(), job.package, CookedOptions(), sink);
    job.error = written ? ErrorCode::None : written.Error();
}

} // namespace

// 스레드 규칙(AS 11.2절): Prepare는 소유 스레드가 아닌 스레드에서 해도 되고(검증·압축 해제·그래프
// 1단계), 그 뒤 소유 스레드가 InstantiatePackages·Commit을 한다. 쓰기는 Registry 소유 스레드
// 전용이다(다른 스레드는 WrongThread). 순서는 Join으로 정한다(시간에 기대지 않는다).
TEST(AssetPackage_ThreadRules) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Chain chain;
    REQUIRE(MakeChain(*env.source, chain, 1, 450));
    Array<uint8> bytes =
        Write(env.allocator, *env.source, chain.packages[0].Handle(), CookedOptions());
    REQUIRE(!bytes.IsEmpty());

    PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                           LoadOptions(PackageKind::Cooked));
    PrepareJob prepare{&state, &bytes};
    {
        platform::Thread worker;
        REQUIRE(worker.Start(&RunPrepare, &prepare) == platform::ThreadStatus::Success);
        REQUIRE(worker.Join() == platform::ThreadStatus::Success);
    }

    REQUIRE(prepare.prepared);
    ConstructionContext transaction(*env.target, env.target->OwnerAccess());
    PackageLoadState* states[] = {&state};
    ObjectHandle handles[1];
    REQUIRE(InstantiatePackages(transaction, states, nullptr, handles));
    RootRef<Object> roots[1];
    REQUIRE(transaction.Commit(handles, roots));
    CHECK(Representative(*env.target, handles[0])->value == 450);

    WriteJob write{&env, chain.packages[0].Handle()};
    {
        platform::Thread worker;
        REQUIRE(worker.Start(&RunWrite, &write) == platform::ThreadStatus::Success);
        REQUIRE(worker.Join() == platform::ThreadStatus::Success);
    }

    CHECK(write.error == ErrorCode::WrongThread);
}
} // namespace package_graph_tests_file
