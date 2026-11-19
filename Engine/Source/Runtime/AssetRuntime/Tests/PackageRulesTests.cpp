#include "PackageEditing.h"

// 패키지 규칙 함수 단위 시험이다(Docs/Design/AssetSystem.ko.md 5.7절 5·6·7): Dependencies 합성 규칙
// (MergeDependencyUses — writer와 로드 대조가 같은 함수), 같은 bulk가 EditorOnly·일반 위치에 함께
// 쓰일 때의 entry 표시, ImportOwner 의존의 로드(대조하지 않음), Package·NewPackage의 인자 오류.
using namespace rocks;
using namespace rocks::asset_test;
using package_detail::DependencyUse;

namespace {

PackageDependencyEntry Entry(uint64 id, PackageDependencyKind kind, bool required) noexcept {
    return PackageDependencyEntry{FixedId(id), kind, TypeId{}, required};
}

bool SameEntries(Span<const PackageDependencyEntry> actual,
                 Span<const PackageDependencyEntry> expected) noexcept {
    if (actual.Size() != expected.Size()) {
        return false;
    }

    for (usize index = 0; index < actual.Size(); ++index) {
        if (!(actual[index].asset == expected[index].asset) ||
            actual[index].kind != expected[index].kind ||
            actual[index].required != expected[index].required) {
            return false;
        }
    }

    return true;
}

} // namespace

// 합성 규칙: 에셋마다 hard 계열은 EditorOnly가 아닌 쓰임이 있으면 Hard, EditorOnly 쓰임만이면
// EditorOnly, soft 계열도 같고, 두 계열이 모두 EditorOnly면 EditorOnly 한 항목(required는 OR)이다.
// 자기 에셋·null 에셋은 넣지 않고 결과는 (assetId, kind) 순이다.
TEST(AssetPackage_MergeDependencyUsesRules) {
    SystemAllocator allocator;
    const AssetId self = FixedId(900);
    const DependencyUse uses[] = {
        // 903: hard 일반 + hard EditorOnly(required) → Hard(required 아님 — 종류를 정한 쓰임만 OR).
        {FixedId(903), true, false, false},
        {FixedId(903), true, true, true},
        // 901: soft EditorOnly(required) + hard EditorOnly → EditorOnly 한 항목(required OR).
        {FixedId(901), false, true, true},
        {FixedId(901), true, true, false},
        // 902: soft 일반 required + soft 일반 → Soft(required). hard EditorOnly → EditorOnly.
        {FixedId(902), false, false, true},
        {FixedId(902), false, false, false},
        {FixedId(902), true, true, false},
        // 904: soft EditorOnly만 → EditorOnly. 자기·null은 빠진다.
        {FixedId(904), false, true, false},
        // 905: hard EditorOnly(required) + soft EditorOnly → EditorOnly(required — hard 쪽에서).
        {FixedId(905), true, true, true},
        {FixedId(905), false, true, false},
        {self, true, false, true},
        {AssetId{}, false, false, false}};
    Array<PackageDependencyEntry> merged(allocator);
    REQUIRE(package_detail::MergeDependencyUses(allocator, uses, self, merged));
    merged.Sort(package_detail::DependencyLess);
    const PackageDependencyEntry expected[] = {Entry(901, PackageDependencyKind::EditorOnly, true),
                                               Entry(902, PackageDependencyKind::Soft, true),
                                               Entry(902, PackageDependencyKind::EditorOnly, false),
                                               Entry(903, PackageDependencyKind::Hard, false),
                                               Entry(904, PackageDependencyKind::EditorOnly, false),
                                               Entry(905, PackageDependencyKind::EditorOnly, true)};
    CHECK(SameEntries(merged.View(), expected));
    // 빈 입력은 빈 결과다.
    Array<PackageDependencyEntry> empty(allocator);
    REQUIRE(
        package_detail::MergeDependencyUses(allocator, Span<const DependencyUse>(), self, empty));
    CHECK(empty.IsEmpty());
}

// 같은 bulk ref가 EditorOnly 위치(대표의 source)에 먼저, 일반 위치(하위 객체 data)에 나중에 쓰이면
// entry는 EditorOnly가 아니다(모든 위치가 EditorOnly일 때만 표시). 쿠킹은 일반 위치만 남는다.
TEST(AssetPackage_SharedBulkEditorOnlyFlag) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    auto package = NewPackage(registry, registry.OwnerAccess(), FixedId(910), PackageKind::Editor);
    REQUIRE(package);
    const ObjectHandle asset =
        Make<TestAsset>(registry, TestAssetType(), package.Value().Handle(), "");
    const ObjectHandle part = Make<TestPart>(registry, TestPartType(), asset, "Part");
    REQUIRE(!asset.IsNull() && !part.IsNull());
    const uint8 data[5] = {1, 2, 3, 4, 5};
    TestBulkSource bulk;
    const BulkDataRef ref = bulk.Add(4, Span<const uint8>(data, 5));
    Pointer<TestAsset>(registry, asset)->source = ref;
    Pointer<TestPart>(registry, part)->data = ref;
    const PackageKind kinds[] = {PackageKind::Editor, PackageKind::Cooked};
    for (PackageKind kind : kinds) {
        PackageWriteOptions options =
            kind == PackageKind::Editor ? EditorOptions() : CookedOptions();
        options.bulkSource = &bulk;
        Array<uint8> bytes = Write(env.allocator, registry, package.Value().Handle(), options);
        REQUIRE(!bytes.IsEmpty());
        PackageReadOptions read;
        read.expectedKind = kind;
        auto contents = ReadPackage(env.allocator, bytes.View(), read);
        REQUIRE(contents);
        REQUIRE(contents.Value().bulkEntries.Size() == 1);
        CHECK(contents.Value().bulkEntries[0].flags == 0);
    }
}

// ImportOwner 의존(에디터 패키지 — 다중 출력의 Import 기록 소유자)은 기록되고, ImportRecord와의
// 로드 대조는 A2로 미룬다(ImportRecord 스키마가 A2 — AS 5.7절 17). 지금은 대조하지 않음을 고정한다.
TEST(AssetPackage_ImportOwnerCrossCheckDeferredToA2) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    auto package = NewPackage(registry, registry.OwnerAccess(), FixedId(920), PackageKind::Editor);
    REQUIRE(package);
    REQUIRE(!Make<TestAsset>(registry, TestAssetType(), package.Value().Handle(), "").IsNull());
    const PackageDependencyEntry extra[] = {
        {FixedId(921), PackageDependencyKind::Build, TypeId{}, false},
        {FixedId(922), PackageDependencyKind::ImportOwner, TypeId{}, true}};
    PackageWriteOptions options = EditorOptions();
    options.extraDependencies = extra;
    Array<uint8> bytes = Write(env.allocator, registry, package.Value().Handle(), options);
    REQUIRE(!bytes.IsEmpty());
    PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                           LoadOptions(PackageKind::Editor));
    REQUIRE(state.Prepare(Move(bytes)));
    REQUIRE(state.Contents().dependencies.Size() == 2);
    CHECK(state.Contents().dependencies[1].kind == PackageDependencyKind::ImportOwner);
    CHECK(state.Contents().dependencies[1].required);
    CHECK(!state.IsReadOnly());

    CHECK(state.Stage().Nodes().IsEmpty());
}

// 인자 오류: NewPackage의 null id·정의되지 않은 종류, BulkBytes의 같은 크기·다른 SHA-256.
TEST(AssetPackage_PackageArgumentErrors) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    CHECK(NewPackage(registry, access, AssetId{}, PackageKind::Cooked).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(NewPackage(registry, access, FixedId(930), static_cast<PackageKind>(3)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(NewPackage(registry, access, FixedId(930), static_cast<PackageKind>(0)).Error() ==
          ErrorCode::InvalidArgument);

    auto package = NewPackage(registry, access, FixedId(931), PackageKind::Cooked);
    REQUIRE(package);
    const ObjectHandle asset =
        Make<TestAsset>(registry, TestAssetType(), package.Value().Handle(), "");
    REQUIRE(!asset.IsNull());
    const uint8 data[3] = {7, 8, 9};
    TestBulkSource bulk;
    Pointer<TestAsset>(registry, asset)->pixels = bulk.Add(1, Span<const uint8>(data, 3));
    PackageWriteOptions options = CookedOptions();
    options.bulkSource = &bulk;
    Array<uint8> files[1] = {Write(env.allocator, registry, package.Value().Handle(), options)};
    REQUIRE(!files[0].IsEmpty());
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    REQUIRE(LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots, handles));
    Package* loaded = Pointer<Package>(*env.target, handles[0]);
    REQUIRE(loaded != nullptr);
    BulkDataRef wrong = Pointer<TestAsset>(registry, asset)->pixels;
    wrong.sha256.bytes[0] ^= 1;
    CHECK(loaded->BulkBytes(wrong).Error() == ErrorCode::TypeMismatch);

    // 진단 out 인자는 선택이다(null이면 오류 코드만 돌려준다).
    Array<uint8> empty(env.allocator);
    CHECK(ReadPackage(env.allocator, empty.View(), PackageReadOptions{}).Error() ==
          ErrorCode::InvalidArgument);
    // 섹션 종류 표: 알려진 kind는 1..9, 모르는 kind의 고정 flags는 0이다.
    CHECK(!IsKnownPackageSection(0));
    CHECK(!IsKnownPackageSection(PackageKnownSectionKinds + 1));
    CHECK(PackageSectionRequiredFlags(
              static_cast<PackageSectionKind>(PackageKnownSectionKinds + 1)) == 0);
}
