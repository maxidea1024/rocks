#include "ManifestTestSupport.h"

// 쿠킹 매니페스트 v1의 규모·조합·결정성·퍼징 시험이다(Docs/Design/AssetSystem 10.1절 A1b):
// 고정 byte(golden SHA-256과 체크인 v2 byte), 칸 종류·플래그·의존 특성 부분집합·경로 모양의 모든
// 조합(번호 ↔ 항목 일대일 검사), 많은 패키지·깊은 Hard 사슬·SCC를 담은 큰 매니페스트, 고정 seed
// 변조.
using namespace rocks;
namespace manifest_scale_tests_file {

using namespace rocks::asset_test;

namespace {

// 플래그 축은 PrimaryAsset 있음·없음 둘이다 — 플래그가 늘면 조합 축을 넓혀야 한다.
static_assert(CookedManifestFlagsMask == 1,
              "Cooked manifest flags changed; extend the combination axes in this test");

// 조합 축이다(AS 10.1절 5): PrimaryAsset·bundle 수와 구성원(비었음·쿠킹된 의존 대상 전부), 의존
// 특성 다섯의 모든 부분집합, 경로 모양.
enum class BundleShape : uint32 {
    None,
    One,
    Many,
    OneWithMembers,
    ManyWithMembers,
    Count
};

// 의존 특성(bit)이다. 특성마다 대상이 달라 한 항목에 어떤 부분집합이든 함께 담긴다.
enum DependencyTrait : uint32 {
    // Hard → 10(쿠킹됨, 의존 없음).
    CookedHard = 1u << 0,
    // Hard → 12, 12도 이 항목을 Hard로(순환).
    HardCycle = 1u << 1,
    // Soft → 11(쿠킹됨).
    CookedSoft = 1u << 2,
    // Soft → 5(쿠킹하지 않음).
    UncookedSoft = 1u << 3,
    // Hard → 13과 Soft → 13(같은 대상).
    SameTargetHardSoft = 1u << 4,
};

constexpr uint32 DependencyTraitCount = 5;
constexpr uint32 DependencySubsets = 1u << DependencyTraitCount;

enum class PathShape : uint32 {
    Shallow,
    Deep,
    // 비ASCII NFC 구성 요소(/Game/Ä…).
    NonAscii,
    Count
};

constexpr uint32 BundleShapes = static_cast<uint32>(BundleShape::Count);
constexpr uint32 PathShapes = static_cast<uint32>(PathShape::Count);
// 원시 조합 공간이다: (PrimaryAsset × bundle 모양 + 일반) × 의존 부분집합 × 경로. PrimaryAsset이
// 아닌 항목은 bundle이 없고(그래서 일반은 한 칸), 구성원은 그 항목의 의존 대상이며 매니페스트에
// 있어야 하므로 구성원 있는 모양은 쿠킹된 의존 대상이 있는 부분집합에서만 허용한다.
constexpr uint32 RawCombinations = (BundleShapes + 1) * DependencySubsets * PathShapes;
constexpr uint32 CookedTargetTraits = CookedHard | HardCycle | CookedSoft | SameTargetHardSoft;

constexpr bool AllowedRaw(uint32 raw) noexcept {
    const uint32 traits = raw / PathShapes % DependencySubsets;
    const uint32 bundle = raw / PathShapes / DependencySubsets;
    const bool members = bundle == static_cast<uint32>(BundleShape::OneWithMembers) ||
                         bundle == static_cast<uint32>(BundleShape::ManyWithMembers);
    return !members || (traits & CookedTargetTraits) != 0;
}

constexpr uint32 CountAllowed() noexcept {
    uint32 count = 0;
    for (uint32 raw = 0; raw < RawCombinations; ++raw) {
        count += AllowedRaw(raw) ? 1 : 0;
    }

    return count;
}

constexpr uint32 AllowedCombinations = CountAllowed();
// 구성원 없는 모양 4칸(PrimaryAsset 셋 + 일반) × 32 × 3 = 384, 구성원 있는 모양 2 × 쿠킹 대상 있는
// 부분집합 30 × 3 = 180.
static_assert(AllowedCombinations == 564, "Manifest combination count changed");
// 조합 항목의 id는 이 값부터이고, 의존 대상 고정 항목(10–13)은 그보다 앞이다.
constexpr uint64 ComboBase = 100;
// 고정 항목 수다(10 Hard 대상·11 Soft 대상·12 순환 짝·13 같은 대상).
constexpr usize FixedEntries = 4;
constexpr uint64 UncookedTarget = 5;
constexpr uint64 HardTarget = 10;
constexpr uint64 SoftTarget = 11;
constexpr uint64 CycleTarget = 12;
constexpr uint64 SharedTarget = 13;

struct Combination {
    bool primary;
    BundleShape bundles;
    uint32 traits;
    PathShape path;
};

// 원시 번호 index의 조합이다(경로가 가장 빨리 바뀌고, 그다음 의존 부분집합, 그다음 bundle).
Combination CombinationAt(uint32 index) noexcept {
    const uint32 path = index % PathShapes;
    index /= PathShapes;
    const uint32 traits = index % DependencySubsets;
    index /= DependencySubsets;
    // 0..BundleShapes-1 = PrimaryAsset × bundle 모양, BundleShapes = 일반(bundle 없음).
    const bool primary = index < BundleShapes;
    return Combination{primary, primary ? static_cast<BundleShape>(index) : BundleShape::None,
                       traits, static_cast<PathShape>(path)};
}

// 읽은 항목에서 원시 조합 번호를 다시 계산한다(CombinationAt의 역 — 빠짐·겹침 검사).
uint32 ClassOf(const CookedManifest& manifest, const CookedManifestEntry& entry) noexcept {
    const bool primary =
        (entry.flags & static_cast<uint32>(CookedManifestFlags::PrimaryAsset)) != 0;
    uint32 bundle = BundleShapes;
    if (primary) {
        const bool members = !entry.bundles.IsEmpty() && !entry.bundles[0].members.IsEmpty();
        const BundleShape shape =
            entry.bundles.IsEmpty() ? BundleShape::None
            : entry.bundles.Size() == 1
                ? (members ? BundleShape::OneWithMembers : BundleShape::One)
                : (members ? BundleShape::ManyWithMembers : BundleShape::Many);
        bundle = static_cast<uint32>(shape);
    }

    uint32 traits = 0;
    const Array<CookedManifestDependency>& dependencies = entry.dependencies;
    for (usize index = 0; index < dependencies.Size(); ++index) {
        const CookedManifestDependency& dependency = dependencies[index];
        const CookedManifestEntry* target = manifest.Find(dependency.asset);
        if (index + 1 < dependencies.Size() && dependencies[index + 1].asset == dependency.asset) {
            traits |= SameTargetHardSoft;
            ++index;
        } else if (dependency.kind == PackageDependencyKind::Soft) {
            traits |= target == nullptr ? UncookedSoft : CookedSoft;
        } else {
            // Hard 대상이 자기를 Hard로 가리키면 순환이다.
            bool cycle = false;
            for (const CookedManifestDependency& back : target->dependencies) {
                cycle = cycle ||
                        (back.asset == entry.asset && back.kind == PackageDependencyKind::Hard);
            }

            traits |= cycle ? HardCycle : CookedHard;
        }
    }

    constexpr uint8 FirstNonAsciiByte = 0x80;
    usize slashes = 0;
    bool ascii = true;
    for (char c : entry.pathHint.View()) {
        slashes += c == '/' ? 1 : 0;
        ascii = ascii && static_cast<uint8>(c) < FirstNonAsciiByte;
    }

    const PathShape path = !ascii        ? PathShape::NonAscii
                           : slashes > 2 ? PathShape::Deep
                                         : PathShape::Shallow;
    return (bundle * DependencySubsets + traits) * PathShapes + static_cast<uint32>(path);
}

// 조합 하나의 의존을 (asset, kind) 오름차순으로 더한다(5 < 10 < 11 < 12 < 13).
bool AddTraits(CookedManifest& manifest, CookedManifestEntry& entry, uint32 traits,
               uint64 low) noexcept {
    bool ok = true;
    if ((traits & UncookedSoft) != 0) {
        ok = ok && AddDependency(entry, UncookedTarget, PackageDependencyKind::Soft);
    }

    if ((traits & CookedHard) != 0) {
        ok = ok && AddDependency(entry, HardTarget, PackageDependencyKind::Hard);
    }

    if ((traits & CookedSoft) != 0) {
        ok = ok && AddDependency(entry, SoftTarget, PackageDependencyKind::Soft);
    }

    if ((traits & HardCycle) != 0) {
        // 순환 짝(고정 항목 2 = 12)도 이 항목을 Hard로 가리킨다(조합 id 오름차순으로 더해진다).
        ok = ok && AddDependency(entry, CycleTarget, PackageDependencyKind::Hard) &&
             AddDependency(manifest.entries[2], low, PackageDependencyKind::Hard);
    }

    if ((traits & SameTargetHardSoft) != 0) {
        ok = ok && AddDependency(entry, SharedTarget, PackageDependencyKind::Hard) &&
             AddDependency(entry, SharedTarget, PackageDependencyKind::Soft);
    }

    return ok;
}

// 모든 허용 조합을 항목으로 담은 매니페스트다.
bool MakeCombinations(CookedManifest& manifest) noexcept {
    FillManifestHeader(manifest);
    if (AddEntry(manifest, HardTarget, "Target") == nullptr ||
        AddEntry(manifest, SoftTarget, "Other") == nullptr ||
        AddEntry(manifest, CycleTarget, "Cycle") == nullptr ||
        AddEntry(manifest, SharedTarget, "Shared") == nullptr) {
        return false;
    }

    for (uint32 index = 0; index < RawCombinations; ++index) {
        if (!AllowedRaw(index)) {
            continue;
        }

        const Combination combination = CombinationAt(index);
        char name[8] = {'C', static_cast<char>('a' + index / 26 / 26 % 26),
                        static_cast<char>('a' + index / 26 % 26),
                        static_cast<char>('a' + index % 26), 0};
        const uint64 low = ComboBase + index;
        CookedManifestEntry* entry = AddEntry(
            manifest, low, name,
            combination.primary ? static_cast<uint32>(CookedManifestFlags::PrimaryAsset) : 0);
        if (entry == nullptr) {
            return false;
        }

        if (combination.path != PathShape::Shallow) {
            entry->pathHint.Clear();
            const StringView prefix =
                combination.path == PathShape::Deep ? "/Game/Deep/A/B/C/" : "/Game/\xC3\x84rger";
            if (!entry->pathHint.TryAppend(prefix).IsOk() ||
                !entry->pathHint.TryAppend(name).IsOk()) {
                return false;
            }
        }

        const bool members = combination.bundles == BundleShape::OneWithMembers ||
                             combination.bundles == BundleShape::ManyWithMembers;
        const uint32 bundles = combination.bundles == BundleShape::None ? 0
                               : combination.bundles == BundleShape::One ||
                                       combination.bundles == BundleShape::OneWithMembers
                                   ? 1
                                   : 3;
        // 구성원은 쿠킹된 의존 대상 전부(오름차순 10 < 11 < 12 < 13)다.
        uint64 targets[4] = {};
        usize targetCount = 0;
        const uint32 traits = combination.traits;
        if (members) {
            const uint64 candidates[] = {HardTarget, SoftTarget, CycleTarget, SharedTarget};
            const uint32 bits[] = {CookedHard, CookedSoft, HardCycle, SameTargetHardSoft};
            for (usize candidate = 0; candidate < 4; ++candidate) {
                if ((traits & bits[candidate]) != 0) {
                    targets[targetCount++] = candidates[candidate];
                }
            }
        }

        const char* const names[] = {"Alpha", "Beta", "Gamma"};
        for (uint32 bundle = 0; bundle < bundles; ++bundle) {
            if (!AddBundle(*entry, names[bundle], Span<const uint64>(targets, targetCount))) {
                return false;
            }
        }

        if (!AddTraits(manifest, *entry, combination.traits, low)) {
            return false;
        }
    }

    return true;
}

} // namespace

// 조합(AS 10.1절 5): PrimaryAsset·bundle(없음·하나·여럿 × 구성원 없음·쿠킹된 의존 대상 전부) +
// 일반 × 의존 특성(쿠킹된 Hard·Hard 순환·쿠킹된 Soft·쿠킹하지 않은 Soft·같은 대상 Hard+Soft) 다섯의
// 부분집합 32 × 경로(얕음·깊음·비ASCII NFC) 3의 모든 허용 조합 564개를 한 매니페스트에 담아
// 왕복하고, 읽은 항목마다 원시 조합 번호를 다시 계산해 만든 번호(id)와 같고 허용 조합임을 본다(번호
// ↔ 항목이 일대일 — 빠짐·겹침이 없다). 축이나 모양이 늘면 조합 수 static_assert가 막고, 플래그가
// 늘면 플래그 static_assert가 막는다. 금지 조합(일반 항목의 bundle, 쿠킹된 의존 대상이 없는 항목의
// 구성원 — 구성원은 의존 대상이고 매니페스트에 있어야 한다)은 CookedManifest_FieldRules가 본다.
TEST(CookedManifest_AllCombinations) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest manifest(allocator);
    REQUIRE(MakeCombinations(manifest));
    Array<uint8> bytes(allocator);
    AssetDiagnostic failure;
    REQUIRE(WriteCookedManifest(allocator, manifest, bytes, &failure));
    auto read = ReadCookedManifest(allocator, bytes.View(), &failure);
    REQUIRE(read);
    REQUIRE(read.Value().entries.Size() == AllowedCombinations + FixedEntries);
    bool seen[RawCombinations] = {};
    bool matched = true;
    for (uint32 index = 0; index < AllowedCombinations; ++index) {
        const CookedManifestEntry& entry = read.Value().entries[FixedEntries + index];
        const uint32 kind = ClassOf(read.Value(), entry);
        matched = matched && kind < RawCombinations && AllowedRaw(kind) && !seen[kind] &&
                  entry.asset == FixedId(ComboBase + kind);
        seen[kind < RawCombinations ? kind : 0] = true;
    }

    CHECK(matched);
    for (uint32 kind = 0; kind < RawCombinations; ++kind) {
        if (AllowedRaw(kind) && !seen[kind]) {
            test::RecordFailure("manifest combination missing", __FILE__, __LINE__);
        }
    }

    Array<uint8> again(allocator);
    REQUIRE(WriteCookedManifest(allocator, read.Value(), again));
    CHECK(SameBytes(again.View(), bytes.View()));
}

namespace {

// 큰 매니페스트: count개 항목, 항목 i는 Hard → i+1(깊은 사슬), 8개마다 끝이 앞 항목을 Hard로 가리켜
// 순환(SCC), Soft → 쿠킹하지 않은 id, 13개마다 PrimaryAsset(bundle 둘).
bool MakeLarge(CookedManifest& manifest, uint32 count) noexcept {
    FillManifestHeader(manifest);
    constexpr uint64 Base = 10000;
    constexpr uint32 CycleSpan = 8;
    constexpr uint32 PrimaryEvery = 13;
    for (uint32 index = 0; index < count; ++index) {
        char name[16] = {};
        uint32 value = index;
        usize length = 0;
        char digits[16];
        do {
            digits[length++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);

        name[0] = 'P';
        for (usize at = 0; at < length; ++at) {
            name[1 + at] = digits[length - 1 - at];
        }

        const bool primary = index % PrimaryEvery == 0;
        CookedManifestEntry* entry =
            AddEntry(manifest, Base + index, StringView(name, length + 1),
                     primary ? static_cast<uint32>(CookedManifestFlags::PrimaryAsset) : 0);
        if (entry == nullptr || (primary && (!AddBundle(*entry, "A") || !AddBundle(*entry, "B")))) {
            return false;
        }

        // (asset, kind) 오름차순: 순환 대상(앞) → 다음(뒤) → 쿠킹하지 않은 Soft(가장 뒤 id).
        if (index % CycleSpan == CycleSpan - 1 &&
            !AddDependency(*entry, Base + index - (CycleSpan - 1), PackageDependencyKind::Hard)) {
            return false;
        }

        if (index + 1 < count &&
            !AddDependency(*entry, Base + index + 1, PackageDependencyKind::Hard)) {
            return false;
        }

        if (!AddDependency(*entry, Base * 100 + index, PackageDependencyKind::Soft)) {
            return false;
        }
    }

    return true;
}

} // namespace

// 규모(AS 10.1절 3·6): 항목 20000개(깊이 20000 Hard 사슬, 8개 단위 순환, 쿠킹하지 않은 Soft,
// PrimaryAsset bundle)를 쓰고 읽어 같은 모델·같은 byte이고 Find가 모든 id를 찾는다. 사슬 끝에서
// 대상 하나를 빼면(쿠킹 폐포 깨짐) 그 항목의 Dependency 손상이다.
TEST(CookedManifest_LargeManifest) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    constexpr uint32 Count = 20000;
    CookedManifest manifest(allocator);
    REQUIRE(MakeLarge(manifest, Count));
    Array<uint8> bytes(allocator);
    AssetDiagnostic failure;
    REQUIRE(WriteCookedManifest(allocator, manifest, bytes, &failure));
    auto read = ReadCookedManifest(allocator, bytes.View(), &failure);
    REQUIRE(read);
    REQUIRE(read.Value().entries.Size() == Count);
    bool found = true;
    for (uint32 index = 0; index < Count; ++index) {
        const CookedManifestEntry* entry = read.Value().Find(FixedId(10000 + index));
        found = found && entry == &read.Value().entries[index];
    }

    CHECK(found);
    Array<uint8> again(allocator);
    REQUIRE(WriteCookedManifest(allocator, read.Value(), again));
    CHECK(SameBytes(again.View(), bytes.View()));

    // 사슬 가운데 항목을 지우면 앞 항목의 Hard 대상이 없다.
    constexpr usize Removed = Count / 2;
    REQUIRE(manifest.entries.TryRemoveAt(Removed, 1));
    CHECK(!ValidateCookedManifest(manifest, allocator, &failure));
    CHECK(CorruptAt(failure, CookedManifestField::Dependency, Removed - 1) &&
          failure.related == FixedId(10000 + Removed));
}

namespace {

// 고정 장면이다: 두 항목(PrimaryAsset·bundle·Hard·Soft, 일반·Hard 순환), 엔진 1.2.3.
bool MakeGolden(CookedManifest& manifest) noexcept {
    FillManifestHeader(manifest);
    CookedManifestEntry* a =
        AddEntry(manifest, 20, "Gold", static_cast<uint32>(CookedManifestFlags::PrimaryAsset));
    const uint64 coreMembers[] = {21};
    if (a == nullptr || !AddBundle(*a, "Core", coreMembers) || !AddBundle(*a, "Visual") ||
        !AddDependency(*a, 21, PackageDependencyKind::Hard) ||
        !AddDependency(*a, 22, PackageDependencyKind::Soft)) {
        return false;
    }

    CookedManifestEntry* b = AddEntry(manifest, 21, "Silver");
    return b != nullptr && AddDependency(*b, 20, PackageDependencyKind::Hard);
}

// 고정 장면 출력의 SHA-256이다.
constexpr StringView GoldenDigest =
    "4a6501edf3723a535dafbd879a39a4f2a73f688cacc767e8905eca9031cdc713";

// 체크인한 v2 매니페스트 byte다(고정 장면 — 셰이더 없는 프로필이라 backend·artifact 표가 비었다).
constexpr uint8 GoldenManifest[] = {
    0x52, 0x4d, 0x41, 0x4e, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x57, 0x69, 0x6e, 0x64, 0x6f, 0x77, 0x73, 0x0c, 0x8f, 0x76, 0xb5, 0x69,
    0x0d, 0x32, 0x6e, 0x2c, 0x01, 0xd7, 0xc5, 0x17, 0xfb, 0xe2, 0xf6, 0x21, 0xf9, 0xbf, 0xa7, 0x25,
    0x5b, 0xe8, 0x78, 0xcf, 0x83, 0xca, 0xbc, 0x3a, 0x66, 0x5d, 0x8c, 0xb5, 0xab, 0x8c, 0x84, 0x3f,
    0xd5, 0xf4, 0xf6, 0xd1, 0xf8, 0x4a, 0x22, 0xfc, 0x35, 0x43, 0x3c, 0x6f, 0xfb, 0x68, 0x01, 0x3e,
    0x70, 0x07, 0xb3, 0x45, 0x5a, 0x60, 0xa1, 0x26, 0x5a, 0xc8, 0xfb, 0x07, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x70, 0x5e, 0xa5, 0x00, 0x00, 0x57, 0x5e, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7d,
    0x44, 0xf2, 0x7c, 0x17, 0x83, 0x5f, 0x98, 0xa8, 0x04, 0xfb, 0x68, 0xee, 0x9b, 0x08, 0x9e, 0x0a,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2f, 0x47, 0x61, 0x6d, 0x65, 0x2f, 0x47, 0x6f, 0x6c,
    0x64, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x14, 0xfc, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15,
    0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15,
    0x15, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x43, 0x6f, 0x72, 0x65, 0x01, 0x00, 0x00, 0x00, 0x00, 0x70, 0x5e, 0xa5, 0x00, 0x00, 0x57,
    0x5e, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x56, 0x69, 0x73, 0x75, 0x61, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x70, 0x5e, 0xa5, 0x00, 0x00, 0x57, 0x5e, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x70, 0x5e, 0xa5, 0x00, 0x00, 0x57, 0x5e, 0x16, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x70, 0x5e, 0xa5, 0x00, 0x00, 0x57, 0x5e, 0x15,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7d, 0x44, 0xf2, 0x7c, 0x17, 0x83, 0x5f, 0x98, 0xa8,
    0x04, 0xfb, 0x68, 0xee, 0x9b, 0x08, 0x9e, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2f,
    0x47, 0x61, 0x6d, 0x65, 0x2f, 0x53, 0x69, 0x6c, 0x76, 0x65, 0x72, 0x15, 0x15, 0x15, 0x15, 0x15,
    0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15,
    0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0xfd, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x16, 0x16, 0x16, 0x16,
    0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16,
    0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x70, 0x5e, 0xa5, 0x00, 0x00, 0x57, 0x5e, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4d,
    0xce, 0x87, 0x38,
};

String HexOf(Allocator& allocator, Span<const uint8> bytes) noexcept {
    constexpr char Digits[] = "0123456789abcdef";
    String text(allocator);
    for (uint8 value : bytes) {
        const char pair[2] = {Digits[value >> 4], Digits[value & 0xF]};
        (void)text.TryAppend(StringView(pair, 2));
    }

    return text;
}

} // namespace

// 고정 byte(AS 10.1절 1 결정성): 고정 장면의 출력 SHA-256이 고정 값이고, 체크인한 v1 byte와 같으며,
// 그 byte를 읽어 다시 쓰면 같은 byte다(형식을 바꾸면 이 값이 바뀐다 — formatVersion 규칙을 따른다).
TEST(CookedManifest_GoldenBytes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest manifest(allocator);
    REQUIRE(MakeGolden(manifest));
    Array<uint8> bytes(allocator);
    REQUIRE(WriteCookedManifest(allocator, manifest, bytes));
    const Sha256Digest digest = Sha256::Hash(bytes.View());
    String actual = HexOf(allocator, Span<const uint8>(digest.bytes, Sha256Digest::Bytes));
    if (!(actual.View() == GoldenDigest)) {
        test::RecordFailure(actual.CStr(), __FILE__, __LINE__);
    }

    const Span<const uint8> stored(GoldenManifest, sizeof(GoldenManifest));
    if (!SameBytes(bytes.View(), stored)) {
        String hex = HexOf(allocator, bytes.View());
        test::RecordFailure(hex.CStr(), __FILE__, __LINE__);
    }

    auto read = ReadCookedManifest(allocator, stored);
    REQUIRE(read);
    Array<uint8> again(allocator);
    REQUIRE(WriteCookedManifest(allocator, read.Value(), again));
    CHECK(SameBytes(again.View(), stored));
}

namespace {

struct XorShift {
    uint64 state;

    uint64 Next() noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

} // namespace

// 고정 seed 변조(AS 10.1절 2): 기본 매니페스트 byte를 4000번 무작위로 바꾼다(byte 1–4개 바꾸기·
// 자르기·덧붙이기, 절반은 CRC를 다시 맞춘다). 결과는 CorruptPackage / InvalidArgument 또는
// UnsupportedVersion / Unsupported뿐이고, 받아들인 입력은 다시 쓰면 같은 byte다(정준 — 같은 모델로
// 풀리는 다른 byte가 없다). 거부와 받아들임이 모두 일어난다.
TEST(CookedManifest_FixedSeedMutationFuzz) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest manifest(allocator);
    REQUIRE(MakeGolden(manifest));
    Array<uint8> valid(allocator);
    REQUIRE(WriteCookedManifest(allocator, manifest, valid));
    constexpr uint32 Iterations = 4000;
    XorShift random{0x5EED0000A1B0ull};
    uint32 rejected = 0;
    uint32 accepted = 0;
    for (uint32 iteration = 0; iteration < Iterations; ++iteration) {
        Array<uint8> input = Copy(allocator, valid.View());
        const uint64 shape = random.Next() % 8;
        if (shape == 0) {
            REQUIRE(input.TryResize(static_cast<usize>(random.Next() % input.Size())));
        } else if (shape == 1) {
            REQUIRE(input.TryPushBack(static_cast<uint8>(random.Next())));
        } else {
            const uint64 changes = 1 + random.Next() % 4;
            for (uint64 change = 0; change < changes; ++change) {
                input[static_cast<usize>(random.Next() % input.Size())] ^=
                    static_cast<uint8>(1 + random.Next() % 255);
            }
        }

        if (iteration % 2 == 0) {
            FixManifestCrc(input);
        }

        AssetDiagnostic failure;
        auto read = ReadCookedManifest(allocator, input.View(), &failure);
        if (!read) {
            ++rejected;
            const bool corrupt = failure.code == AssetDiagnosticCode::CorruptPackage &&
                                 read.Error() == ErrorCode::InvalidArgument;
            const bool version = failure.code == AssetDiagnosticCode::UnsupportedVersion &&
                                 read.Error() == ErrorCode::Unsupported;
            CHECK(corrupt || version);
            continue;
        }

        ++accepted;
        Array<uint8> again(allocator);
        REQUIRE(WriteCookedManifest(allocator, read.Value(), again));
        CHECK(SameBytes(again.View(), input.View()));
    }

    CHECK(rejected + accepted == Iterations);
    CHECK(rejected > Iterations / 2);
    CHECK(accepted > 0);
}

// bundle 구성원 상한(AS 10.1절 3·5): 구성원이 정확히 CookedManifestMaxBundleMembers(65536)개인
// bundle(구성원 모두 쿠킹된 Soft 의존 대상)은 검증·쓰기·읽기를 통과하고 같은 구성원으로 읽힌다(하나
// 더면 CookedManifest_FieldRules가 BundleMemberCount로 거부한다).
TEST(CookedManifest_BundleMemberLimit) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest manifest(allocator);
    FillManifestHeader(manifest);
    constexpr uint64 Primary = 1;
    constexpr uint64 FirstMember = 2;
    REQUIRE(manifest.entries.TryReserve(CookedManifestMaxBundleMembers + 1));
    REQUIRE(AddEntry(manifest, Primary, "Primary",
                     static_cast<uint32>(CookedManifestFlags::PrimaryAsset)) != nullptr);
    Array<uint64> members(allocator);
    REQUIRE(members.TryReserve(CookedManifestMaxBundleMembers));
    for (uint32 index = 0; index < CookedManifestMaxBundleMembers; ++index) {
        char name[8] = {'M',
                        static_cast<char>('a' + index / 26 / 26 / 26 % 26),
                        static_cast<char>('a' + index / 26 / 26 % 26),
                        static_cast<char>('a' + index / 26 % 26),
                        static_cast<char>('a' + index % 26),
                        0};
        const uint64 id = FirstMember + index;
        REQUIRE(AddEntry(manifest, id, name) != nullptr);
        REQUIRE(members.TryPushBack(id));
    }

    CookedManifestEntry& primary = manifest.entries[0];
    REQUIRE(primary.dependencies.TryReserve(CookedManifestMaxBundleMembers));
    for (uint64 id : members) {
        REQUIRE(AddDependency(primary, id, PackageDependencyKind::Soft));
    }

    REQUIRE(AddBundle(primary, "All", members.View()));
    AssetDiagnostic failure;
    REQUIRE(ValidateCookedManifest(manifest, allocator, &failure));
    Array<uint8> bytes(allocator);
    REQUIRE(WriteCookedManifest(allocator, manifest, bytes, &failure));
    auto read = ReadCookedManifest(allocator, bytes.View(), &failure);
    REQUIRE(read);
    const CookedManifestBundle& bundle = read.Value().entries[0].bundles[0];
    REQUIRE(bundle.members.Size() == CookedManifestMaxBundleMembers);
    CHECK(bundle.members[0] == FixedId(FirstMember));
    CHECK(bundle.members[CookedManifestMaxBundleMembers - 1] ==
          FixedId(FirstMember + CookedManifestMaxBundleMembers - 1));
}

} // namespace manifest_scale_tests_file
