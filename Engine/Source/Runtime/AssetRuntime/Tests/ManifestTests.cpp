#include "ManifestTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>
#include <Core/Text/NameComponent.h>
#include <TestSupport/NFailAllocator.h>

// 쿠킹 매니페스트 v1 시험이다(Docs/Design/AssetSystem.ko.md 10절, 10.1절 A1b): 왕복·결정성·Find,
// 칸마다 규칙 위반(검증·writer·reader가 같은 진단), 구조 변조(CRC를 다시 맞춘 잘림·개수·길이·남는
// byte), 버전, 대조 함수(프로파일·병합·패키지 — A1a writer가 쓴 쿠킹 패키지와), 파일 경로.
using namespace rocks;
using namespace rocks::asset_test;

namespace {

// 기본 장면: 10(PrimaryAsset, bundle 둘, Hard → 11·Soft → 12·Soft → 99 — 쿠킹하지 않은 대상),
// 11(Hard → 10 — 10과 순환), 12(의존 없음).
bool MakeBase(CookedManifest& manifest) noexcept {
    FillManifestHeader(manifest);
    CookedManifestEntry* a =
        AddEntry(manifest, 10, "Hero", static_cast<uint32>(CookedManifestFlags::PrimaryAsset));
    if (a == nullptr || !AddBundle(*a, "Core") || !AddBundle(*a, "Hud") ||
        !AddDependency(*a, 11, PackageDependencyKind::Hard) ||
        !AddDependency(*a, 12, PackageDependencyKind::Soft) ||
        !AddDependency(*a, 99, PackageDependencyKind::Soft)) {
        return false;
    }

    CookedManifestEntry* b = AddEntry(manifest, 11, "Sword");
    if (b == nullptr || !AddDependency(*b, 10, PackageDependencyKind::Hard)) {
        return false;
    }

    return AddEntry(manifest, 12, "Shield") != nullptr;
}

bool SameEntry(const CookedManifestEntry& left, const CookedManifestEntry& right) noexcept {
    bool same = left.asset == right.asset && left.type == right.type &&
                left.pathHint.View() == right.pathHint.View() &&
                SameBytes(Span<const uint8>(left.packageSha256.bytes, 32),
                          Span<const uint8>(right.packageSha256.bytes, 32)) &&
                left.packageBytes == right.packageBytes &&
                left.bulkRawBytes == right.bulkRawBytes &&
                SameBytes(Span<const uint8>(left.cookKey.bytes, 32),
                          Span<const uint8>(right.cookKey.bytes, 32)) &&
                left.flags == right.flags && left.bundles.Size() == right.bundles.Size() &&
                left.dependencies.Size() == right.dependencies.Size();
    for (usize index = 0; same && index < left.bundles.Size(); ++index) {
        same = left.bundles[index].View() == right.bundles[index].View();
    }

    for (usize index = 0; same && index < left.dependencies.Size(); ++index) {
        same = left.dependencies[index].asset == right.dependencies[index].asset &&
               left.dependencies[index].kind == right.dependencies[index].kind;
    }

    return same;
}

bool SameManifest(const CookedManifest& left, const CookedManifest& right) noexcept {
    bool same = left.profileId.View() == right.profileId.View() &&
                SameBytes(Span<const uint8>(left.profileSettings.bytes, 32),
                          Span<const uint8>(right.profileSettings.bytes, 32)) &&
                SameBytes(Span<const uint8>(left.cookSettings.bytes, 32),
                          Span<const uint8>(right.cookSettings.bytes, 32)) &&
                left.cookerVersion == right.cookerVersion &&
                left.cookedBy.majorVersion == right.cookedBy.majorVersion &&
                left.cookedBy.minorVersion == right.cookedBy.minorVersion &&
                left.cookedBy.patchVersion == right.cookedBy.patchVersion &&
                left.entries.Size() == right.entries.Size();
    for (usize index = 0; same && index < left.entries.Size(); ++index) {
        same = SameEntry(left.entries[index], right.entries[index]);
    }

    return same;
}

// 진단의 asset·related 기대값에서 null id를 뜻한다(FixedId 하위 값으로 쓰지 않는 값).
constexpr uint64 NoId = 0;

AssetId ExpectedId(uint64 low) noexcept {
    return low == NoId ? AssetId{} : FixedId(low);
}

// 규칙 위반 하나다: 기본 장면을 바꾸는 함수와 기대 칸·항목·진단 asset·related(FixedId 하위 값,
// NoId = null).
struct Violation {
    const char* name;
    void (*mutate)(CookedManifest&);
    CookedManifestField field;
    uint64 detail;
    uint64 asset;
    uint64 related;
};

void SetPath(CookedManifest& manifest, usize index, StringView path) {
    manifest.entries[index].pathHint.Clear();
    (void)manifest.entries[index].pathHint.TryAppend(path);
}

// index 항목의 경로를 정확히 bytes byte로 만든다: "/Game" + 64 byte 구성 요소 셋 + 남은 길이의
// 마지막 구성 요소(bytes는 202 이상, 마지막 구성 요소는 64 byte 이하가 되는 길이).
void SetPathOfLength(CookedManifest& manifest, usize index, usize bytes) {
    constexpr uint32 FullComponents = 3;
    SetPath(manifest, index, "/Game");
    String& path = manifest.entries[index].pathHint;
    for (uint32 part = 0; part < FullComponents; ++part) {
        (void)path.TryAppend("/");
        for (uint32 at = 0; at < MaxNameComponentBytes; ++at) {
            (void)path.TryAppend("a");
        }
    }

    (void)path.TryAppend("/");
    while (path.View().ByteCount() < bytes) {
        (void)path.TryAppend("b");
    }
}

// 실패한 쓰기가 out을 건드리지 않았는지 보려고 미리 채우는 값이다.
Array<uint8> Prefilled(Allocator& allocator) noexcept {
    Array<uint8> out(allocator);
    (void)out.TryPushBack(0xAA);
    (void)out.TryPushBack(0xBB);
    return out;
}

bool IsPrefilled(const Array<uint8>& out) noexcept {
    return out.Size() == 2 && out[0] == 0xAA && out[1] == 0xBB;
}

// 진단의 asset·related가 기대값인지 본다.
bool SameIds(const AssetDiagnostic& failure, uint64 asset, uint64 related) noexcept {
    return failure.asset == ExpectedId(asset) && failure.related == ExpectedId(related);
}

void SetBundles(CookedManifest& manifest, StringView first, StringView second) {
    manifest.entries[0].bundles.Clear();
    (void)AddBundle(manifest.entries[0], first);
    (void)AddBundle(manifest.entries[0], second);
}

void SetDependency(CookedManifest& manifest, usize position, uint64 low,
                   PackageDependencyKind kind) {
    manifest.entries[0].dependencies[position] = CookedManifestDependency{FixedId(low), kind};
}

using F = CookedManifestField;
constexpr uint64 Head = CookedManifestNoEntry;

const Violation Violations[] = {
    {"empty profile", [](CookedManifest& m) { m.profileId.Clear(); }, F::ProfileId, Head, NoId,
     NoId},
    {"long profile",
     [](CookedManifest& m) {
         m.profileId.Clear();
         for (uint32 index = 0; index <= PackageMaxStringBytes; ++index) {
             (void)m.profileId.TryAppend("p");
         }
     },
     F::ProfileId, Head, NoId, NoId},
    {"null asset", [](CookedManifest& m) { m.entries[0].asset = AssetId{}; }, F::AssetId, 0, NoId,
     NoId},
    {"duplicate asset", [](CookedManifest& m) { m.entries[1].asset = FixedId(10); }, F::AssetId, 1,
     10, NoId},
    {"unsorted assets", [](CookedManifest& m) { m.entries[2].asset = FixedId(5); }, F::AssetId, 2,
     5, NoId},
    {"null type", [](CookedManifest& m) { m.entries[1].type = TypeId{}; }, F::TypeId, 1, 11, NoId},
    {"empty path", [](CookedManifest& m) { SetPath(m, 0, ""); }, F::PathHint, 0, 10, NoId},
    {"relative path", [](CookedManifest& m) { SetPath(m, 0, "Game/Hero"); }, F::PathHint, 0, 10,
     NoId},
    {"mount only", [](CookedManifest& m) { SetPath(m, 0, "/Game"); }, F::PathHint, 0, 10, NoId},
    {"empty component", [](CookedManifest& m) { SetPath(m, 0, "/Game//Hero"); }, F::PathHint, 0, 10,
     NoId},
    {"trailing slash", [](CookedManifest& m) { SetPath(m, 0, "/Game/Hero/"); }, F::PathHint, 0, 10,
     NoId},
    {"space", [](CookedManifest& m) { SetPath(m, 0, "/Game/He ro"); }, F::PathHint, 0, 10, NoId},
    {"dot", [](CookedManifest& m) { SetPath(m, 0, "/Game/Hero.Body"); }, F::PathHint, 0, 10, NoId},
    {"reserved", [](CookedManifest& m) { SetPath(m, 0, "/Game/CON"); }, F::PathHint, 0, 10, NoId},
    {"not NFC", [](CookedManifest& m) { SetPath(m, 0, "/Game/He\xCC\x81ro"); }, F::PathHint, 0, 10,
     NoId},
    {"backslash", [](CookedManifest& m) { SetPath(m, 0, "/Game\\Hero"); }, F::PathHint, 0, 10,
     NoId},
    // 64 byte 구성 요소 넷 + 구분자 = 265 byte.
    {"long path",
     [](CookedManifest& m) {
         SetPath(m, 0, "/Game");
         for (uint32 part = 0; part < 4; ++part) {
             (void)m.entries[0].pathHint.TryAppend("/");
             for (uint32 index = 0; index < MaxNameComponentBytes; ++index) {
                 (void)m.entries[0].pathHint.TryAppend("a");
             }
         }
     },
     F::PathHint, 0, 10, NoId},
    // 상한 + 1 byte(경계 256 byte는 아래에서 받는다).
    {"path one byte over",
     [](CookedManifest& m) { SetPathOfLength(m, 1, CookedManifestMaxPathBytes + 1); }, F::PathHint,
     1, 11, NoId},
    {"caseless duplicate path", [](CookedManifest& m) { SetPath(m, 2, "/game/HERO"); }, F::PathHint,
     2, 12, NoId},
    {"three equal paths",
     [](CookedManifest& m) {
         SetPath(m, 1, "/GAME/hero");
         SetPath(m, 2, "/game/HERO");
     },
     F::PathHint, 1, 11, NoId},
    // 비ASCII 대소문자(Ä·ä)도 caseless로 같은 경로다.
    {"non-ASCII caseless duplicate path",
     [](CookedManifest& m) {
         SetPath(m, 1, "/Game/\xC3\x84rger");
         SetPath(m, 2, "/game/\xC3\xA4rger");
     },
     F::PathHint, 2, 12, NoId},
    {"small package", [](CookedManifest& m) { m.entries[1].packageBytes = PackageHeaderBytes - 1; },
     F::PackageBytes, 1, 11, NoId},
    {"large package",
     [](CookedManifest& m) { m.entries[1].packageBytes = PackageMaxFileBytes + 1; },
     F::PackageBytes, 1, 11, NoId},
    {"undefined flag", [](CookedManifest& m) { m.entries[2].flags = 2; }, F::Flags, 2, 12, NoId},
    {"bundles without PrimaryAsset", [](CookedManifest& m) { m.entries[0].flags = 0; },
     F::BundleCount, 0, 10, NoId},
    {"too many bundles",
     [](CookedManifest& m) {
         m.entries[0].bundles.Clear();
         for (uint32 index = 0; index <= CookedManifestMaxBundles; ++index) {
             char name[5] = {'B', static_cast<char>('a' + index / 26 / 26 % 26),
                             static_cast<char>('a' + index / 26 % 26),
                             static_cast<char>('a' + index % 26), 0};
             (void)AddBundle(m.entries[0], name);
         }
     },
     F::BundleCount, 0, 10, NoId},
    {"invalid bundle", [](CookedManifest& m) { SetBundles(m, "Core", "H d"); }, F::Bundle, 0, 10,
     NoId},
    {"unsorted bundles", [](CookedManifest& m) { SetBundles(m, "Hud", "Core"); }, F::Bundle, 0, 10,
     NoId},
    {"duplicate bundle", [](CookedManifest& m) { SetBundles(m, "Hud", "Hud"); }, F::Bundle, 0, 10,
     NoId},
    {"caseless duplicate bundle", [](CookedManifest& m) { SetBundles(m, "HUD", "Hud"); }, F::Bundle,
     0, 10, NoId},
    // Ä(C3 84) < ä(C3 A4) — byte 순서는 맞고 caseless로 같다(8진 escape — 16진은 뒤의 b를 먹는다).
    {"non-ASCII caseless duplicate bundle",
     [](CookedManifest& m) { SetBundles(m, "\303\204b", "\303\244b"); }, F::Bundle, 0, 10, NoId},
    {"null dependency",
     [](CookedManifest& m) {
         m.entries[0].dependencies[0] =
             CookedManifestDependency{AssetId{}, PackageDependencyKind::Hard};
     },
     F::Dependency, 0, 10, NoId},
    {"self dependency",
     [](CookedManifest& m) { SetDependency(m, 0, 10, PackageDependencyKind::Hard); }, F::Dependency,
     0, 10, 10},
    {"editor-only kind",
     [](CookedManifest& m) { SetDependency(m, 0, 11, PackageDependencyKind::EditorOnly); },
     F::Dependency, 0, 10, 11},
    {"build kind", [](CookedManifest& m) { SetDependency(m, 0, 11, PackageDependencyKind::Build); },
     F::Dependency, 0, 10, 11},
    {"unsorted dependencies",
     [](CookedManifest& m) { SetDependency(m, 2, 5, PackageDependencyKind::Soft); }, F::Dependency,
     0, 10, 5},
    {"duplicate dependency",
     [](CookedManifest& m) { SetDependency(m, 1, 11, PackageDependencyKind::Hard); }, F::Dependency,
     0, 10, 11},
    {"hard dependency not cooked",
     [](CookedManifest& m) { SetDependency(m, 2, 99, PackageDependencyKind::Hard); }, F::Dependency,
     0, 10, 99},
    {"too many dependencies",
     [](CookedManifest& m) {
         m.entries[2].dependencies.Clear();
         for (uint32 index = 0; index <= CookedManifestMaxDependencies; ++index) {
             (void)AddDependency(m.entries[2], 1000 + index, PackageDependencyKind::Soft);
         }
     },
     F::DependencyCount, 2, 12, NoId},
};

} // namespace

// 왕복: 기본 장면을 쓰고 읽으면 같은 모델이고, 다시 쓰면 같은 byte다(결정성). 시험 인코더도 같은
// byte를 낸다(배치 일치). Find는 있는 id·없는 id(앞·사이·뒤)를 맞힌다. 빈 매니페스트도 왕복한다.
TEST(CookedManifest_RoundTrip) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest manifest(allocator);
    REQUIRE(MakeBase(manifest));
    Array<uint8> bytes(allocator);
    AssetDiagnostic failure;
    REQUIRE(WriteCookedManifest(allocator, manifest, bytes, &failure));
    CHECK(SameBytes(bytes.View(), EncodeManifest(allocator, manifest).View()));
    auto read = ReadCookedManifest(allocator, bytes.View(), &failure);
    REQUIRE(read);
    CHECK(SameManifest(read.Value(), manifest));
    Array<uint8> again(allocator);
    REQUIRE(WriteCookedManifest(allocator, read.Value(), again));
    CHECK(SameBytes(again.View(), bytes.View()));

    CHECK(read.Value().Find(FixedId(11)) == &read.Value().entries[1]);
    CHECK(read.Value().Find(FixedId(10)) == &read.Value().entries[0]);
    CHECK(read.Value().Find(FixedId(12)) == &read.Value().entries[2]);

    // Find의 없는 id: 10·12·14 사이가 빈 장면에서 앞(1)·사이(11·13)·뒤(15).
    CookedManifest gapped(allocator);
    FillManifestHeader(gapped);
    REQUIRE(AddEntry(gapped, 10, "Ten") != nullptr && AddEntry(gapped, 12, "Twelve") != nullptr &&
            AddEntry(gapped, 14, "Fourteen") != nullptr);
    Array<uint8> gappedBytes(allocator);
    REQUIRE(WriteCookedManifest(allocator, gapped, gappedBytes));
    auto gappedRead = ReadCookedManifest(allocator, gappedBytes.View());
    REQUIRE(gappedRead);
    const uint64 present[] = {10, 12, 14};
    for (usize index = 0; index < 3; ++index) {
        CHECK(gappedRead.Value().Find(FixedId(present[index])) ==
              &gappedRead.Value().entries[index]);
    }

    const uint64 missing[] = {1, 11, 13, 15};
    for (uint64 low : missing) {
        CHECK(gappedRead.Value().Find(FixedId(low)) == nullptr);
    }

    // 비ASCII NFC 경로·bundle은 받고 그대로 왕복한다.
    CookedManifest unicode(allocator);
    REQUIRE(MakeBase(unicode));
    SetPath(unicode, 1, "/Game/\xC3\x84rger");
    SetBundles(unicode, "Core", "\303\204b");
    Array<uint8> unicodeBytes(allocator);
    REQUIRE(WriteCookedManifest(allocator, unicode, unicodeBytes, &failure));
    auto unicodeRead = ReadCookedManifest(allocator, unicodeBytes.View(), &failure);
    REQUIRE(unicodeRead);
    CHECK(SameManifest(unicodeRead.Value(), unicode));

    CookedManifest empty(allocator);
    FillManifestHeader(empty);
    Array<uint8> emptyBytes(allocator);
    REQUIRE(WriteCookedManifest(allocator, empty, emptyBytes));
    auto emptyRead = ReadCookedManifest(allocator, emptyBytes.View());
    REQUIRE(emptyRead);
    CHECK(emptyRead.Value().entries.IsEmpty() && SameManifest(emptyRead.Value(), empty));
    CHECK(emptyRead.Value().Find(FixedId(10)) == nullptr);

    // 진단 out 인자 없이도 같은 오류다(읽기·검증·쓰기).
    CHECK(ReadCookedManifest(allocator, Span<const uint8>()).Error() == ErrorCode::InvalidArgument);
    empty.profileId.Clear();
    CHECK(ValidateCookedManifest(empty, allocator).Error() == ErrorCode::InvalidArgument);
    Array<uint8> refused(allocator);
    CHECK(WriteCookedManifest(allocator, empty, refused).Error() == ErrorCode::InvalidArgument);
}

// 칸 규칙(AS 10.1절 4–6): 위반이 하나뿐인 모델마다 ValidateCookedManifest·WriteCookedManifest(out
// 그대로)·ReadCookedManifest(시험 인코더로 쓴 byte)가 같은 칸·항목·asset·related의 CorruptPackage다
// (위반이 여럿이면 세 경로가 다를 수 있다 — CookedManifest_JudgementOrder). 경계는 받는다(패키지
// 64 byte·64 GiB, bundle 256개, 의존 65536개, 경로 256 byte, 프로파일 4096 byte, 엔진 버전 칸마다
// 0xFFFF, 쿠킹하지 않은 Soft 대상).
TEST(CookedManifest_FieldRules) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    for (const Violation& violation : Violations) {
        CookedManifest manifest(allocator);
        REQUIRE(MakeBase(manifest));
        violation.mutate(manifest);
        auto same = [&](const AssetDiagnostic& failure) {
            return CorruptAt(failure, violation.field, violation.detail) &&
                   SameIds(failure, violation.asset, violation.related);
        };
        AssetDiagnostic validated;
        CHECK(!ValidateCookedManifest(manifest, allocator, &validated));
        Array<uint8> out = Prefilled(allocator);
        AssetDiagnostic written;
        CHECK(WriteCookedManifest(allocator, manifest, out, &written).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(IsPrefilled(out));
        AssetDiagnostic read;
        CHECK(!ReadCookedManifest(allocator, EncodeManifest(allocator, manifest).View(), &read));
        if (!same(validated) || !same(written) || !same(read)) {
            test::RecordFailure(violation.name, __FILE__, __LINE__);
        }
    }

    // 경계값은 받는다.
    CookedManifest edge(allocator);
    REQUIRE(MakeBase(edge));
    edge.entries[1].packageBytes = PackageHeaderBytes;
    edge.entries[2].packageBytes = PackageMaxFileBytes;
    edge.entries[2].bulkRawBytes = NumericLimits<uint64>::Max();
    edge.entries[0].bundles.Clear();
    for (uint32 index = 0; index < CookedManifestMaxBundles; ++index) {
        char name[5] = {'B', static_cast<char>('a' + index / 26 / 26 % 26),
                        static_cast<char>('a' + index / 26 % 26),
                        static_cast<char>('a' + index % 26), 0};
        REQUIRE(AddBundle(edge.entries[0], name));
    }

    for (uint32 index = 0; index < CookedManifestMaxDependencies; ++index) {
        REQUIRE(AddDependency(edge.entries[2], 1000 + index, PackageDependencyKind::Soft));
    }

    SetPathOfLength(edge, 1, CookedManifestMaxPathBytes);
    REQUIRE(edge.entries[1].pathHint.View().ByteCount() == CookedManifestMaxPathBytes);
    edge.profileId.Clear();
    for (uint32 index = 0; index < PackageMaxStringBytes; ++index) {
        REQUIRE(edge.profileId.TryAppend("p"));
    }

    constexpr uint16 MaxEnginePart = NumericLimits<uint16>::Max();
    edge.cookedBy = EngineVersion{MaxEnginePart, MaxEnginePart, MaxEnginePart};
    Array<uint8> bytes(allocator);
    AssetDiagnostic failure;
    REQUIRE(WriteCookedManifest(allocator, edge, bytes, &failure));
    auto read = ReadCookedManifest(allocator, bytes.View(), &failure);
    REQUIRE(read);
    CHECK(SameManifest(read.Value(), edge));
}

// 구조 변조(CRC를 다시 맞춘다 — AS 10.1절 2·3): magic 네 byte, formatVersion(0·2·최댓값 —
// UnsupportedVersion), CRC 한 bit, 최소 길이 미만(모든 길이), 개수 칸(항목 수가 실제보다 크거나
// 작음· 상한 + 1·남은 byte로 담을 수 없음), 프로파일 길이 칸(4 KiB 초과·남은 byte보다 큼), 엔진
// 버전 칸 (u16 초과 — 셋 각각), bundle·의존 개수 칸(상한 + 1·남은 byte 초과), 모르는 의존 kind,
// 남는 byte, 모든 길이로 자르기.
TEST(CookedManifest_StructureTampering) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest manifest(allocator);
    REQUIRE(MakeBase(manifest));
    const Array<uint8> valid = EncodeManifest(allocator, manifest);
    REQUIRE(ReadCookedManifest(allocator, valid.View()));
    auto expect = [&](const Array<uint8>& bytes, CookedManifestField field, uint64 detail) {
        AssetDiagnostic failure;
        return !ReadCookedManifest(allocator, bytes.View(), &failure) &&
               CorruptAt(failure, field, detail);
    };
    auto withRaw = [&](auto&& change) {
        ManifestRaw raw;
        change(raw);
        return EncodeManifest(allocator, manifest, raw);
    };

    for (usize index = 0; index < 4; ++index) {
        CHECK(expect(withRaw([&](ManifestRaw& raw) { raw.magic[index] ^= 1; }), F::Magic, Head));
    }

    const uint32 versions[] = {0, CookedManifestFormatVersion + 1, NumericLimits<uint32>::Max()};
    for (uint32 version : versions) {
        AssetDiagnostic failure;
        CHECK(ReadCookedManifest(
                  allocator, withRaw([&](ManifestRaw& raw) { raw.formatVersion = version; }).View(),
                  &failure)
                  .Error() == ErrorCode::Unsupported);
        CHECK(failure.code == AssetDiagnosticCode::UnsupportedVersion &&
              failure.error == ErrorCode::Unsupported &&
              failure.section == static_cast<uint32>(F::FormatVersion) && failure.detail == Head &&
              failure.asset.IsNull());
    }

    Array<uint8> flipped = Copy(allocator, valid.View());
    flipped[flipped.Size() - 1] ^= 0x80;
    CHECK(expect(flipped, F::Crc, Head));
    // CRC를 구조보다 먼저 본다: 개수 칸 최상위 byte를 0xFF로 바꾸면 CRC를 다시 맞춘 byte는
    // EntryCount, 맞추지 않은 byte는 Crc다.
    constexpr usize EntryCountTopByte = 11;
    flipped = Copy(allocator, valid.View());
    flipped[EntryCountTopByte] = 0xFF;
    CHECK(expect(flipped, F::Crc, Head));
    FixManifestCrc(flipped);
    CHECK(expect(flipped, F::EntryCount, Head));

    for (usize length = 0; length < 16; ++length) {
        CHECK(expect(Copy(allocator, valid.View().Slice(0, length)), F::File, Head));
    }

    // 상한 + 1·남은 byte로 담을 수 없는 수는 개수 칸, 실제보다 작으면 남는 byte, 실제보다 크면 없는
    // 항목을 읽다 잘린다(그 칸).
    const uint32 counts[] = {CookedManifestMaxEntries + 1, 1000};
    for (uint32 count : counts) {
        CHECK(expect(withRaw([&](ManifestRaw& raw) {
                         raw.overrideCount = true;
                         raw.entryCount = count;
                     }),
                     F::EntryCount, Head));
    }

    CHECK(expect(withRaw([&](ManifestRaw& raw) {
                     raw.overrideCount = true;
                     raw.entryCount = 2;
                 }),
                 F::File, Head));
    {
        AssetDiagnostic failure;
        CHECK(!ReadCookedManifest(allocator, withRaw([&](ManifestRaw& raw) {
                                                 raw.overrideCount = true;
                                                 raw.entryCount = 4;
                                             }).View(),
                                  &failure));
        CHECK(failure.code == AssetDiagnosticCode::CorruptPackage && failure.detail == 3);
    }

    const uint64 lengths[] = {uint64{PackageMaxStringBytes} + 1, 1u << 20, PackageMaxStringBytes};
    for (uint64 length : lengths) {
        CHECK(expect(withRaw([&](ManifestRaw& raw) {
                         raw.overrideProfileLength = true;
                         raw.profileLength = length;
                     }),
                     F::ProfileId, Head));
    }

    for (uint32 part = 0; part < 3; ++part) {
        CHECK(expect(withRaw([&](ManifestRaw& raw) {
                         raw.enginePart = part;
                         raw.engineValue = 0x10000;
                     }),
                     F::EngineVersion, Head));
    }

    const uint32 bundleCounts[] = {CookedManifestMaxBundles + 1, 100000};
    for (uint32 count : bundleCounts) {
        CHECK(expect(withRaw([&](ManifestRaw& raw) {
                         raw.overrideBundleCount = true;
                         raw.bundleCount = count;
                     }),
                     F::BundleCount, 0));
    }

    const uint32 dependencyCounts[] = {CookedManifestMaxDependencies + 1, 50000};
    for (uint32 count : dependencyCounts) {
        CHECK(expect(withRaw([&](ManifestRaw& raw) {
                         raw.overrideDependencyCount = true;
                         raw.dependencyCount = count;
                     }),
                     F::DependencyCount, 0));
    }

    const uint32 kinds[] = {0, 3, 4, 5, 6, NumericLimits<uint32>::Max()};
    for (uint32 kind : kinds) {
        CHECK(expect(withRaw([&](ManifestRaw& raw) {
                         raw.overrideKind = true;
                         raw.kind = kind;
                     }),
                     F::Dependency, 0));
    }

    CHECK(expect(withRaw([&](ManifestRaw& raw) { raw.trailing = 1; }), F::File, Head));

    // 잘못된 UTF-8(모델 String은 담을 수 없어 byte로 넣는다): 프로파일·경로·bundle의 첫 byte를
    // 0xFF로.
    auto patched = [&](StringView text) {
        Array<uint8> bytes = Copy(allocator, valid.View());
        for (usize at = 0; at + text.ByteCount() <= bytes.Size(); ++at) {
            bool same = true;
            for (usize index = 0; same && index < text.ByteCount(); ++index) {
                same = bytes[at + index] == static_cast<uint8>(text.Data()[index]);
            }

            if (same) {
                bytes[at] = 0xFF;
                break;
            }
        }

        FixManifestCrc(bytes);
        return bytes;
    };
    CHECK(expect(patched("Windows"), F::ProfileId, Head));
    CHECK(expect(patched("/Game/Hero"), F::PathHint, 0));
    CHECK(expect(patched("Core"), F::Bundle, 0));

    // 파일 상한 초과는 내용을 읽기 전에 거부한다(길이만 큰 span — byte에 닿지 않는다). 이 시험은
    // 읽기 전에 길이로 거부하는 순서에 기댄다. 순서가 바뀌면 경계 밖 읽기가 된다.
    CHECK(expect(Array<uint8>(allocator), F::File, Head));
    {
        AssetDiagnostic failure;
        const Span<const uint8> huge(valid.Data(),
                                     static_cast<usize>(CookedManifestMaxFileBytes) + 1);
        CHECK(!ReadCookedManifest(allocator, huge, &failure) && CorruptAt(failure, F::File, Head));
    }

    // 항목이 없는 매니페스트를 모든 길이로 자른다 — 개수 칸이 0이라 헤더 칸마다 잘림에 닿는다.
    CookedManifest empty(allocator);
    FillManifestHeader(empty);
    const Array<uint8> emptyBytes = EncodeManifest(allocator, empty);
    const CookedManifestField headerFields[] = {F::ProfileId, F::ProfileSettings, F::CookSettings,
                                                F::CookerVersion, F::EngineVersion};
    bool seen[5] = {};
    for (usize length = 16; length < emptyBytes.Size(); ++length) {
        Array<uint8> cut = Copy(allocator, emptyBytes.View().Slice(0, length));
        FixManifestCrc(cut);
        AssetDiagnostic failure;
        CHECK(!ReadCookedManifest(allocator, cut.View(), &failure) &&
              failure.code == AssetDiagnosticCode::CorruptPackage);
        for (usize field = 0; field < 5; ++field) {
            seen[field] =
                seen[field] || failure.section == static_cast<uint32>(headerFields[field]);
        }
    }

    for (bool hit : seen) {
        CHECK(hit);
    }

    // 모든 길이로 자르고 CRC를 다시 맞춘다 — 언제나 CorruptPackage다(어느 칸인지는 자른 위치).
    uint32 truncated = 0;
    for (usize length = 16; length < valid.Size(); ++length) {
        Array<uint8> cut = Copy(allocator, valid.View().Slice(0, length));
        FixManifestCrc(cut);
        AssetDiagnostic failure;
        auto result = ReadCookedManifest(allocator, cut.View(), &failure);
        CHECK(!result && failure.code == AssetDiagnosticCode::CorruptPackage &&
              failure.error == ErrorCode::InvalidArgument);
        ++truncated;
    }

    CHECK(truncated == valid.Size() - 16);
}

// 항목 칸 잘림(AS 10.1절 2·3): 항목 둘인 매니페스트를 항목 1 안의 모든 위치로 자르고 CRC를 다시
// 맞추면, 자른 위치가 속한 칸의 CorruptPackage(항목 1)다. 칸 범위는 아래 표이고, 개수 칸은 남은
// byte 검사를 포함한다 — bundle 개수 칸은 첫 bundle 길이 칸까지(개수 1 × 최소 8 byte가 남지 않으면
// 개수 칸에서 거부), 의존 개수 칸은 의존 byte 전부(개수를 남은 byte로 묶어 의존 칸은 잘리지
// 않는다). 항목 0의 경로·bundle을 길게 해 항목 1 안에서 잘라도 항목 수 칸 검사(항목 2개 × 최소
// byte)를 넘는다.
TEST(CookedManifest_EntryTruncation) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest manifest(allocator);
    FillManifestHeader(manifest);
    CookedManifestEntry* first =
        AddEntry(manifest, 10, "LongNameToKeepEntryCountCheckSatisfiedWhenCuttingTheNextOne",
                 static_cast<uint32>(CookedManifestFlags::PrimaryAsset));
    REQUIRE(first != nullptr && AddBundle(*first, "CoreBundleWithALongName"));
    CookedManifestEntry* second =
        AddEntry(manifest, 11, "Sword", static_cast<uint32>(CookedManifestFlags::PrimaryAsset));
    REQUIRE(second != nullptr && AddBundle(*second, "Blade") &&
            AddDependency(*second, 10, PackageDependencyKind::Hard) &&
            AddDependency(*second, 99, PackageDependencyKind::Soft));
    Array<uint8> valid(allocator);
    REQUIRE(WriteCookedManifest(allocator, manifest, valid));

    constexpr usize GuidBytes = 16;
    constexpr usize DigestBytes = Sha256Digest::Bytes;
    constexpr usize LengthBytes = sizeof(uint64);
    constexpr usize CountBytes = sizeof(uint32);
    constexpr usize DependencyBytes = GuidBytes + sizeof(uint32);
    const CookedManifestEntry& entry = manifest.entries[1];
    struct Range {
        CookedManifestField field;
        usize bytes;
    };
    const Range ranges[] = {
        {F::AssetId, GuidBytes},
        {F::TypeId, GuidBytes},
        {F::PathHint, LengthBytes + entry.pathHint.View().ByteCount()},
        {F::PackageSha256, DigestBytes},
        {F::PackageBytes, sizeof(uint64)},
        {F::BulkRawBytes, sizeof(uint64)},
        {F::CookKey, DigestBytes},
        {F::Flags, sizeof(uint32)},
        {F::BundleCount, CountBytes + LengthBytes},
        {F::Bundle, entry.bundles[0].View().ByteCount()},
        {F::DependencyCount, CountBytes + entry.dependencies.Size() * DependencyBytes},
    };
    usize entryBytes = 0;
    for (const Range& range : ranges) {
        entryBytes += range.bytes;
    }

    // 항목 1은 CRC 바로 앞에서 끝난다.
    const usize body = valid.Size() - sizeof(uint32);
    usize at = body - entryBytes;
    for (const Range& range : ranges) {
        bool all = true;
        for (usize end = at; end < at + range.bytes; ++end) {
            Array<uint8> cut = Copy(allocator, valid.View().Slice(0, end + sizeof(uint32)));
            FixManifestCrc(cut);
            AssetDiagnostic failure;
            const AssetId asset = range.field == F::AssetId ? AssetId{} : FixedId(11);
            all = all && !ReadCookedManifest(allocator, cut.View(), &failure) &&
                  CorruptAt(failure, range.field, 1) && failure.asset == asset;
        }

        if (!all) {
            test::RecordFailure("entry field truncation", __FILE__, __LINE__);
        }

        at += range.bytes;
    }

    CHECK(at == body);
}

// 할당 실패(모든 지점): 쓰기·읽기·검증이 OutOfMemory(진단 None)로 끝나고 쓰기 out은 그대로다.
// 장면에는 NFC quick check가 MAYBE인 경로·bundle(U+1EA1 + U+0301 — 합칠 수 없어 NFC다)이 있어 NFC
// 확인 scratch 할당과 비ASCII caseless 키(정규화·folding) 할당 지점도 지난다.
TEST(CookedManifest_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest manifest(allocator);
    REQUIRE(MakeBase(manifest));
    // 비교 키 builder(내장 7 byte, 두 배씩 자람)가 구성 요소 키에서도, 다음 구분자에서도 자라게
    // 한다: 첫 구성 요소 키가 용량을 꼭 채우는 길이(6·13·27 → "/" 포함 7·14·28)와 그 사이 길이. 긴
    // bundle 이름도 키에서 자란다.
    SetPath(manifest, 1, "/Conten/Sword");
    SetPath(manifest, 2, "/GameContentXY/Shield");
    REQUIRE(AddEntry(manifest, 13, "Mid") != nullptr);
    SetPath(manifest, 3, "/GameContentWithLongMountXYZ/Mid");
    REQUIRE(AddEntry(manifest, 14, "Other") != nullptr);
    SetPath(manifest, 4, "/Content/O\xE1\xBA\xA1\xCC\x81");
    SetBundles(manifest, "Core", "Expansion1");
    REQUIRE(AddBundle(manifest.entries[0], "G\xE1\xBA\xA1\xCC\x81"));
    Array<uint8> bytes(allocator);
    REQUIRE(WriteCookedManifest(allocator, manifest, bytes));
    test::NFailAllocator failing(allocator);
    uint32 counts[3] = {};
    for (uint32 operation = 0; operation < 3; ++operation) {
        for (uint64 attempt = 1;; ++attempt) {
            failing.FailOnAttempt(attempt);
            AssetDiagnostic failure;
            Result<void> result = Result<void>::Success();
            Array<uint8> out = Prefilled(allocator);
            if (operation == 0) {
                result = WriteCookedManifest(failing, manifest, out, &failure);
            } else if (operation == 1) {
                auto read = ReadCookedManifest(failing, bytes.View(), &failure);
                result = read ? Result<void>::Success() : Result<void>::Failure(read.Error());
            } else {
                result = ValidateCookedManifest(manifest, failing, &failure);
            }

            if (failing.Attempts() < attempt) {
                REQUIRE(result);
                break;
            }

            CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
            CHECK(failure.code == AssetDiagnosticCode::None);
            CHECK(IsPrefilled(out));
            ++counts[operation];
        }

        failing.DisableFailure();
        CHECK(counts[operation] > 0);
    }
}

// writer 파일 상한(AS 10.1절 7): 상한은 CRC 칸까지 포함한 파일 전체다. 상한을 인자로 받는 내부
// 함수로 경계를 본다 — 출력 크기와 같은 상한은 받고(공개 writer와 같은 byte), 1 작으면
// CapacityExceeded(진단 None, out 그대로)다.
TEST(CookedManifest_WriteLimit) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest manifest(allocator);
    REQUIRE(MakeBase(manifest));
    Array<uint8> bytes(allocator);
    REQUIRE(WriteCookedManifest(allocator, manifest, bytes));
    Array<uint8> exact(allocator);
    AssetDiagnostic failure;
    REQUIRE(package_detail::WriteCookedManifestWithLimit(allocator, manifest, bytes.Size(), exact,
                                                         &failure));
    CHECK(SameBytes(exact.View(), bytes.View()));

    Array<uint8> out = Prefilled(allocator);
    CHECK(package_detail::WriteCookedManifestWithLimit(allocator, manifest, bytes.Size() - 1, out,
                                                       &failure)
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(failure.code == AssetDiagnosticCode::None &&
          failure.error == ErrorCode::CapacityExceeded);
    CHECK(IsPrefilled(out));
}

namespace {

// 가장 큰 할당 요청 크기를 기록하고 아래 allocator로 넘긴다.
class LargestRequestAllocator final : public Allocator {
public:
    explicit LargestRequestAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        largest_ = bytes > largest_ ? bytes : largest_;
        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

    usize Largest() const noexcept {
        return largest_;
    }

private:
    Allocator& inner_;
    usize largest_ = 0;
};

} // namespace

// 할당 상한(AS 10.1절 3): 개수 칸이 큰 입력(항목 1000·bundle 100000·의존 50000 — 남은 byte로 담을
// 수 없다)도 reader의 가장 큰 할당 요청이 입력 크기의 작은 배수 이하다(개수 칸 값만큼 미리 잡지
// 않는다). 정상 입력도 같은 한도 안이다.
TEST(CookedManifest_AllocationBound) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest manifest(allocator);
    REQUIRE(MakeBase(manifest));
    // 가장 큰 요청 / 입력 byte의 한도다(항목 모델이 최소 항목 byte의 두 배를 넘지 않는다).
    constexpr usize MaxRequestPerInputByte = 4;
    auto largest = [&](const Array<uint8>& bytes, bool accepted) {
        LargestRequestAllocator recording(allocator);
        auto read = ReadCookedManifest(recording, bytes.View());
        CHECK(read.IsOk() == accepted);
        return recording.Largest() <= MaxRequestPerInputByte * bytes.Size();
    };
    CHECK(largest(EncodeManifest(allocator, manifest), true));

    ManifestRaw entries;
    entries.overrideCount = true;
    entries.entryCount = 1000;
    CHECK(largest(EncodeManifest(allocator, manifest, entries), false));
    ManifestRaw bundles;
    bundles.overrideBundleCount = true;
    bundles.bundleCount = 100000;
    CHECK(largest(EncodeManifest(allocator, manifest, bundles), false));
    ManifestRaw dependencies;
    dependencies.overrideDependencyCount = true;
    dependencies.dependencyCount = 50000;
    CHECK(largest(EncodeManifest(allocator, manifest, dependencies), false));
}

// 프로파일·병합 대조(AS 10.1절 8): 실행 프로파일이 다르면 IncompatibleCook, 병합은 네 칸 각각이
// 다르면 IncompatibleCook(그 칸)이고 엔진 버전·항목은 보지 않는다.
TEST(CookedManifest_ProfileAndMergeChecks) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest existing(allocator);
    REQUIRE(MakeBase(existing));
    CHECK(CheckCookedManifestProfile(existing, "Windows"));
    AssetDiagnostic failure;
    CHECK(CheckCookedManifestProfile(existing, "Android", &failure).Error() ==
          ErrorCode::Unsupported);
    CHECK(failure.code == AssetDiagnosticCode::IncompatibleCook &&
          failure.section == static_cast<uint32>(F::ProfileId));
    CHECK(!CheckCookedManifestProfile(existing, "windows"));

    for (uint32 change = 0; change < 6; ++change) {
        CookedManifest incoming(allocator);
        FillManifestHeader(incoming);
        CookedManifestField expected = F::File;
        if (change == 0) {
            incoming.profileId.Clear();
            (void)incoming.profileId.TryAppend("Android");
            expected = F::ProfileId;
        } else if (change == 1) {
            incoming.profileSettings.bytes[31] ^= 1;
            expected = F::ProfileSettings;
        } else if (change == 2) {
            incoming.cookSettings.bytes[0] ^= 1;
            expected = F::CookSettings;
        } else if (change == 3) {
            ++incoming.cookerVersion;
            expected = F::CookerVersion;
        } else if (change == 4) {
            // 엔진 버전은 병합 판정에 쓰지 않는다.
            ++incoming.cookedBy.majorVersion;
        }

        AssetDiagnostic merge;
        auto result = CheckCookedManifestMerge(existing, incoming, &merge);
        if (expected == F::File) {
            CHECK(result);
        } else {
            CHECK(result.Error() == ErrorCode::Unsupported &&
                  merge.code == AssetDiagnosticCode::IncompatibleCook &&
                  merge.section == static_cast<uint32>(expected));
        }
    }
}

namespace {

// 쿠킹 패키지 장면: B(1801), A(1800 — 대표 에셋 Hard → B, Soft → 1700, Zstd bulk와 쿠킹에서 빠지는
// EditorOnly bulk), C(1802 — 같은 대상 B를 Hard와 Soft로)를 A1a writer로 쓰고 매니페스트를 그
// 결과에서 만든다. A의 Soft 대상 id가 Hard 대상보다 작아 의존 순서가 (asset, kind)이지 종류 먼저가
// 아님이 드러난다.
struct CookedScene {
    explicit CookedScene(Allocator& allocator) noexcept
        : a(allocator), b(allocator), c(allocator), pixels(allocator), data(allocator),
          manifest(allocator) {
    }

    Array<uint8> a;
    Array<uint8> b;
    Array<uint8> c;
    Array<uint8> pixels;
    Array<uint8> data;
    CookedManifest manifest;
};

bool FillEntry(CookedManifestEntry& entry, Allocator& allocator, const Array<uint8>& file,
               StringView path) noexcept {
    auto read = ReadPackage(allocator, file.View(), PackageReadOptions{});
    if (!read) {
        return false;
    }

    entry.asset = read.Value().header.assetId;
    entry.type = read.Value().exports[0].type;
    entry.packageBytes = file.Size();
    entry.packageSha256 = Sha256::Hash(file.View());
    entry.bulkRawBytes = 0;
    for (const PackageBulkEntry& bulk : read.Value().bulkEntries) {
        entry.bulkRawBytes += bulk.rawBytes;
    }

    for (const PackageDependencyEntry& dependency : read.Value().dependencies) {
        if (!entry.dependencies
                 .TryPushBack(CookedManifestDependency{dependency.asset, dependency.kind})
                 .IsOk()) {
            return false;
        }
    }

    return entry.pathHint.TryAssign(path).IsOk();
}

bool MakeCookedScene(AssetEnv& env, CookedScene& scene) noexcept {
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    auto b = NewPackage(source, access, FixedId(1801), PackageKind::Cooked);
    auto a = NewPackage(source, access, FixedId(1800), PackageKind::Cooked);
    auto c = NewPackage(source, access, FixedId(1802), PackageKind::Cooked);
    if (!a || !b || !c) {
        return false;
    }

    const ObjectHandle bAsset = Make<TestAsset>(source, TestAssetType(), b.Value().Handle(), "");
    const ObjectHandle aAsset = Make<TestAsset>(source, TestAssetType(), a.Value().Handle(), "");
    const ObjectHandle cAsset = Make<TestAsset>(source, TestAssetType(), c.Value().Handle(), "");
    TestAsset* pointer = Pointer<TestAsset>(source, aAsset);
    TestAsset* both = Pointer<TestAsset>(source, cAsset);
    if (bAsset.IsNull() || pointer == nullptr || both == nullptr ||
        !source.SetReference(access, aAsset, pointer->hard, bAsset) ||
        !pointer->soft.TryAssign(FixedId(1700), StringView()) ||
        !source.SetReference(access, cAsset, both->hard, bAsset) ||
        !both->soft.TryAssign(FixedId(1801), StringView())) {
        return false;
    }

    FillCompressible(scene.pixels, 3000, 5);
    FillIncompressible(scene.data, 200, 6);
    TestBulkSource bulk;
    pointer->pixels = bulk.Add(1, scene.pixels.View());
    pointer->source = bulk.Add(2, scene.data.View());
    PackageWriteOptions options = CookedOptions();
    options.bulkSource = &bulk;
    options.bulkCodec = BulkCodec::Zstd;
    scene.a = Write(env.allocator, source, a.Value().Handle(), options);
    scene.b = Write(env.allocator, source, b.Value().Handle(), CookedOptions());
    scene.c = Write(env.allocator, source, c.Value().Handle(), CookedOptions());
    if (scene.a.IsEmpty() || scene.b.IsEmpty() || scene.c.IsEmpty()) {
        return false;
    }

    FillManifestHeader(scene.manifest);
    constexpr uint32 SceneEntries = 3;
    for (uint32 index = 0; index < SceneEntries; ++index) {
        if (!scene.manifest.entries.TryEmplaceBack(env.allocator)) {
            return false;
        }
    }

    return FillEntry(scene.manifest.entries[0], env.allocator, scene.a, "/Game/Hero") &&
           FillEntry(scene.manifest.entries[1], env.allocator, scene.b, "/Game/Sword") &&
           FillEntry(scene.manifest.entries[2], env.allocator, scene.c, "/Game/Shield") &&
           ValidateCookedManifest(scene.manifest, env.allocator).IsOk();
}

} // namespace

// 패키지 대조(AS 10.1절 8): A1a writer가 쓴 쿠킹 패키지는 그 결과로 만든 항목과 맞는다(Hard·Soft
// 의존 — Soft 대상 id가 Hard 대상보다 작은 (asset, kind) 순서와 같은 대상의 Hard+Soft — 와 Zstd
// bulk raw 합 포함). 칸마다 하나씩 다르면 그 칸의 CorruptPackage다 — 크기(한 byte 덧붙임·항목
// 크기), SHA-256(한 byte), 종류(에디터 패키지), assetId, 대표 에셋 타입, bulk raw 합(하나
// 작음·u64를 넘는 합), 프로파일(id·설정·쿠커 버전), 의존(빠짐·남음·종류·대상·순서 — related는 처음
// 다른 의존의 대상).
TEST(CookedManifest_PackageCrossCheck) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedScene scene(allocator);
    REQUIRE(MakeCookedScene(env, scene));
    const CookedManifest& manifest = scene.manifest;
    auto contents = ReadPackage(allocator, scene.a.View(), PackageReadOptions{});
    REQUIRE(contents);
    const CookedManifestEntry& entry = manifest.entries[0];
    // EditorOnly bulk(source)는 쿠킹하지 않으므로 raw 합은 pixels뿐이다.
    REQUIRE(entry.dependencies.Size() == 2);
    REQUIRE(entry.bulkRawBytes == scene.pixels.Size());
    // (asset, kind) 순서: Soft → 1700이 Hard → 1801보다 앞이다(종류 먼저라면 반대).
    CHECK(entry.dependencies[0].asset == FixedId(1700) &&
          entry.dependencies[0].kind == PackageDependencyKind::Soft &&
          entry.dependencies[1].asset == FixedId(1801) &&
          entry.dependencies[1].kind == PackageDependencyKind::Hard);
    CHECK(CheckCookedPackage(manifest, entry, scene.a.View(), contents.Value()));
    auto bContents = ReadPackage(allocator, scene.b.View(), PackageReadOptions{});
    REQUIRE(bContents);
    CHECK(CheckCookedPackage(manifest, manifest.entries[1], scene.b.View(), bContents.Value()));
    auto cContents = ReadPackage(allocator, scene.c.View(), PackageReadOptions{});
    REQUIRE(cContents);
    const CookedManifestEntry& sameTarget = manifest.entries[2];
    CHECK(sameTarget.dependencies.Size() == 2 &&
          sameTarget.dependencies[0].asset == FixedId(1801) &&
          sameTarget.dependencies[0].kind == PackageDependencyKind::Hard &&
          sameTarget.dependencies[1].asset == FixedId(1801) &&
          sameTarget.dependencies[1].kind == PackageDependencyKind::Soft);
    CHECK(CheckCookedPackage(manifest, sameTarget, scene.c.View(), cContents.Value()));

    auto expect = [&](const CookedManifest& m, const CookedManifestEntry& e, Span<const uint8> file,
                      const PackageContents& c, CookedManifestField field, uint64 related = NoId) {
        AssetDiagnostic failure;
        return CheckCookedPackage(m, e, file, c, &failure).Error() == ErrorCode::InvalidArgument &&
               CorruptAt(failure, field, CookedManifestNoEntry) && failure.asset == e.asset &&
               failure.related == ExpectedId(related);
    };

    // 크기·SHA-256(파일 쪽 변조).
    Array<uint8> longer = Copy(allocator, scene.a.View());
    REQUIRE(longer.TryPushBack(0));
    CHECK(expect(manifest, entry, longer.View(), contents.Value(), F::PackageBytes));
    Array<uint8> flipped = Copy(allocator, scene.a.View());
    flipped[100] ^= 1;
    CHECK(expect(manifest, entry, flipped.View(), contents.Value(), F::PackageSha256));

    // 항목 쪽 칸 하나씩(file의 결과로 항목을 새로 만들어 바꾼다).
    auto changed = [&](const Array<uint8>& file, const PackageContents& c, auto&& change,
                       CookedManifestField field, uint64 related) {
        CookedManifest copy(allocator);
        FillManifestHeader(copy);
        auto added = copy.entries.TryEmplaceBack(allocator);
        if (!added) {
            return false;
        }

        CookedManifestEntry& e = *added.Value();
        if (!FillEntry(e, allocator, file, "/Game/Hero")) {
            return false;
        }

        change(copy, e);
        return expect(copy, e, file.View(), c, field, related);
    };
    auto changedA = [&](auto&& change, CookedManifestField field, uint64 related = NoId) {
        return changed(scene.a, contents.Value(), change, field, related);
    };
    CHECK(changedA([](CookedManifest&, CookedManifestEntry& e) { e.packageBytes += 1; },
                   F::PackageBytes));
    CHECK(changedA([](CookedManifest&, CookedManifestEntry& e) { e.packageSha256.bytes[0] ^= 1; },
                   F::PackageSha256));
    CHECK(changedA([](CookedManifest&, CookedManifestEntry& e) { e.asset = FixedId(1803); },
                   F::AssetId));
    CHECK(changedA([](CookedManifest&, CookedManifestEntry& e) { e.type = TestPartType(); },
                   F::TypeId));
    CHECK(changedA([](CookedManifest&, CookedManifestEntry& e) { e.bulkRawBytes -= 1; },
                   F::BulkRawBytes));
    CHECK(changedA(
        [](CookedManifest& m, CookedManifestEntry&) {
            m.profileId.Clear();
            (void)m.profileId.TryAppend("Android");
        },
        F::PackageProfile));
    CHECK(changedA([](CookedManifest& m, CookedManifestEntry&) { m.profileSettings.bytes[0] ^= 1; },
                   F::PackageProfile));
    CHECK(changedA([](CookedManifest& m, CookedManifestEntry&) { ++m.cookerVersion; },
                   F::PackageProfile));
    // 의존: 패키지는 [Soft 1700, Hard 1801]이다.
    CHECK(
        changedA([](CookedManifest&, CookedManifestEntry& e) { (void)e.dependencies.TryPopBack(); },
                 F::PackageDependencies, 1801));
    CHECK(changedA(
        [](CookedManifest&, CookedManifestEntry& e) {
            (void)e.dependencies.TryPushBack(
                CookedManifestDependency{FixedId(1900), PackageDependencyKind::Soft});
        },
        F::PackageDependencies, 1900));
    CHECK(changedA(
        [](CookedManifest&, CookedManifestEntry& e) {
            e.dependencies[1].kind = PackageDependencyKind::Soft;
        },
        F::PackageDependencies, 1801));
    CHECK(changedA(
        [](CookedManifest&, CookedManifestEntry& e) { e.dependencies[1].asset = FixedId(1899); },
        F::PackageDependencies, 1899));
    CHECK(changedA([](CookedManifest&, CookedManifestEntry& e) { e.dependencies.Clear(); },
                   F::PackageDependencies, 1700));
    // 종류 먼저 순서로 적은 항목([Hard 1801, Soft 1700])은 다르다.
    CHECK(changedA(
        [](CookedManifest&, CookedManifestEntry& e) {
            const CookedManifestDependency soft = e.dependencies[0];
            e.dependencies[0] = e.dependencies[1];
            e.dependencies[1] = soft;
        },
        F::PackageDependencies, 1801));

    // 같은 대상 Hard+Soft 패키지: Soft가 빠지거나 두 종류의 순서가 바뀌면 다르다.
    CHECK(changed(
        scene.c, cContents.Value(),
        [](CookedManifest&, CookedManifestEntry& e) { (void)e.dependencies.TryPopBack(); },
        F::PackageDependencies, 1801));
    CHECK(changed(
        scene.c, cContents.Value(),
        [](CookedManifest&, CookedManifestEntry& e) {
            e.dependencies[0].kind = PackageDependencyKind::Soft;
            e.dependencies[1].kind = PackageDependencyKind::Hard;
        },
        F::PackageDependencies, 1801));

    // bulk raw 합이 u64를 넘으면(호출자가 준 내용이 손상) 2⁶⁴로 감아 같은 값이 되어도 불일치다:
    // rawBytes를 (실제 + 2⁶³)과 2⁶³ 둘로 바꾼다.
    auto wrapped = ReadPackage(allocator, scene.a.View(), PackageReadOptions{});
    REQUIRE(wrapped && wrapped.Value().bulkEntries.Size() == 1);
    constexpr uint64 HalfRange = uint64{1} << 63;
    PackageBulkEntry extra = wrapped.Value().bulkEntries[0];
    extra.rawBytes = HalfRange;
    wrapped.Value().bulkEntries[0].rawBytes += HalfRange;
    REQUIRE(wrapped.Value().bulkEntries.TryPushBack(extra));
    CHECK(expect(manifest, entry, scene.a.View(), wrapped.Value(), F::BulkRawBytes));

    // 종류: 에디터 패키지(크기·SHA-256은 그 파일로 맞춘 항목).
    ObjectRegistry& source = *env.source;
    auto editor = NewPackage(source, source.OwnerAccess(), FixedId(1804), PackageKind::Editor);
    REQUIRE(editor);
    REQUIRE(!Make<TestAsset>(source, TestAssetType(), editor.Value().Handle(), "").IsNull());
    const Array<uint8> editorFile =
        Write(allocator, source, editor.Value().Handle(), EditorOptions());
    PackageReadOptions editorRead;
    editorRead.expectedKind = PackageKind::Editor;
    auto editorContents = ReadPackage(allocator, editorFile.View(), editorRead);
    REQUIRE(editorContents);
    CookedManifestEntry editorEntry(allocator);
    editorEntry.asset = FixedId(1804);
    editorEntry.type = TestAssetType();
    editorEntry.packageBytes = editorFile.Size();
    editorEntry.packageSha256 = Sha256::Hash(editorFile.View());
    CHECK(expect(manifest, editorEntry, editorFile.View(), editorContents.Value(), F::PackageKind));

    // export가 없는 내용(공개 API — 호출자가 다른 파일의 결과를 넘긴 경우)은 대표 에셋 타입 칸이다.
    PackageContents bare(allocator);
    bare.header.kind = PackageKind::Cooked;
    bare.header.assetId = entry.asset;
    CHECK(expect(manifest, entry, scene.a.View(), bare, F::TypeId));
}

// 판정 순서(AS 10.1절 2·8): 위반이 둘 이상인 입력은 먼저 보는 쪽의 진단이다. 각 입력은 적힌 칸
// 말고는 정상이고, 첫 줄 말고는 CRC를 다시 맞춘다(틀린 CRC가 적힌 줄은 그 뒤에 CRC를 깬다).
// reader는 구조(길이 → magic → formatVersion → CRC → 칸 읽기 → 끝 위치)를 다 본 뒤 칸 규칙을 보고,
// 검증· writer는 칸 규칙만 본다 — 그래서 여러 위반에서는 reader와 검증·writer가 다를 수 있다.
//   길이 10 + 잘못된 magic                          → File
//   magic·formatVersion 둘 다 잘못                   → Magic
//   formatVersion 2 + 틀린 CRC                      → UnsupportedVersion
//   빈 프로파일 + 틀린 CRC                           → Crc
//   남는 1 byte + 빈 프로파일                         → File
//   항목 0 flags=2 + 항목 1 bundle 257개              → reader BundleCount/1, 검증·writer Flags/0
//   경로 caseless 중복 + 쿠킹되지 않은 Hard 대상       → PathHint/2(세 경로 모두)
//   병합: profileId·cookerVersion 둘 다 다름          → ProfileId
//   패키지 대조: 길이가 다른 파일 + 다른 패키지 내용  → PackageBytes
//   패키지 대조: SHA만 다른 파일 + 다른 패키지 내용   → PackageSha256
TEST(CookedManifest_JudgementOrder) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest manifest(allocator);
    REQUIRE(MakeBase(manifest));
    auto read = [&](const Array<uint8>& bytes, AssetDiagnostic& failure) {
        return ReadCookedManifest(allocator, bytes.View(), &failure).IsOk();
    };
    auto readCorrupt = [&](const Array<uint8>& bytes, CookedManifestField field, uint64 detail) {
        AssetDiagnostic failure;
        return !read(bytes, failure) && CorruptAt(failure, field, detail);
    };
    auto breakCrc = [](Array<uint8>& bytes) { bytes[bytes.Size() - 1] ^= 1; };

    const Array<uint8> valid = EncodeManifest(allocator, manifest);
    constexpr usize ShortLength = 10;
    Array<uint8> shortBytes = Copy(allocator, valid.View().Slice(0, ShortLength));
    shortBytes[0] ^= 1;
    CHECK(readCorrupt(shortBytes, F::File, Head));

    ManifestRaw both;
    both.magic[0] ^= 1;
    both.formatVersion = CookedManifestFormatVersion + 1;
    CHECK(readCorrupt(EncodeManifest(allocator, manifest, both), F::Magic, Head));

    ManifestRaw version;
    version.formatVersion = CookedManifestFormatVersion + 1;
    Array<uint8> versionBytes = EncodeManifest(allocator, manifest, version);
    breakCrc(versionBytes);
    {
        AssetDiagnostic failure;
        CHECK(!read(versionBytes, failure) &&
              failure.code == AssetDiagnosticCode::UnsupportedVersion &&
              failure.error == ErrorCode::Unsupported &&
              failure.section == static_cast<uint32>(F::FormatVersion) && failure.detail == Head);
    }

    CookedManifest noProfile(allocator);
    REQUIRE(MakeBase(noProfile));
    noProfile.profileId.Clear();
    Array<uint8> noProfileBytes = EncodeManifest(allocator, noProfile);
    breakCrc(noProfileBytes);
    CHECK(readCorrupt(noProfileBytes, F::Crc, Head));
    ManifestRaw trailing;
    trailing.trailing = 1;
    CHECK(readCorrupt(EncodeManifest(allocator, noProfile, trailing), F::File, Head));

    // 구조 위반(항목 1 bundle 개수 칸)과 칸 규칙 위반(항목 0 flags): reader는 구조를 먼저 다 본다.
    CookedManifest mixed(allocator);
    REQUIRE(MakeBase(mixed));
    mixed.entries[0].flags = 2;
    mixed.entries[1].flags = static_cast<uint32>(CookedManifestFlags::PrimaryAsset);
    for (uint32 index = 0; index <= CookedManifestMaxBundles; ++index) {
        char name[5] = {'B', static_cast<char>('a' + index / 26 / 26 % 26),
                        static_cast<char>('a' + index / 26 % 26),
                        static_cast<char>('a' + index % 26), 0};
        REQUIRE(AddBundle(mixed.entries[1], name));
    }

    {
        AssetDiagnostic validated;
        CHECK(!ValidateCookedManifest(mixed, allocator, &validated) &&
              CorruptAt(validated, F::Flags, 0) && SameIds(validated, 10, NoId));
        Array<uint8> out = Prefilled(allocator);
        AssetDiagnostic written;
        CHECK(!WriteCookedManifest(allocator, mixed, out, &written) &&
              CorruptAt(written, F::Flags, 0) && IsPrefilled(out));
        AssetDiagnostic decoded;
        CHECK(!read(EncodeManifest(allocator, mixed), decoded) &&
              CorruptAt(decoded, F::BundleCount, 1) && SameIds(decoded, 11, NoId));
    }

    // 항목 사이 규칙: 경로 유일성을 Hard 대상 존재보다 먼저 본다(세 경로 모두).
    CookedManifest crossed(allocator);
    REQUIRE(MakeBase(crossed));
    SetPath(crossed, 2, "/game/HERO");
    SetDependency(crossed, 2, 99, PackageDependencyKind::Hard);
    {
        AssetDiagnostic validated;
        CHECK(!ValidateCookedManifest(crossed, allocator, &validated) &&
              CorruptAt(validated, F::PathHint, 2) && SameIds(validated, 12, NoId));
        Array<uint8> out = Prefilled(allocator);
        AssetDiagnostic written;
        CHECK(!WriteCookedManifest(allocator, crossed, out, &written) &&
              CorruptAt(written, F::PathHint, 2) && IsPrefilled(out));
        CHECK(readCorrupt(EncodeManifest(allocator, crossed), F::PathHint, 2));
    }

    // 병합: 네 칸 중 앞 칸(profileId)이다.
    CookedManifest incoming(allocator);
    FillManifestHeader(incoming);
    incoming.profileId.Clear();
    REQUIRE(incoming.profileId.TryAppend("Android"));
    ++incoming.cookerVersion;
    {
        AssetDiagnostic merge;
        CHECK(!CheckCookedManifestMerge(manifest, incoming, &merge) &&
              merge.code == AssetDiagnosticCode::IncompatibleCook &&
              merge.section == static_cast<uint32>(F::ProfileId));
    }

    // 패키지 대조: 크기·SHA-256을 내용(다른 패키지 B의 결과 — assetId·의존·bulk 합이 다르다)보다
    // 먼저 본다.
    CookedScene scene(allocator);
    REQUIRE(MakeCookedScene(env, scene));
    auto bContents = ReadPackage(allocator, scene.b.View(), PackageReadOptions{});
    REQUIRE(bContents);
    const CookedManifestEntry& entry = scene.manifest.entries[0];
    auto compare = [&](const Array<uint8>& file, CookedManifestField field) {
        AssetDiagnostic failure;
        return !CheckCookedPackage(scene.manifest, entry, file.View(), bContents.Value(),
                                   &failure) &&
               CorruptAt(failure, field, Head);
    };
    Array<uint8> longer = Copy(allocator, scene.a.View());
    REQUIRE(longer.TryPushBack(0));
    CHECK(compare(longer, F::PackageBytes));
    Array<uint8> flipped = Copy(allocator, scene.a.View());
    flipped[100] ^= 1;
    CHECK(compare(flipped, F::PackageSha256));
    // 같은 파일이면 그다음 칸(B의 assetId)에서 다르다 — 위 두 줄이 내용보다 먼저임을 대조한다.
    CHECK(compare(scene.a, F::AssetId));
}

// 패키지 파일 경로(AS 4.2절): Packages/<a>/<A>-<h>.rpkg — A는 36자 소문자, a는 그 앞 2자, h는
// SHA-256 앞 16자. 앞에 내용이 있는 builder에 붙인다.
TEST(CookedManifest_PackagePath) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StringBuilder<16> path(env.allocator);
    REQUIRE(path.TryAppend("root/"));
    const AssetId asset{Guid{0x0123456789ABCDEFull, 0xFEDCBA9876543210ull}};
    REQUIRE(AppendCookedPackagePath(path, asset, DigestOf(0xAB)));
    CHECK(path.View() == StringView("root/Packages/01/01234567-89ab-cdef-fedc-ba9876543210-"
                                    "abababababababab.rpkg"));

    // 할당 실패(모든 지점 — 식별자 서식과 out 성장)는 그 오류다.
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        StringBuilder<4> out(failing);
        auto appended = AppendCookedPackagePath(out, asset, DigestOf(0xAB));
        if (failing.Attempts() < attempt) {
            REQUIRE(appended);
            break;
        }

        CHECK(appended.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 1);
}

// 항목 수 상한(AS 10.1절 3): 2²⁰ + 1개 항목 모델은 검증·쓰기가 EntryCount 손상이다(항목 칸은 보기
// 전에 거부한다 — 빈 항목들이라 할당은 항목 배열 하나다). 비용: 그 배열(sizeof(CookedManifestEntry)
// × (2²⁰ + 1), 64-bit에서 216 byte × 1048577 ≈ 226 MB)을 한 번에 잡는다 — 이 시험에서 가장 큰
// 할당이다. reader 쪽 개수 칸 상한은 CookedManifest_StructureTampering이 본다.
TEST(CookedManifest_EntryCountLimit) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest manifest(allocator);
    FillManifestHeader(manifest);
    REQUIRE(manifest.entries.TryReserve(CookedManifestMaxEntries + 1));
    for (uint32 index = 0; index <= CookedManifestMaxEntries; ++index) {
        REQUIRE(manifest.entries.TryEmplaceBack(allocator));
    }

    AssetDiagnostic failure;
    CHECK(!ValidateCookedManifest(manifest, allocator, &failure));
    CHECK(CorruptAt(failure, F::EntryCount, Head));
    Array<uint8> out = Prefilled(allocator);
    CHECK(WriteCookedManifest(allocator, manifest, out, &failure).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(IsPrefilled(out) && CorruptAt(failure, F::EntryCount, Head));
}
