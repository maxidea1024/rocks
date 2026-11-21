#include "AssetFixtureValues.h"

#include <AssetRuntime/PackageLoad.h>
#include <TestSupport/NFailAllocator.h>
#include <stdio.h>

// import 정책 시험이다(Docs/Design/AssetSystem.ko.md 7.1절 15 — A2c2a, ObjectFoundations 5.4절):
// NullMissing은 누락·형식이 맞지 않는 import를 null로 잇고 미해결 목록에 적는다 — 대상 패키지
// 없음, 대표 에셋 형식이 Hard 의존 expectedType의 파생이 아님, 하위 객체 없음(그 아래 import도
// 미해결), 하위 객체 형식이 선언과 다름. Strict는 지금처럼 실패한다. 인자·할당 실패.
using namespace rocks;
namespace package_import_policy_tests_file {

using namespace rocks::asset_test;

namespace {

constexpr uint64 TargetId = 2700;
constexpr uint64 SourceId = 2701;

// B의 모양이다.
enum class Variant {
    Full,
    Missing,
    RepresentativeType,
    NoPart,
    PartType
};

// env.source에 B(TestAsset — 하위 Part(TestPart) — 그 아래 Inner(TestPart))와 A(TestAsset — Hard →
// B 대표, hardList → B/Part/Inner)를 만들어 A를 쓴다.
Array<uint8> MakeSourceFile(AssetEnv& env) noexcept {
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    auto b = NewPackage(registry, access, FixedId(TargetId), PackageKind::Editor);
    auto a = NewPackage(registry, access, FixedId(SourceId), PackageKind::Editor);
    if (!a || !b) {
        return Array<uint8>(env.allocator);
    }

    const ObjectHandle bAsset = Make<TestAsset>(registry, TestAssetType(), b.Value().Handle(), "");
    const ObjectHandle part = Make<TestPart>(registry, TestPartType(), bAsset, "Part");
    const ObjectHandle inner = Make<TestPart>(registry, TestPartType(), part, "Inner");
    const ObjectHandle aAsset = Make<TestAsset>(registry, TestAssetType(), a.Value().Handle(), "");
    TestAsset* value = Pointer<TestAsset>(registry, aAsset);
    if (inner.IsNull() || value == nullptr ||
        !registry.SetReference(access, aAsset, value->hard, bAsset) ||
        !WithBorrow<TestAsset>(registry, aAsset, [&](TestAsset& held) noexcept {
            return static_cast<bool>(registry.AddReference(access, aAsset, held.hardList, inner));
        })) {
        return Array<uint8>(env.allocator);
    }

    AssetDiagnostic failure;
    Array<uint8> written =
        Write(env.allocator, registry, a.Value().Handle(), EditorOptions(), &failure);
    if (written.IsEmpty()) {
        printf("  write failed: code %u error %u section %u\n", static_cast<unsigned>(failure.code),
               static_cast<unsigned>(failure.error), failure.section);
    }

    return written;
}

// env.target에 B의 모양을 공개한다(Missing은 만들지 않는다).
bool MakeTarget(AssetEnv& env, Variant variant, RootRef<Package>& package) noexcept {
    if (variant == Variant::Missing) {
        return true;
    }

    ObjectRegistry& registry = *env.target;
    auto made =
        NewPackage(registry, registry.OwnerAccess(), FixedId(TargetId), PackageKind::Editor);
    if (!made) {
        return false;
    }

    package = Move(made.Value());
    const ObjectHandle asset =
        variant == Variant::RepresentativeType
            ? Make<BaseGameAsset>(registry, IdOf<BaseGameAsset>(), package.Handle(), "")
            : Make<TestAsset>(registry, TestAssetType(), package.Handle(), "");
    if (asset.IsNull()) {
        return false;
    }

    if (variant == Variant::NoPart) {
        return true;
    }

    if (variant == Variant::PartType) {
        return !Make<TestScratch>(registry, TestScratchType(), asset, "Part").IsNull();
    }

    const ObjectHandle part = Make<TestPart>(registry, TestPartType(), asset, "Part");
    return !part.IsNull() && !Make<TestPart>(registry, TestPartType(), part, "Inner").IsNull();
}

struct Expected {
    Variant variant;
    // 미해결 기록(import 위치·코드)이다(최대 둘).
    usize count;
    uint32 imports[2];
    AssetDiagnosticCode codes[2];
    bool hardNull;
};

} // namespace

// NullMissing(7.1절 15): B의 모양마다 A를 열어 미해결 기록과 null 참조를 본다 — 온전함(기록 없음·
// 참조 이어짐), 없음(에셋 첫 import에 MissingDependency 하나·모두 null), 대표 형식
// 다름(TypeMismatch 하나 — 칸 Dependencies), Part 없음(Part MissingDependency·Inner도 outer
// 미해결로 MissingDependency — Hard는 이어짐), Part 형식 다름(Part TypeMismatch·Inner outer
// 미해결). Strict는 없는 대상에서 MissingDependency로 실패한다.
TEST(PackageImportPolicy_NullMissing) {
    using C = AssetDiagnosticCode;
    const Expected cases[] = {
        {Variant::Full, 0, {}, {}, false},
        {Variant::Missing, 1, {0}, {C::MissingDependency}, true},
        {Variant::RepresentativeType, 1, {0}, {C::TypeMismatch}, true},
        {Variant::NoPart, 2, {1, 2}, {C::MissingDependency, C::MissingDependency}, false},
        {Variant::PartType, 2, {1, 2}, {C::TypeMismatch, C::MissingDependency}, false},
    };
    for (const Expected& one : cases) {
        AssetEnv env;
        REQUIRE(env.Ready());
        const Array<uint8> file = MakeSourceFile(env);
        REQUIRE(!file.IsEmpty());
        RootRef<Package> target;
        REQUIRE(MakeTarget(env, one.variant, target));
        TestResolver resolver;
        if (one.variant != Variant::Missing) {
            resolver.Add(FixedId(TargetId), target.Handle());
        }

        ObjectRegistry& registry = *env.target;
        PackageLoadState state(env.allocator, registry, env.types, registry.OwnerAccess(),
                               LoadOptions(PackageKind::Editor));
        REQUIRE(state.Prepare(Copy(env.allocator, file.View())));
        REQUIRE(state.Contents().imports.Size() == 3);
        Array<PackageUnresolvedImport> unresolved(env.allocator);
        PackageInstantiateOptions options;
        options.resolver = &resolver;
        options.imports = PackageImportPolicy::NullMissing;
        options.unresolved = &unresolved;
        ConstructionContext transaction(registry, registry.OwnerAccess());
        PackageLoadState* states[] = {&state};
        ObjectHandle packages[1];
        REQUIRE(InstantiatePackages(transaction, states, options, packages));
        RootRef<Object> roots[1];
        REQUIRE(transaction.Commit(packages, roots));
        REQUIRE(unresolved.Size() == one.count);
        for (usize index = 0; index < one.count; ++index) {
            CHECK(unresolved[index].package == FixedId(SourceId));
            CHECK(unresolved[index].target == FixedId(TargetId));
            CHECK(unresolved[index].import == one.imports[index]);
            CHECK(unresolved[index].code == one.codes[index]);
        }

        if (one.variant == Variant::RepresentativeType) {
            CHECK(unresolved[0].error == ErrorCode::TypeMismatch);
        }

        auto representative = registry.FindChild(registry.OwnerAccess(), packages[0], "");
        REQUIRE(representative);
        TestAsset* asset = Pointer<TestAsset>(registry, representative.Value());
        REQUIRE(asset != nullptr);
        CHECK(asset->hard.Handle().IsNull() == one.hardNull);
        REQUIRE(asset->hardList.Size() == 1);
        CHECK(asset->hardList[0].IsNull() == (one.variant != Variant::Full));
    }

    // Strict: 없는 대상은 MissingDependency 실패.
    AssetEnv env;
    REQUIRE(env.Ready());
    const Array<uint8> file = MakeSourceFile(env);
    REQUIRE(!file.IsEmpty());
    PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                           LoadOptions(PackageKind::Editor));
    REQUIRE(state.Prepare(Copy(env.allocator, file.View())));
    ConstructionContext transaction(*env.target, env.target->OwnerAccess());
    PackageLoadState* states[] = {&state};
    ObjectHandle packages[1];
    AssetDiagnostic failure;
    CHECK(
        !InstantiatePackages(transaction, states, PackageInstantiateOptions{}, packages, &failure));
    CHECK(failure.code == AssetDiagnosticCode::MissingDependency);
}

// 인자와 할당 실패(7.1절 15): NullMissing인데 기록 표가 null이면 InvalidArgument(상태는 쓰지 않아
// 다시 넘길 수 있다), 미해결 기록의 할당 실패는 그 오류다.
TEST(PackageImportPolicy_ArgumentsAndAllocation) {
    AssetEnv env;
    REQUIRE(env.Ready());
    const Array<uint8> file = MakeSourceFile(env);
    REQUIRE(!file.IsEmpty());
    ObjectRegistry& registry = *env.target;
    PackageLoadState state(env.allocator, registry, env.types, registry.OwnerAccess(),
                           LoadOptions(PackageKind::Editor));
    REQUIRE(state.Prepare(Copy(env.allocator, file.View())));
    PackageInstantiateOptions options;
    options.imports = PackageImportPolicy::NullMissing;
    PackageLoadState* states[] = {&state};
    ObjectHandle packages[1];
    {
        ConstructionContext transaction(registry, registry.OwnerAccess());
        CHECK(InstantiatePackages(transaction, states, options, packages).Error() ==
              ErrorCode::InvalidArgument);
    }

    test::NFailAllocator failing(env.allocator);
    Array<PackageUnresolvedImport> unresolved(failing);
    options.unresolved = &unresolved;
    failing.FailOnAttempt(1);
    ConstructionContext transaction(registry, registry.OwnerAccess());
    AssetDiagnostic failure;
    CHECK(InstantiatePackages(transaction, states, options, packages, &failure).Error() ==
          ErrorCode::OutOfMemory);
    failing.DisableFailure();
    CHECK(failure.code == AssetDiagnosticCode::None);
}

} // namespace package_import_policy_tests_file
