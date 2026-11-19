#include "ManifestTestSupport.h"

// 쿠킹 매니페스트 v1의 규모·조합·결정성·퍼징 시험이다(Docs/Design/AssetSystem.ko.md 10.1절 A1b):
// 고정 byte(golden SHA-256과 체크인 v1 byte), 칸 종류·플래그의 모든 조합(빠짐 검사 포함), 많은
// 패키지·깊은 Hard 사슬·SCC를 담은 큰 매니페스트, 고정 seed 변조.
using namespace rocks;
namespace manifest_scale_tests_file {

using namespace rocks::asset_test;

namespace {

// 조합 축이다(AS 10.1절 5): PrimaryAsset·bundle 수, 의존 모양, 경로 깊이.
enum class BundleShape : uint32 {
    None,
    One,
    Many,
    Count
};
enum class DependencyShape : uint32 {
    None,
    Hard,
    Soft,
    HardAndSoftSameTarget,
    SoftNotCooked,
    HardCycle,
    Count
};
enum class PathShape : uint32 {
    Shallow,
    Deep,
    Count
};

constexpr uint32 BundleShapes = static_cast<uint32>(BundleShape::Count);
constexpr uint32 DependencyShapes = static_cast<uint32>(DependencyShape::Count);
constexpr uint32 PathShapes = static_cast<uint32>(PathShape::Count);
// PrimaryAsset이 아닌 항목은 bundle이 없다(규칙) — 허용 조합은 (PrimaryAsset × bundle 셋 + 일반 ×
// bundle 없음) × 의존 × 경로다.
constexpr uint32 AllowedCombinations = (BundleShapes + 1) * DependencyShapes * PathShapes;
// 조합 항목의 id는 이 값부터이고, 의존 대상 고정 항목(10·11)은 그보다 앞이다.
constexpr uint64 ComboBase = 100;

struct Combination {
    bool primary;
    BundleShape bundles;
    DependencyShape dependencies;
    PathShape path;
};

// index번째 허용 조합이다.
Combination CombinationAt(uint32 index) noexcept {
    const uint32 path = index % PathShapes;
    index /= PathShapes;
    const uint32 dependency = index % DependencyShapes;
    index /= DependencyShapes;
    // 0..BundleShapes-1 = PrimaryAsset × bundle 모양, BundleShapes = 일반(bundle 없음).
    const bool primary = index < BundleShapes;
    return Combination{primary, primary ? static_cast<BundleShape>(index) : BundleShape::None,
                       static_cast<DependencyShape>(dependency), static_cast<PathShape>(path)};
}

// 조합 하나의 분류 번호다(읽은 항목에서 다시 계산한다 — 빠짐 검사).
uint32 ClassOf(const CookedManifest& manifest, const CookedManifestEntry& entry) noexcept {
    const bool primary =
        (entry.flags & static_cast<uint32>(CookedManifestFlags::PrimaryAsset)) != 0;
    uint32 bundle = BundleShapes;
    if (primary) {
        bundle = entry.bundles.IsEmpty()     ? static_cast<uint32>(BundleShape::None)
                 : entry.bundles.Size() == 1 ? static_cast<uint32>(BundleShape::One)
                                             : static_cast<uint32>(BundleShape::Many);
    }

    DependencyShape dependency = DependencyShape::None;
    const usize count = entry.dependencies.Size();
    if (count == 2 && entry.dependencies[0].asset == entry.dependencies[1].asset) {
        dependency = DependencyShape::HardAndSoftSameTarget;
    } else if (count == 1 && entry.dependencies[0].kind == PackageDependencyKind::Soft) {
        dependency = manifest.Find(entry.dependencies[0].asset) == nullptr
                         ? DependencyShape::SoftNotCooked
                         : DependencyShape::Soft;
    } else if (count == 1) {
        // Hard 대상이 자기를 Hard로 가리키면 순환이다.
        const CookedManifestEntry* target = manifest.Find(entry.dependencies[0].asset);
        bool cycle = false;
        for (const CookedManifestDependency& back : target->dependencies) {
            cycle =
                cycle || (back.asset == entry.asset && back.kind == PackageDependencyKind::Hard);
        }

        dependency = cycle ? DependencyShape::HardCycle : DependencyShape::Hard;
    }

    usize slashes = 0;
    for (char c : entry.pathHint.View()) {
        slashes += c == '/' ? 1 : 0;
    }

    const uint32 path = slashes > 2 ? static_cast<uint32>(PathShape::Deep)
                                    : static_cast<uint32>(PathShape::Shallow);
    return (bundle * DependencyShapes + static_cast<uint32>(dependency)) * PathShapes + path;
}

// 모든 허용 조합을 항목으로 담은 매니페스트다. 순환 조합은 고정 항목 11과 서로 Hard로 가리키지
// 않도록 자기 짝(고정 항목 12 — 조합 항목을 Hard로 가리킨다)을 둔다.
bool MakeCombinations(CookedManifest& manifest) noexcept {
    FillManifestHeader(manifest);
    // 고정 대상: 10(의존 없음), 11(의존 없음).
    if (AddEntry(manifest, 10, "Target") == nullptr || AddEntry(manifest, 11, "Other") == nullptr) {
        return false;
    }

    // 순환 짝: 12가 순환 조합 항목들을 Hard로 가리킨다(아래에서 채운다).
    CookedManifestEntry* cycle = AddEntry(manifest, 12, "Cycle");
    if (cycle == nullptr) {
        return false;
    }

    for (uint32 index = 0; index < AllowedCombinations; ++index) {
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

        if (combination.path == PathShape::Deep) {
            entry->pathHint.Clear();
            if (!entry->pathHint.TryAppend("/Game/Deep/A/B/C/").IsOk() ||
                !entry->pathHint.TryAppend(name).IsOk()) {
                return false;
            }
        }

        const uint32 bundles = combination.bundles == BundleShape::One    ? 1
                               : combination.bundles == BundleShape::Many ? 3
                                                                          : 0;
        const char* const names[] = {"Alpha", "Beta", "Gamma"};
        for (uint32 bundle = 0; bundle < bundles; ++bundle) {
            if (!AddBundle(*entry, names[bundle])) {
                return false;
            }
        }

        bool ok = true;
        switch (combination.dependencies) {
        case DependencyShape::None:
            break;
        case DependencyShape::Hard:
            ok = AddDependency(*entry, 10, PackageDependencyKind::Hard);
            break;
        case DependencyShape::Soft:
            ok = AddDependency(*entry, 11, PackageDependencyKind::Soft);
            break;
        case DependencyShape::HardAndSoftSameTarget:
            ok = AddDependency(*entry, 10, PackageDependencyKind::Hard) &&
                 AddDependency(*entry, 10, PackageDependencyKind::Soft);
            break;
        case DependencyShape::SoftNotCooked:
            ok = AddDependency(*entry, 5, PackageDependencyKind::Soft);
            break;
        case DependencyShape::HardCycle:
            ok = AddDependency(*entry, 12, PackageDependencyKind::Hard) &&
                 AddDependency(manifest.entries[2], low, PackageDependencyKind::Hard);
            break;
        case DependencyShape::Count:
            ok = false;
            break;
        }

        if (!ok) {
            return false;
        }
    }

    return true;
}

} // namespace

// 조합(AS 10.1절 5): PrimaryAsset·bundle(없음·하나·여럿) × 의존(없음·Hard·Soft·같은 대상 Hard+Soft·
// 쿠킹하지 않은 Soft·Hard 순환) × 경로 깊이의 모든 허용 조합을 한 매니페스트에 담아 왕복하고, 읽은
// 결과에서 조합 분류를 다시 세어 빠진 조합이 없음을 본다(축이나 모양이 늘면 AllowedCombinations가
// 바뀌어 이 시험이 그 칸을 만든다). 금지 조합(일반 항목의 bundle)은 CookedManifest_FieldRules가
// 본다.
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
    REQUIRE(read.Value().entries.Size() == AllowedCombinations + 3);
    bool seen[AllowedCombinations] = {};
    for (usize index = 3; index < read.Value().entries.Size(); ++index) {
        const uint32 kind = ClassOf(read.Value(), read.Value().entries[index]);
        REQUIRE(kind < AllowedCombinations);
        CHECK(!seen[kind]);
        seen[kind] = true;
    }

    for (uint32 kind = 0; kind < AllowedCombinations; ++kind) {
        if (!seen[kind]) {
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
    if (a == nullptr || !AddBundle(*a, "Core") ||
        !AddDependency(*a, 21, PackageDependencyKind::Hard) ||
        !AddDependency(*a, 22, PackageDependencyKind::Soft)) {
        return false;
    }

    CookedManifestEntry* b = AddEntry(manifest, 21, "Silver");
    return b != nullptr && AddDependency(*b, 20, PackageDependencyKind::Hard);
}

// 고정 장면 출력의 SHA-256이다.
constexpr StringView GoldenDigest =
    "323e8607cc8a929b2bc41526377c32b5fdf4d17e67637ed24d3d46d7a7f582b2";

// 체크인한 v1 매니페스트 byte다(고정 장면).
constexpr uint8 GoldenManifest[] = {
    0x52, 0x4d, 0x41, 0x4e, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x57, 0x69, 0x6e, 0x64, 0x6f, 0x77, 0x73, 0x0c, 0x8f, 0x76, 0xb5, 0x69,
    0x0d, 0x32, 0x6e, 0x2c, 0x01, 0xd7, 0xc5, 0x17, 0xfb, 0xe2, 0xf6, 0x21, 0xf9, 0xbf, 0xa7, 0x25,
    0x5b, 0xe8, 0x78, 0xcf, 0x83, 0xca, 0xbc, 0x3a, 0x66, 0x5d, 0x8c, 0xb5, 0xab, 0x8c, 0x84, 0x3f,
    0xd5, 0xf4, 0xf6, 0xd1, 0xf8, 0x4a, 0x22, 0xfc, 0x35, 0x43, 0x3c, 0x6f, 0xfb, 0x68, 0x01, 0x3e,
    0x70, 0x07, 0xb3, 0x45, 0x5a, 0x60, 0xa1, 0x26, 0x5a, 0xc8, 0xfb, 0x07, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x70, 0x5e, 0xa5, 0x00,
    0x00, 0x57, 0x5e, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7d, 0x44, 0xf2, 0x7c, 0x17,
    0x83, 0x5f, 0x98, 0xa8, 0x04, 0xfb, 0x68, 0xee, 0x9b, 0x08, 0x9e, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x2f, 0x47, 0x61, 0x6d, 0x65, 0x2f, 0x47, 0x6f, 0x6c, 0x64, 0x14, 0x14, 0x14,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0xfc, 0x03, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x15, 0x15,
    0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15,
    0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x01, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x43, 0x6f, 0x72,
    0x65, 0x02, 0x00, 0x00, 0x00, 0x00, 0x70, 0x5e, 0xa5, 0x00, 0x00, 0x57, 0x5e, 0x15, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x70, 0x5e, 0xa5, 0x00, 0x00, 0x57,
    0x5e, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x70, 0x5e,
    0xa5, 0x00, 0x00, 0x57, 0x5e, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7d, 0x44, 0xf2,
    0x7c, 0x17, 0x83, 0x5f, 0x98, 0xa8, 0x04, 0xfb, 0x68, 0xee, 0x9b, 0x08, 0x9e, 0x0c, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x2f, 0x47, 0x61, 0x6d, 0x65, 0x2f, 0x53, 0x69, 0x6c, 0x76, 0x65,
    0x72, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15,
    0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x15,
    0x15, 0xfd, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16,
    0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16,
    0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x70, 0x5e,
    0xa5, 0x00, 0x00, 0x57, 0x5e, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x15, 0xe1, 0xf8, 0xd8,
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
} // namespace manifest_scale_tests_file
