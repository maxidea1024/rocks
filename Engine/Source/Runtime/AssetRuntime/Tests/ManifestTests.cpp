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

// 규칙 위반 하나다: 기본 장면을 바꾸는 함수와 기대 칸·항목.
struct Violation {
    const char* name;
    void (*mutate)(CookedManifest&);
    CookedManifestField field;
    uint64 detail;
};

void SetPath(CookedManifest& manifest, usize index, StringView path) {
    manifest.entries[index].pathHint.Clear();
    (void)manifest.entries[index].pathHint.TryAppend(path);
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
    {"empty profile", [](CookedManifest& m) { m.profileId.Clear(); }, F::ProfileId, Head},
    {"long profile",
     [](CookedManifest& m) {
         m.profileId.Clear();
         for (uint32 index = 0; index <= PackageMaxStringBytes; ++index) {
             (void)m.profileId.TryAppend("p");
         }
     },
     F::ProfileId, Head},
    {"null asset", [](CookedManifest& m) { m.entries[0].asset = AssetId{}; }, F::AssetId, 0},
    {"duplicate asset", [](CookedManifest& m) { m.entries[1].asset = FixedId(10); }, F::AssetId, 1},
    {"unsorted assets", [](CookedManifest& m) { m.entries[2].asset = FixedId(5); }, F::AssetId, 2},
    {"null type", [](CookedManifest& m) { m.entries[1].type = TypeId{}; }, F::TypeId, 1},
    {"empty path", [](CookedManifest& m) { SetPath(m, 0, ""); }, F::PathHint, 0},
    {"relative path", [](CookedManifest& m) { SetPath(m, 0, "Game/Hero"); }, F::PathHint, 0},
    {"mount only", [](CookedManifest& m) { SetPath(m, 0, "/Game"); }, F::PathHint, 0},
    {"empty component", [](CookedManifest& m) { SetPath(m, 0, "/Game//Hero"); }, F::PathHint, 0},
    {"trailing slash", [](CookedManifest& m) { SetPath(m, 0, "/Game/Hero/"); }, F::PathHint, 0},
    {"space", [](CookedManifest& m) { SetPath(m, 0, "/Game/He ro"); }, F::PathHint, 0},
    {"dot", [](CookedManifest& m) { SetPath(m, 0, "/Game/Hero.Body"); }, F::PathHint, 0},
    {"reserved", [](CookedManifest& m) { SetPath(m, 0, "/Game/CON"); }, F::PathHint, 0},
    {"not NFC", [](CookedManifest& m) { SetPath(m, 0, "/Game/He\xCC\x81ro"); }, F::PathHint, 0},
    {"backslash", [](CookedManifest& m) { SetPath(m, 0, "/Game\\Hero"); }, F::PathHint, 0},
    {"long path",
     [](CookedManifest& m) {
         SetPath(m, 0, "/Game");
         // 64 byte 구성 요소 셋 + 구분자 = 256을 넘는다.
         for (uint32 part = 0; part < 4; ++part) {
             (void)m.entries[0].pathHint.TryAppend("/");
             for (uint32 index = 0; index < MaxNameComponentBytes; ++index) {
                 (void)m.entries[0].pathHint.TryAppend("a");
             }
         }
     },
     F::PathHint, 0},
    {"caseless duplicate path", [](CookedManifest& m) { SetPath(m, 2, "/game/HERO"); }, F::PathHint,
     2},
    {"three equal paths",
     [](CookedManifest& m) {
         SetPath(m, 1, "/GAME/hero");
         SetPath(m, 2, "/game/HERO");
     },
     F::PathHint, 1},
    {"small package", [](CookedManifest& m) { m.entries[1].packageBytes = PackageHeaderBytes - 1; },
     F::PackageBytes, 1},
    {"large package",
     [](CookedManifest& m) { m.entries[1].packageBytes = PackageMaxFileBytes + 1; },
     F::PackageBytes, 1},
    {"undefined flag", [](CookedManifest& m) { m.entries[2].flags = 2; }, F::Flags, 2},
    {"bundles without PrimaryAsset", [](CookedManifest& m) { m.entries[0].flags = 0; },
     F::BundleCount, 0},
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
     F::BundleCount, 0},
    {"invalid bundle", [](CookedManifest& m) { SetBundles(m, "Core", "H d"); }, F::Bundle, 0},
    {"unsorted bundles", [](CookedManifest& m) { SetBundles(m, "Hud", "Core"); }, F::Bundle, 0},
    {"duplicate bundle", [](CookedManifest& m) { SetBundles(m, "Hud", "Hud"); }, F::Bundle, 0},
    {"caseless duplicate bundle", [](CookedManifest& m) { SetBundles(m, "HUD", "Hud"); }, F::Bundle,
     0},
    {"null dependency",
     [](CookedManifest& m) {
         m.entries[0].dependencies[0] =
             CookedManifestDependency{AssetId{}, PackageDependencyKind::Hard};
     },
     F::Dependency, 0},
    {"self dependency",
     [](CookedManifest& m) { SetDependency(m, 0, 10, PackageDependencyKind::Hard); }, F::Dependency,
     0},
    {"editor-only kind",
     [](CookedManifest& m) { SetDependency(m, 0, 11, PackageDependencyKind::EditorOnly); },
     F::Dependency, 0},
    {"build kind", [](CookedManifest& m) { SetDependency(m, 0, 11, PackageDependencyKind::Build); },
     F::Dependency, 0},
    {"unsorted dependencies",
     [](CookedManifest& m) { SetDependency(m, 2, 5, PackageDependencyKind::Soft); }, F::Dependency,
     0},
    {"duplicate dependency",
     [](CookedManifest& m) { SetDependency(m, 1, 11, PackageDependencyKind::Hard); }, F::Dependency,
     0},
    {"hard dependency not cooked",
     [](CookedManifest& m) { SetDependency(m, 2, 99, PackageDependencyKind::Hard); }, F::Dependency,
     0},
    {"too many dependencies",
     [](CookedManifest& m) {
         m.entries[2].dependencies.Clear();
         for (uint32 index = 0; index <= CookedManifestMaxDependencies; ++index) {
             (void)AddDependency(m.entries[2], 1000 + index, PackageDependencyKind::Soft);
         }
     },
     F::DependencyCount, 2},
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
    const uint64 missing[] = {1, 13, 99};
    for (uint64 low : missing) {
        CHECK(read.Value().Find(FixedId(low)) == nullptr);
    }

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

// 칸 규칙(AS 10.1절 4–6): 위반 하나마다 ValidateCookedManifest·WriteCookedManifest(out 그대로)·
// ReadCookedManifest(시험 인코더로 쓴 byte)가 같은 칸·항목의 CorruptPackage다. 경계는 받는다(패키지
// 64 byte·64 GiB, bundle 256개, 의존 65536개, 경로 256 byte, 쿠킹하지 않은 Soft 대상).
TEST(CookedManifest_FieldRules) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    for (const Violation& violation : Violations) {
        CookedManifest manifest(allocator);
        REQUIRE(MakeBase(manifest));
        violation.mutate(manifest);
        AssetDiagnostic validated;
        CHECK(!ValidateCookedManifest(manifest, allocator, &validated));
        CHECK(CorruptAt(validated, violation.field, violation.detail));
        Array<uint8> out(allocator);
        AssetDiagnostic written;
        CHECK(WriteCookedManifest(allocator, manifest, out, &written).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(out.IsEmpty() && CorruptAt(written, violation.field, violation.detail));
        AssetDiagnostic read;
        CHECK(!ReadCookedManifest(allocator, EncodeManifest(allocator, manifest).View(), &read));
        if (!CorruptAt(read, violation.field, violation.detail)) {
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

    SetPath(edge, 1, "/Game");
    for (uint32 part = 0; part < 3; ++part) {
        (void)edge.entries[1].pathHint.TryAppend("/");
        for (uint32 index = 0; index < MaxNameComponentBytes; ++index) {
            (void)edge.entries[1].pathHint.TryAppend("a");
        }
    }

    // "/Game" 5 + 3 × 65 = 200 — 256까지 채운다.
    (void)edge.entries[1].pathHint.TryAppend("/");
    for (usize index = edge.entries[1].pathHint.View().ByteCount();
         index < CookedManifestMaxPathBytes; ++index) {
        (void)edge.entries[1].pathHint.TryAppend("b");
    }

    REQUIRE(edge.entries[1].pathHint.View().ByteCount() == CookedManifestMaxPathBytes);
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
              failure.section == static_cast<uint32>(F::FormatVersion));
    }

    Array<uint8> flipped = Copy(allocator, valid.View());
    flipped[flipped.Size() - 1] ^= 0x80;
    CHECK(expect(flipped, F::Crc, Head));
    flipped = Copy(allocator, valid.View());
    flipped[20] ^= 1;
    CHECK(expect(flipped, F::Crc, Head));

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

    // 파일 상한 초과는 내용을 읽기 전에 거부한다(길이만 큰 span — byte에 닿지 않는다).
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

// 할당 실패(모든 지점): 쓰기·읽기·검증이 OutOfMemory(진단 None)로 끝나고 쓰기 out은 그대로다.
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
    SetPath(manifest, 4, "/Content/Other");
    SetBundles(manifest, "Core", "Expansion1");
    Array<uint8> bytes(allocator);
    REQUIRE(WriteCookedManifest(allocator, manifest, bytes));
    test::NFailAllocator failing(allocator);
    uint32 counts[3] = {};
    for (uint32 operation = 0; operation < 3; ++operation) {
        for (uint64 attempt = 1;; ++attempt) {
            failing.FailOnAttempt(attempt);
            AssetDiagnostic failure;
            Result<void> result = Result<void>::Success();
            Array<uint8> out(allocator);
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
            CHECK(operation != 0 || out.IsEmpty());
            ++counts[operation];
        }

        failing.DisableFailure();
        CHECK(counts[operation] > 0);
    }
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

// 쿠킹 패키지 장면: B(1801)와 A(1800 — 대표 에셋 Hard → B, Soft → 1802, Zstd bulk와 쿠킹에서 빠지는
// EditorOnly bulk)를 A1a writer로
// 쓰고 매니페스트를 그 결과에서 만든다.
struct CookedScene {
    explicit CookedScene(Allocator& allocator) noexcept
        : a(allocator), b(allocator), pixels(allocator), data(allocator), manifest(allocator) {
    }

    Array<uint8> a;
    Array<uint8> b;
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
    if (!a || !b) {
        return false;
    }

    const ObjectHandle bAsset = Make<TestAsset>(source, TestAssetType(), b.Value().Handle(), "");
    const ObjectHandle aAsset = Make<TestAsset>(source, TestAssetType(), a.Value().Handle(), "");
    TestAsset* pointer = Pointer<TestAsset>(source, aAsset);
    if (bAsset.IsNull() || pointer == nullptr ||
        !source.SetReference(access, aAsset, pointer->hard, bAsset) ||
        !pointer->soft.TryAssign(FixedId(1802), StringView())) {
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
    if (scene.a.IsEmpty() || scene.b.IsEmpty()) {
        return false;
    }

    FillManifestHeader(scene.manifest);
    auto first = scene.manifest.entries.TryEmplaceBack(env.allocator);
    auto second = first ? scene.manifest.entries.TryEmplaceBack(env.allocator)
                        : Result<CookedManifestEntry*>::Failure(first.Error());
    if (!second) {
        return false;
    }

    return FillEntry(scene.manifest.entries[0], env.allocator, scene.a, "/Game/Hero") &&
           FillEntry(scene.manifest.entries[1], env.allocator, scene.b, "/Game/Sword") &&
           ValidateCookedManifest(scene.manifest, env.allocator).IsOk();
}

} // namespace

// 패키지 대조(AS 10.1절 8): A1a writer가 쓴 쿠킹 패키지는 그 결과로 만든 항목과 맞는다(Hard·Soft
// 의존·Zstd bulk raw 합 포함). 칸마다 하나씩 다르면 그 칸의 CorruptPackage다 — 크기(한 byte 덧붙임·
// 항목 크기), SHA-256(한 byte), 종류(에디터 패키지), assetId, 대표 에셋 타입, bulk raw 합, 프로파일
// (id·설정·쿠커 버전), 의존(빠짐·남음·종류·대상).
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
    CHECK(CheckCookedPackage(manifest, entry, scene.a.View(), contents.Value()));
    auto bContents = ReadPackage(allocator, scene.b.View(), PackageReadOptions{});
    REQUIRE(bContents);
    CHECK(CheckCookedPackage(manifest, manifest.entries[1], scene.b.View(), bContents.Value()));

    auto expect = [&](const CookedManifest& m, const CookedManifestEntry& e, Span<const uint8> file,
                      const PackageContents& c, CookedManifestField field) {
        AssetDiagnostic failure;
        return CheckCookedPackage(m, e, file, c, &failure).Error() == ErrorCode::InvalidArgument &&
               CorruptAt(failure, field, CookedManifestNoEntry) && failure.asset == e.asset;
    };

    // 크기·SHA-256(파일 쪽 변조).
    Array<uint8> longer = Copy(allocator, scene.a.View());
    REQUIRE(longer.TryPushBack(0));
    CHECK(expect(manifest, entry, longer.View(), contents.Value(), F::PackageBytes));
    Array<uint8> flipped = Copy(allocator, scene.a.View());
    flipped[100] ^= 1;
    CHECK(expect(manifest, entry, flipped.View(), contents.Value(), F::PackageSha256));

    // 항목 쪽 칸 하나씩.
    auto changed = [&](auto&& change, CookedManifestField field) {
        CookedManifest copy(allocator);
        FillManifestHeader(copy);
        auto added = copy.entries.TryEmplaceBack(allocator);
        if (!added) {
            return false;
        }

        CookedManifestEntry& e = *added.Value();
        if (!FillEntry(e, allocator, scene.a, "/Game/Hero")) {
            return false;
        }

        change(copy, e);
        return expect(copy, e, scene.a.View(), contents.Value(), field);
    };
    CHECK(changed([](CookedManifest&, CookedManifestEntry& e) { e.packageBytes += 1; },
                  F::PackageBytes));
    CHECK(changed([](CookedManifest&, CookedManifestEntry& e) { e.packageSha256.bytes[0] ^= 1; },
                  F::PackageSha256));
    CHECK(changed([](CookedManifest&, CookedManifestEntry& e) { e.asset = FixedId(1803); },
                  F::AssetId));
    CHECK(changed([](CookedManifest&, CookedManifestEntry& e) { e.type = TestPartType(); },
                  F::TypeId));
    CHECK(changed([](CookedManifest&, CookedManifestEntry& e) { e.bulkRawBytes -= 1; },
                  F::BulkRawBytes));
    CHECK(changed(
        [](CookedManifest& m, CookedManifestEntry&) {
            m.profileId.Clear();
            (void)m.profileId.TryAppend("Android");
        },
        F::PackageProfile));
    CHECK(changed([](CookedManifest& m, CookedManifestEntry&) { m.profileSettings.bytes[0] ^= 1; },
                  F::PackageProfile));
    CHECK(changed([](CookedManifest& m, CookedManifestEntry&) { ++m.cookerVersion; },
                  F::PackageProfile));
    CHECK(
        changed([](CookedManifest&, CookedManifestEntry& e) { (void)e.dependencies.TryPopBack(); },
                F::PackageDependencies));
    CHECK(changed(
        [](CookedManifest&, CookedManifestEntry& e) {
            (void)e.dependencies.TryPushBack(
                CookedManifestDependency{FixedId(1900), PackageDependencyKind::Soft});
        },
        F::PackageDependencies));
    CHECK(changed(
        [](CookedManifest&, CookedManifestEntry& e) {
            e.dependencies[0].kind = PackageDependencyKind::Soft;
        },
        F::PackageDependencies));
    CHECK(changed(
        [](CookedManifest&, CookedManifestEntry& e) { e.dependencies[1].asset = FixedId(1899); },
        F::PackageDependencies));
    CHECK(changed([](CookedManifest&, CookedManifestEntry& e) { e.dependencies.Clear(); },
                  F::PackageDependencies));

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
// 전에 거부한다 — 빈 항목들이라 할당은 항목 배열 하나다). reader 쪽 개수 칸 상한은
// CookedManifest_StructureTampering이 본다.
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
    Array<uint8> out(allocator);
    CHECK(WriteCookedManifest(allocator, manifest, out, &failure).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(out.IsEmpty() && CorruptAt(failure, F::EntryCount, Head));
}
