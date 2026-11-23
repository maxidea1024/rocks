#include "AssetFixtureValues.h"

#include <AssetRuntime/AssetTypeHooks.h>
#include <TestSupport/NFailAllocator.h>

// 에셋 타입 hook 표 시험이다(Docs/Design/AssetSystem 7.1절 15 — A2c1): base 사슬의 가장
// 가까운 hook 하나, CallBase로 부모 hook 부르기(부르지 않으면 부모 태그가 꺼진다), 종류마다 따로
// 찾기, hook· 타입 조회 오류 전달, 등록 수명(handle 소멸·Release·이동), 태그·의존 sink의
// 검사·합치기·정렬·할당 실패.
using namespace rocks;
namespace asset_type_hook_tests_file {

using namespace rocks::asset_test;

namespace {

// hook이 부른 순서를 적는다(시험 전용 전역 — 한 시험 안에서만 쓴다).
uint32 calls[4] = {};
bool failBase = false;
// BaseTags가 받은 저장할 패키지 id다(CallBase가 넘기는지 본다).
AssetId baseAsset{};

Result<void> BaseTags(const AssetTypeHookContext& context, AssetTagSink& sink) noexcept {
    ++calls[0];
    baseAsset = context.asset;
    if (failBase) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (auto base = context.CallBaseTags(sink); !base) {
        return base;
    }

    return sink.Add("Kind", "Base");
}

Result<void> DerivedTagsWithBase(const AssetTypeHookContext& context, AssetTagSink& sink) noexcept {
    ++calls[1];
    if (auto base = context.CallBaseTags(sink); !base) {
        return base;
    }

    return sink.Add("Kind", "Derived");
}

Result<void> DerivedTagsAlone(const AssetTypeHookContext&, AssetTagSink& sink) noexcept {
    ++calls[2];
    return sink.Add("Kind", "Derived");
}

Result<void> BaseDependencies(const AssetTypeHookContext& context,
                              AssetDependencySink& sink) noexcept {
    ++calls[3];
    if (failBase) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (auto base = context.CallBaseBuildDependencies(sink); !base) {
        return base;
    }

    return sink.Add(FixedId(2600));
}

Result<void> DerivedDependencies(const AssetTypeHookContext& context,
                                 AssetDependencySink& sink) noexcept {
    if (auto base = context.CallBaseBuildDependencies(sink); !base) {
        return base;
    }

    return sink.Add(FixedId(2601));
}

void ResetCalls() noexcept {
    for (uint32& count : calls) {
        count = 0;
    }

    failBase = false;
}

bool HasTag(const AssetTagSink& sink, StringView key, StringView value) noexcept {
    for (const PackageTag& tag : sink.Tags()) {
        if (tag.key.View() == key && tag.value.View() == value) {
            return true;
        }
    }

    return false;
}

} // namespace

// 가장 가까운 hook과 CallBase(7.1절 15): Base에만 hook이 있으면 Derived 객체에 Base hook이 불린다.
// Derived hook이 CallBase하면 두 태그, 하지 않으면 Derived 태그만이다. 종류마다 따로 찾는다(Derived
// 항목의 Build hook이 없으면 Base의 것). hook이 없는 타입(TestAsset)은 아무것도 하지 않고 성공한다.
// 최상위 hook의 CallBase는 무동작이다.
TEST(AssetTypeHooks_NearestAndCallBase) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ResetCalls();
    AssetTypeHookTable table(env.allocator, env.types);
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    const TypeId base = IdOf<BaseGameAsset>();
    const TypeId derived = IdOf<DerivedGameAsset>();
    auto baseRegistration = table.Register(base, {&BaseTags, &BaseDependencies});
    REQUIRE(baseRegistration);
    {
        AssetTagSink sink(env.allocator);
        REQUIRE(table.CollectTags(registry, access, ObjectHandle{}, derived, AssetId{}, sink));
        CHECK(calls[0] == 1 && sink.Tags().Size() == 1 && HasTag(sink, "Kind", "Base"));
        AssetDependencySink dependencies(env.allocator);
        REQUIRE(table.CollectBuildDependencies(registry, access, ObjectHandle{}, derived, AssetId{},
                                               dependencies));
        REQUIRE(dependencies.Dependencies().Size() == 1);
        CHECK(dependencies.Dependencies()[0].asset == FixedId(2600));
        CHECK(dependencies.Dependencies()[0].kind == PackageDependencyKind::Build);
    }

    {
        auto derivedRegistration = table.Register(derived, {&DerivedTagsWithBase, nullptr});
        REQUIRE(derivedRegistration);
        AssetTagSink sink(env.allocator);
        // 저장할 패키지 id는 CallBase로 부른 base hook까지 그대로 간다.
        REQUIRE(table.CollectTags(registry, access, ObjectHandle{}, derived, FixedId(2601), sink));
        CHECK(baseAsset == FixedId(2601));
        CHECK(sink.Tags().Size() == 2 && HasTag(sink, "Kind", "Base") &&
              HasTag(sink, "Kind", "Derived"));
        AssetDependencySink dependencies(env.allocator);
        REQUIRE(table.CollectBuildDependencies(registry, access, ObjectHandle{}, derived, AssetId{},
                                               dependencies));
        CHECK(dependencies.Dependencies().Size() == 1);
    }

    {
        auto alone = table.Register(derived, {&DerivedTagsAlone, &DerivedDependencies});
        REQUIRE(alone);
        AssetTagSink sink(env.allocator);
        REQUIRE(table.CollectTags(registry, access, ObjectHandle{}, derived, AssetId{}, sink));
        CHECK(sink.Tags().Size() == 1 && HasTag(sink, "Kind", "Derived"));
        AssetDependencySink dependencies(env.allocator);
        REQUIRE(table.CollectBuildDependencies(registry, access, ObjectHandle{}, derived, AssetId{},
                                               dependencies));
        REQUIRE(dependencies.Dependencies().Size() == 2);
        CHECK(dependencies.Dependencies()[0].asset == FixedId(2600));
        CHECK(dependencies.Dependencies()[1].asset == FixedId(2601));
    }

    AssetTagSink none(env.allocator);
    REQUIRE(table.CollectTags(registry, access, ObjectHandle{}, TestAssetType(), AssetId{}, none));
    AssetDependencySink noDependencies(env.allocator);
    REQUIRE(table.CollectBuildDependencies(registry, access, ObjectHandle{}, TestAssetType(),
                                           AssetId{}, noDependencies));
    CHECK(none.Tags().IsEmpty() && noDependencies.Dependencies().IsEmpty());
}

// 오류(7.1절 15): 0 타입·hook 둘 다 null InvalidArgument, 같은 타입 두 번 AlreadyExists. hook의
// 오류는 그대로 오고(직접·CallBase 거쳐), 미등록 타입은 조회 오류(NotFound)다. 타입을 등록 해제한
// 뒤의 CallBase도 그 조회 오류다.
TEST(AssetTypeHooks_Errors) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ResetCalls();
    AssetTypeHookTable table(env.allocator, env.types);
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    const TypeId base = IdOf<BaseGameAsset>();
    const TypeId derived = IdOf<DerivedGameAsset>();
    CHECK(table.Register(TypeId{}, {&BaseTags, nullptr}).Error() == ErrorCode::InvalidArgument);
    CHECK(table.Register(base, {}).Error() == ErrorCode::InvalidArgument);
    auto baseRegistration = table.Register(base, {&BaseTags, &BaseDependencies});
    REQUIRE(baseRegistration);
    CHECK(table.Register(base, {nullptr, &BaseDependencies}).Error() == ErrorCode::AlreadyExists);
    auto derivedRegistration =
        table.Register(derived, {&DerivedTagsWithBase, &DerivedDependencies});
    REQUIRE(derivedRegistration);
    failBase = true;
    AssetTagSink sink(env.allocator);
    CHECK(table.CollectTags(registry, access, ObjectHandle{}, derived, AssetId{}, sink).Error() ==
          ErrorCode::Busy);
    CHECK(table.CollectTags(registry, access, ObjectHandle{}, base, AssetId{}, sink).Error() ==
          ErrorCode::Busy);
    AssetDependencySink dependencies(env.allocator);
    CHECK(table
              .CollectBuildDependencies(registry, access, ObjectHandle{}, derived, AssetId{},
                                        dependencies)
              .Error() == ErrorCode::Busy);
    failBase = false;
    const TypeId unknown = Guid{0x7E57, 0x0001};
    CHECK(table.CollectTags(registry, access, ObjectHandle{}, unknown, AssetId{}, sink).Error() ==
          ErrorCode::NotFound);
    CHECK(table
              .CollectBuildDependencies(registry, access, ObjectHandle{}, unknown, AssetId{},
                                        dependencies)
              .Error() == ErrorCode::NotFound);
    // 미등록 타입을 hookType으로 가진 문맥의 CallBase(hook 안에서 타입이 사라진 경우).
    const AssetTypeHookContext orphan{registry, access, ObjectHandle{}, unknown, table, env.types};
    CHECK(orphan.CallBaseTags(sink).Error() == ErrorCode::NotFound);
    CHECK(orphan.CallBaseBuildDependencies(dependencies).Error() == ErrorCode::NotFound);
}

// 등록 수명(7.1절 15): handle을 Release하거나 소멸시키면 항목이 빠진다. 이동한 handle은 비활성이고
// 이동 대입은 앞 등록을 놓는다. 자기 대입은 무동작이다.
TEST(AssetTypeHooks_RegistrationLifetime) {
    AssetEnv env;
    REQUIRE(env.Ready());
    AssetTypeHookTable table(env.allocator, env.types);
    const TypeId base = IdOf<BaseGameAsset>();
    const TypeId derived = IdOf<DerivedGameAsset>();
    {
        auto scoped = table.Register(base, {&BaseTags, nullptr});
        REQUIRE(scoped);
        CHECK(table.Count() == 1 && scoped.Value().IsActive());
    }

    CHECK(table.Count() == 0);
    auto first = table.Register(base, {&BaseTags, nullptr});
    REQUIRE(first);
    AssetTypeHookRegistration moved(Move(first.Value()));
    CHECK(!first.Value().IsActive() && moved.IsActive() && table.Count() == 1);
    auto second = table.Register(derived, {&DerivedTagsAlone, nullptr});
    REQUIRE(second);
    moved = Move(second.Value());
    CHECK(table.Count() == 1 && moved.IsActive());
    AssetTypeHookRegistration& self = moved;
    moved = Move(self);
    CHECK(moved.IsActive() && table.Count() == 1);
    moved.Release();
    CHECK(!moved.IsActive() && table.Count() == 0);
    moved.Release();
    CHECK(table.Count() == 0);
}

// sink(7.1절 13·15): 태그는 PackageTags 규칙으로 검사하고(빈 키·긴 키·잘못된 UTF-8 InvalidArgument)
// 같은 (키, 값)을 합치며 Write는 정렬된 Tags byte(비면 빈 byte)다. 의존은 null id를 거부하고
// AssetId 순·유일이다. 모든 할당 지점의 실패는 그 오류다.
TEST(AssetTypeHooks_Sinks) {
    SystemAllocator allocator;
    AssetTagSink sink(allocator);
    auto empty = sink.Write(allocator);
    CHECK(empty && empty.Value().IsEmpty());
    CHECK(sink.Add("", "x").Error() == ErrorCode::InvalidArgument);
    CHECK(sink.Add("K\xff", "x").Error() == ErrorCode::InvalidArgument);
    REQUIRE(sink.Add("B", "2"));
    REQUIRE(sink.Add("A", "1"));
    REQUIRE(sink.Add("B", "2"));
    CHECK(sink.Tags().Size() == 2);
    // 목록 값(같은 키 여러 값)은 값 순이다.
    REQUIRE(sink.Add("B", "3"));
    REQUIRE(sink.Add("B", "1"));
    REQUIRE(sink.Tags().Size() == 4);
    CHECK(sink.Tags()[1].value.View() == "1" && sink.Tags()[3].value.View() == "3");
    REQUIRE(sink.Add("B", "1"));
    CHECK(sink.Tags().Size() == 4);
    auto written = sink.Write(allocator);
    REQUIRE(written);
    auto read = ReadPackageTags(allocator, written.Value().View());
    REQUIRE(read && read.Value().Size() == 4);
    CHECK(read.Value()[0].key.View() == "A");
    AssetDependencySink dependencies(allocator);
    CHECK(dependencies.Add(AssetId{}).Error() == ErrorCode::InvalidArgument);
    REQUIRE(dependencies.Add(FixedId(3)));
    REQUIRE(dependencies.Add(FixedId(1)));
    REQUIRE(dependencies.Add(FixedId(2)));
    REQUIRE(dependencies.Add(FixedId(1)));
    REQUIRE(dependencies.Dependencies().Size() == 3);
    CHECK(dependencies.Dependencies()[0].asset == FixedId(1));
    CHECK(dependencies.Dependencies()[2].asset == FixedId(3));
    test::NFailAllocator failing(allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetTagSink tags(failing);
        AssetDependencySink deps(failing);
        failing.FailOnAttempt(attempt);
        Result<void> result = tags.Add("Key", "Value");
        if (result) {
            result = tags.Add("Other", "Value");
        }

        if (result) {
            auto bytes = tags.Write(failing);
            result = bytes ? Result<void>::Success() : Result<void>::Failure(bytes.Error());
        }

        if (result) {
            result = deps.Add(FixedId(5));
        }

        if (result) {
            result = deps.Add(FixedId(4));
        }

        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(result);
            break;
        }

        CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 0);
}

// 등록의 할당 실패(7.1절 15): 모든 지점에서 그 오류이고 표는 그대로다.
TEST(AssetTypeHooks_RegisterAllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    test::NFailAllocator failing(env.allocator);
    AssetTypeHookTable table(failing, env.types);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto registration = table.Register(IdOf<BaseGameAsset>(), {&BaseTags, nullptr});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(registration && table.Count() == 1);
            break;
        }

        CHECK(registration.Error() == ErrorCode::OutOfMemory && table.Count() == 0);
        ++failures;
    }

    CHECK(failures > 0);
}

} // namespace asset_type_hook_tests_file
