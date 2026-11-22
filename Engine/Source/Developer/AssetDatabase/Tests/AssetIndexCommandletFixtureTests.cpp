#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "AssetIndexCommandletFixtures.h"

#include <AssetDatabase/ImportRecord.h>
#include <AssetRuntime/PackageTags.h>
#include <Platform/File.h>
#include <stdio.h>
#include <string.h>

// 명령 시험 fixture byte 고정 시험이다(Docs/Design/AssetSystem.ko.md 7.1절 13 — A2b). C# 명령
// 시험(Programs/Rocks.Commandlet.Tests)은 패키지를 만들 수 없어 AssetIndexCommandletFixtures.h의
// 배열을 읽어 프로젝트에 쓴다. 이 시험이 그 배열이 지금 writer 출력과 같은지 본다 — 포맷이 바뀌어
// 다르면 새 헤더 내용을 현재 디렉터리의 AssetIndexCommandletFixtures.h.new에 쓰고 실패한다(그
// 파일로 헤더를 바꾼다). 장면: Hero(2300, TestAsset — Hard → Base, Soft → 2309 "Sub", 태그
// Kind=Hero), Base(2301, BaseGameAsset), 같은 id의 DerivedGameAsset 대표(형식 대조용), Newer(2310,
// 엔진 99.0.0 저장 — 명령 엔진보다 새 패키지), 고아 출력 둘(A2e4 — Orphan(2311)은 색인에 없는
// owner 2312, HeroOrphan(2313)은 기록이 없는 Hero를 owner로 가리키는 참조 기록). 엔진 0.1.0
// 저장(명령 엔진 버전 이하).
using namespace rocks;
namespace asset_index_commandlet_fixture_tests_file {

using namespace rocks::asset_test;

namespace {

constexpr uint64 HeroId = 2300;
constexpr uint64 BaseId = 2301;
constexpr uint64 MissingSoftId = 2309;
constexpr uint64 NewerId = 2310;
constexpr uint64 OrphanId = 2311;
constexpr uint64 MissingOwnerId = 2312;
constexpr uint64 HeroOrphanId = 2313;
constexpr EngineVersion FixtureEngineVersion{0, 1, 0};
constexpr EngineVersion NewerEngineVersion{99, 0, 0};
// 배열 한 줄의 byte 수다(clang-format 100열 안).
constexpr usize BytesPerLine = 16;

const PackageTagInput HeroTags[] = {{"Kind", "Hero"}};

struct Fixtures {
    explicit Fixtures(Allocator& allocator) noexcept
        : hero(allocator), base(allocator), baseAsDerived(allocator), newer(allocator),
          orphan(allocator), heroOrphan(allocator) {
    }

    Array<uint8> hero;
    Array<uint8> base;
    Array<uint8> baseAsDerived;
    Array<uint8> newer;
    Array<uint8> orphan;
    Array<uint8> heroOrphan;
};

template <class T>
ObjectHandle MakeRepresentative(ObjectRegistry& registry, uint64 id,
                                RootRef<Package>& package) noexcept {
    auto made = NewPackage(registry, registry.OwnerAccess(), FixedId(id), PackageKind::Editor);
    if (!made) {
        return ObjectHandle{};
    }

    package = Move(made.Value());
    return Make<T>(registry, IdOf<T>(), package.Handle(), "");
}

PackageWriteOptions FixtureOptions(EngineVersion version) noexcept {
    PackageWriteOptions options = EditorOptions();
    options.engineVersion = version;
    return options;
}

// id 패키지(TestAsset)를 owner를 가리키는 참조 기록 + ImportOwner 의존으로 쓴다.
Array<uint8> WriteOutput(AssetEnv& env, uint64 id, uint64 owner) noexcept {
    ObjectRegistry& registry = *env.source;
    RootRef<Package> package;
    if (MakeRepresentative<TestAsset>(registry, id, package).IsNull()) {
        return Array<uint8>(env.allocator);
    }

    ImportRecordInput input;
    input.owner = FixedId(owner);
    auto record = WriteImportRecord(env.allocator, FixedId(id), input);
    if (!record) {
        return Array<uint8>(env.allocator);
    }

    const PackageDependencyEntry owners[] = {
        {FixedId(owner), PackageDependencyKind::ImportOwner, TypeId{}, false}};
    PackageWriteOptions options = FixtureOptions(FixtureEngineVersion);
    options.importRecord = record.Value().View();
    options.extraDependencies = owners;
    return Write(env.allocator, registry, package.Handle(), options);
}

bool MakeFixtures(AssetEnv& env, Fixtures& out) noexcept {
    ObjectRegistry& registry = *env.source;
    RootRef<Package> heroPackage;
    RootRef<Package> basePackage;
    RootRef<Package> newerPackage;
    const ObjectHandle base = MakeRepresentative<BaseGameAsset>(registry, BaseId, basePackage);
    const ObjectHandle hero = MakeRepresentative<TestAsset>(registry, HeroId, heroPackage);
    const ObjectHandle newer = MakeRepresentative<TestAsset>(registry, NewerId, newerPackage);
    TestAsset* heroValue = Pointer<TestAsset>(registry, hero);
    if (base.IsNull() || newer.IsNull() || heroValue == nullptr ||
        !registry.SetReference(registry.OwnerAccess(), hero, heroValue->hard, base) ||
        !heroValue->soft.TryAssign(FixedId(MissingSoftId), "Sub")) {
        return false;
    }

    auto tags = WritePackageTags(env.allocator, HeroTags);
    if (!tags) {
        return false;
    }

    PackageWriteOptions heroOptions = FixtureOptions(FixtureEngineVersion);
    heroOptions.tags = tags.Value().View();
    out.hero = Write(env.allocator, registry, heroPackage.Handle(), heroOptions);
    out.base =
        Write(env.allocator, registry, basePackage.Handle(), FixtureOptions(FixtureEngineVersion));
    out.newer =
        Write(env.allocator, registry, newerPackage.Handle(), FixtureOptions(NewerEngineVersion));
    ObjectRegistry& variants = *env.target;
    RootRef<Package> derivedPackage;
    if (MakeRepresentative<DerivedGameAsset>(variants, BaseId, derivedPackage).IsNull()) {
        return false;
    }

    out.baseAsDerived = Write(env.allocator, variants, derivedPackage.Handle(),
                              FixtureOptions(FixtureEngineVersion));
    out.orphan = WriteOutput(env, OrphanId, MissingOwnerId);
    out.heroOrphan = WriteOutput(env, HeroOrphanId, HeroId);
    return !out.hero.IsEmpty() && !out.base.IsEmpty() && !out.newer.IsEmpty() &&
           !out.baseAsDerived.IsEmpty() && !out.orphan.IsEmpty() && !out.heroOrphan.IsEmpty();
}

bool Same(Span<const uint8> expected, const Array<uint8>& actual) noexcept {
    return expected.Size() == actual.Size() &&
           memcmp(expected.Data(), actual.Data(), actual.Size()) == 0;
}

// 배열 하나의 정의를 text에 붙인다.
bool AppendArray(String& text, const char* name, const Array<uint8>& bytes) noexcept {
    char piece[64];
    int count = snprintf(piece, sizeof(piece), "inline constexpr uint8 %s[] = {\n", name);
    bool ok = count > 0 &&
              static_cast<bool>(text.TryAppend(StringView(piece, static_cast<usize>(count))));
    for (usize index = 0; ok && index < bytes.Size(); ++index) {
        const bool lineEnd = index % BytesPerLine == BytesPerLine - 1 || index + 1 == bytes.Size();
        count =
            snprintf(piece, sizeof(piece), "%s0x%02x,%s", index % BytesPerLine == 0 ? "    " : "",
                     static_cast<unsigned>(bytes[index]), lineEnd ? "\n" : " ");
        ok = count > 0 &&
             static_cast<bool>(text.TryAppend(StringView(piece, static_cast<usize>(count))));
    }

    return ok && static_cast<bool>(text.TryAppend("};\n"));
}

} // namespace

TEST(AssetIndex_CommandletFixtureBytes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Fixtures fixtures(env.allocator);
    REQUIRE(MakeFixtures(env, fixtures));
    const bool same =
        Same(asset_index_fixtures::CommandletHeroPackage, fixtures.hero) &&
        Same(asset_index_fixtures::CommandletBasePackage, fixtures.base) &&
        Same(asset_index_fixtures::CommandletBaseAsDerivedPackage, fixtures.baseAsDerived) &&
        Same(asset_index_fixtures::CommandletNewerPackage, fixtures.newer) &&
        Same(asset_index_fixtures::CommandletOrphanPackage, fixtures.orphan) &&
        Same(asset_index_fixtures::CommandletHeroOrphanPackage, fixtures.heroOrphan);
    if (!same) {
        String text(env.allocator);
        REQUIRE(AppendArray(text, "CommandletHeroPackage", fixtures.hero));
        REQUIRE(text.TryAppend("\n"));
        REQUIRE(AppendArray(text, "CommandletBasePackage", fixtures.base));
        REQUIRE(text.TryAppend("\n"));
        REQUIRE(AppendArray(text, "CommandletBaseAsDerivedPackage", fixtures.baseAsDerived));
        REQUIRE(text.TryAppend("\n"));
        REQUIRE(AppendArray(text, "CommandletNewerPackage", fixtures.newer));
        REQUIRE(text.TryAppend("\n"));
        REQUIRE(AppendArray(text, "CommandletOrphanPackage", fixtures.orphan));
        REQUIRE(text.TryAppend("\n"));
        REQUIRE(AppendArray(text, "CommandletHeroOrphanPackage", fixtures.heroOrphan));
        platform::File file;
        REQUIRE(
            file.Open("AssetIndexCommandletFixtures.h.new", platform::FileOpenMode::CreateAlways));
        REQUIRE(file.WriteAll(text.View().Data(), text.View().ByteCount()).status);
        REQUIRE(file.Close());
    }

    CHECK(same);
}

} // namespace asset_index_commandlet_fixture_tests_file
