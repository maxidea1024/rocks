#pragma once

#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "MemoryFileSystem.h"

#include <AssetDatabase/EditorPackageSet.h>
#include <AssetRuntime/Package.h>
#include <AssetRuntime/PackageRangeReader.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>

// 에디터 패키지 집합 시험(열기·저장)의 공용 장면이다(Docs/Design/AssetSystem 7.1절 15 —
// A2c2a·A2c2b): 시험 원본 패키지 묶음, 메모리 프로젝트와 색인, 집합 설정, 실패한 할당의 tag를
// 기록하는 allocator.
namespace rocks::editor_set_test {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;

constexpr uint64 BaseId = 2800;
constexpr uint64 DerivedId = 2801;
constexpr uint64 HeroId = 2802;
constexpr uint64 VillainId = 2803;
constexpr uint64 LeafId = 2804;
constexpr uint64 CycleAId = 2805;
constexpr uint64 CycleBId = 2806;
constexpr uint64 HubId = 2807;
// Hero의 Soft 대상(열지 않는다 — 색인에도 없다)과 어디에도 없는 id다.
constexpr uint64 SoftOnlyId = 2898;
constexpr uint64 NowhereId = 2899;
constexpr int32 HeroValue = 7;
constexpr usize PixelBytes = 3000;
// 더 새 엔진 버전(TestEngineVersion보다 주 버전이 크다 — NewerVersion)이다.
constexpr EngineVersion NewerEngine{9, 0, 0};
const PackageCustomVersion UnknownCustomVersions[] = {{Guid{0x1DE, 0x99}, 1}};

// Registry가 실패를 삼키는 유일한 할당(GC 참조 칸 표)의 tag('GCRT' — ObjectRegistryGcInternal.h
// GcReferenceTableTag)다.
constexpr uint32 GcReferenceTableTag = 0x47435254u;

// n번째 할당을 실패시키고 그 tag를 기록한다(NFailAllocator에 실패 tag를 더한 것).
class TaggedFailAllocator final : public Allocator {
public:
    explicit TaggedFailAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    void FailOnAttempt(uint64 attempt) noexcept {
        attempts_ = 0;
        target_ = attempt;
        failed_ = false;
        failedTag_ = 0;
    }

    void DisableFailure() noexcept {
        FailOnAttempt(0);
    }

    bool Failed() const noexcept {
        return failed_;
    }

    uint32 FailedTag() const noexcept {
        return failedTag_;
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        if (target_ != 0 && ++attempts_ == target_) {
            failed_ = true;
            failedTag_ = tag;
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

private:
    Allocator& inner_;
    uint64 attempts_ = 0;
    uint64 target_ = 0;
    bool failed_ = false;
    uint32 failedTag_ = 0;
};

template <class T>
inline ObjectHandle MakeAsset(ObjectRegistry& registry, uint64 id,
                              RootRef<Package>& package) noexcept {
    auto made = NewPackage(registry, registry.OwnerAccess(), FixedId(id), PackageKind::Editor);
    if (!made) {
        return ObjectHandle{};
    }

    package = Move(made.Value());
    return Make<T>(registry, IdOf<T>(), package.Handle(), "");
}

// fields 하나만 든 Record다(손상·다른 스키마 ImportRecord).
inline Array<uint8> OneFieldRecord(Allocator& allocator, Guid schema) noexcept {
    const uint8 value[4] = {1, 2, 3, 4};
    RecordBuilder builder(allocator, schema, 1);
    if (!builder.AddField(Guid{0, 9}, WireType::U32, value)) {
        return Array<uint8>(allocator);
    }

    auto built = builder.Build();
    return built ? Move(built.Value()) : Array<uint8>(allocator);
}

// 시험 원본 패키지다(env.source). Base(BaseGameAsset), Derived(DerivedGameAsset), Hero(TestAsset —
// Hard → Base, EditorOnly → Derived, Soft → SoftOnly, value, pixels bulk), Villain(TestAsset —
// Hard· typed → Hero), Leaf(TestAsset 참조 없음), CycleA ↔ CycleB(서로 Hard), Hub(TestAsset —
// hardList → Base·Derived·Leaf·CycleA·Villain, 닫힘 8개).
struct Sources {
    explicit Sources(Allocator& allocator) noexcept
        : pixels(allocator), base(allocator), derived(allocator), hero(allocator),
          villain(allocator), leaf(allocator), cycleA(allocator), cycleB(allocator),
          hub(allocator) {
    }

    RootRef<Package> packages[8];
    Array<uint8> pixels;
    TestBulkSource bulk;
    Array<uint8> base;
    Array<uint8> derived;
    Array<uint8> hero;
    Array<uint8> villain;
    Array<uint8> leaf;
    Array<uint8> cycleA;
    Array<uint8> cycleB;
    Array<uint8> hub;
};

inline bool MakeSources(AssetEnv& env, Sources& out) noexcept {
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    const ObjectHandle base = MakeAsset<BaseGameAsset>(registry, BaseId, out.packages[0]);
    const ObjectHandle derived = MakeAsset<DerivedGameAsset>(registry, DerivedId, out.packages[1]);
    const ObjectHandle hero = MakeAsset<TestAsset>(registry, HeroId, out.packages[2]);
    const ObjectHandle villain = MakeAsset<TestAsset>(registry, VillainId, out.packages[3]);
    const ObjectHandle leaf = MakeAsset<TestAsset>(registry, LeafId, out.packages[4]);
    const ObjectHandle cycleA = MakeAsset<TestAsset>(registry, CycleAId, out.packages[5]);
    const ObjectHandle cycleB = MakeAsset<TestAsset>(registry, CycleBId, out.packages[6]);
    const ObjectHandle hub = MakeAsset<TestAsset>(registry, HubId, out.packages[7]);
    TestAsset* heroValue = Pointer<TestAsset>(registry, hero);
    TestAsset* villainValue = Pointer<TestAsset>(registry, villain);
    TestAsset* cycleAValue = Pointer<TestAsset>(registry, cycleA);
    TestAsset* cycleBValue = Pointer<TestAsset>(registry, cycleB);
    if (base.IsNull() || derived.IsNull() || leaf.IsNull() || heroValue == nullptr ||
        villainValue == nullptr || cycleAValue == nullptr || cycleBValue == nullptr ||
        hub.IsNull() || !out.pixels.TryResize(PixelBytes)) {
        return false;
    }

    for (usize index = 0; index < PixelBytes; ++index) {
        out.pixels[index] = static_cast<uint8>(index * 7 + 3);
    }

    heroValue->value = HeroValue;
    heroValue->pixels = out.bulk.Add(1, out.pixels.View());
    if (!registry.SetReference(access, hero, heroValue->hard, base) ||
        !registry.SetReference(access, hero, heroValue->editorRef, derived) ||
        !heroValue->soft.TryAssign(FixedId(SoftOnlyId), "") ||
        !registry.SetReference(access, villain, villainValue->hard, hero) ||
        !registry.SetReference(access, villain, villainValue->typed, hero) ||
        !registry.SetReference(access, cycleA, cycleAValue->hard, cycleB) ||
        !registry.SetReference(access, cycleB, cycleBValue->hard, cycleA)) {
        return false;
    }

    const ObjectHandle hubTargets[] = {base, derived, leaf, cycleA, villain};
    for (ObjectHandle target : hubTargets) {
        if (!WithBorrow<TestAsset>(registry, hub, [&](TestAsset& held) noexcept {
                return static_cast<bool>(registry.AddReference(access, hub, held.hardList, target));
            })) {
            return false;
        }
    }

    PackageWriteOptions heroOptions = EditorOptions();
    heroOptions.bulkSource = &out.bulk;
    out.base = Write(env.allocator, registry, out.packages[0].Handle(), EditorOptions());
    out.derived = Write(env.allocator, registry, out.packages[1].Handle(), EditorOptions());
    out.hero = Write(env.allocator, registry, out.packages[2].Handle(), heroOptions);
    out.villain = Write(env.allocator, registry, out.packages[3].Handle(), EditorOptions());
    out.leaf = Write(env.allocator, registry, out.packages[4].Handle(), EditorOptions());
    out.cycleA = Write(env.allocator, registry, out.packages[5].Handle(), EditorOptions());
    out.cycleB = Write(env.allocator, registry, out.packages[6].Handle(), EditorOptions());
    out.hub = Write(env.allocator, registry, out.packages[7].Handle(), EditorOptions());
    const Array<uint8>* const all[] = {&out.base, &out.derived, &out.hero,   &out.villain,
                                       &out.leaf, &out.cycleA,  &out.cycleB, &out.hub};
    for (const Array<uint8>* bytes : all) {
        if (bytes->IsEmpty()) {
            return false;
        }
    }

    return true;
}

// 메모리 프로젝트다(Content 파일 + 색인).
struct Project {
    // root는 프로젝트 루트다(기본은 디스크에 없는 메모리 루트 — 저장 시험은 잠금 파일을 둘 디스크
    // 임시 디렉터리를 준다, Content 파일은 언제나 메모리다).
    explicit Project(Allocator& allocator, StringView root = MemoryRoot) noexcept
        : layout(allocator), ready(static_cast<bool>(layout.Initialize(root))),
          files(allocator, layout.ContentDirectory()), index(allocator, layout) {
    }

    bool Rebuild() noexcept {
        return static_cast<bool>(index.Rebuild(files, PackageReadOptions{}));
    }

    ProjectLayout layout;
    bool ready;
    MemoryFileSystem files;
    AssetIndex index;
};

inline EditorPackageSetConfig Config(AssetEnv& env, Project& project) noexcept {
    EditorPackageSetConfig config;
    config.types = &env.types;
    config.files = &project.files;
    config.layout = &project.layout;
    config.index = &project.index;
    config.read.engineVersion = TestEngineVersion;
    config.registryContext = ContextId{0x4550534554ull, 1};
    config.registryOptions.names = &env.names;
    return config;
}

// bytes를 집합과 같은 옵션(Defer·에디터·TestEngineVersion)으로 직접 Prepare한 실패 진단이다(손상·
// 버전 모양의 기대값 — 집합이 Prepare 진단을 그대로 넘기는지 본다).
inline AssetDiagnostic DirectPrepareFailure(AssetEnv& env, Span<const uint8> bytes) noexcept {
    PackageLoadOptions options;
    options.bulk = PackageBulkMode::Defer;
    options.read.expectedKind = PackageKind::Editor;
    options.read.engineVersion = TestEngineVersion;
    PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                           options);
    MemoryPackageRangeReader reader(bytes);
    AssetDiagnostic failure;
    CHECK(!state.Prepare(reader, &failure));
    return failure;
}

inline TestAsset* AssetOf(EditorPackageSet& set, ObjectHandle package) noexcept {
    auto representative = set.Registry().FindChild(set.Access(), package, "");
    return representative ? Pointer<TestAsset>(set.Registry(), representative.Value()) : nullptr;
}

inline ObjectHandle RepresentativeOf(EditorPackageSet& set, uint64 id) noexcept {
    const EditorPackageInfo* info = set.Find(FixedId(id));
    if (info == nullptr) {
        return ObjectHandle{};
    }

    auto representative = set.Registry().FindChild(set.Access(), info->package, "");
    return representative ? representative.Value() : ObjectHandle{};
}

} // namespace rocks::editor_set_test
