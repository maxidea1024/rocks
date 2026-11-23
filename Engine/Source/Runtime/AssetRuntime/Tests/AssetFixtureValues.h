#pragma once

#include "AssetTestSupport.h"

#include <AssetTestFixtures/ValueAssets.generated.h>
#include <AssetTestFixtures/ValueAssets.h>

// fixture 에셋의 결정적 값 생성기와 필드 단위 검사 함수다(Docs/Design/AssetSystem 14.1절).
// Fill*은 seed로 모든 영속 필드를 채우고, Check*는 같은 seed의 기대값과 필드마다 비교한다. 참조
// 필드는 장면을 만드는 시험이 따로 설정·확인한다(대상 handle은 Registry마다 다르다).
namespace rocks::asset_test {

using asset_fixture::BaseGameAsset;
using asset_fixture::BulkAsset;
using asset_fixture::DerivedGameAsset;
using asset_fixture::FixtureEntry;
using asset_fixture::FixtureKind;
using asset_fixture::FixturePayload;
using asset_fixture::FixturePoint;
using asset_fixture::FixtureRange;
using asset_fixture::GameComponent;
using asset_fixture::GameDataAsset;
using asset_fixture::SimpleAsset;
using asset_fixture::ValidatedAsset;
using asset_fixture::ValueAsset;

template <class T>
TypeId IdOf() noexcept {
    return GeneratedType<T>::Id();
}

// 검사 함수가 기대 문자열을 만들 때 쓰는 allocator다.
inline SystemAllocator& ScratchAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// seed로 만든 짧은 문자열("<prefix>-<seed 10진>")이다.
inline bool Label(String& out, StringView prefix, uint32 seed) noexcept {
    char digits[16];
    usize count = 0;
    do {
        digits[count++] = static_cast<char>('0' + seed % 10);
        seed /= 10;
    } while (seed != 0);

    char text[17];
    text[0] = '-';
    for (usize index = 0; index < count; ++index) {
        text[index + 1] = digits[count - 1 - index];
    }

    out.Clear();
    return out.TryAppend(prefix).IsOk() && out.TryAppend(StringView(text, count + 1)).IsOk();
}

inline bool LabelIs(StringView actual, StringView prefix, uint32 seed) noexcept {
    String expected(ScratchAllocator());
    return Label(expected, prefix, seed) && actual == expected.View();
}

// ---- SimpleAsset ----

inline bool FillSimple(SimpleAsset& asset, NamePool& names, uint32 seed) noexcept {
    asset.count = static_cast<int32>(seed * 7) - 100;
    asset.tiny = static_cast<int8>(-(static_cast<int32>(seed % 100)));
    asset.big = (uint64{seed} << 40) | 0xABCDu;
    asset.ratio = static_cast<float>(seed) * 0.25f;
    asset.precise = static_cast<double>(seed) / 3.0;
    asset.enabled = (seed & 1) != 0;
    asset.kind = seed % 2 == 0 ? FixtureKind::Large : FixtureKind::Small;
    asset.key = Guid{0x1000 + seed, 0x2000 + seed};
    String tag(ScratchAllocator());
    if (!Label(tag, "tag", seed)) {
        return false;
    }

    auto interned = names.TryIntern(tag.View());
    if (!interned) {
        return false;
    }

    asset.tag = interned.Value();
    return Label(asset.label, "simple", seed);
}

inline bool CheckSimple(const SimpleAsset& asset, NamePool& names, uint32 seed) noexcept {
    String tag(ScratchAllocator());
    auto expectedTag = Label(tag, "tag", seed) ? names.TryFind(tag.View())
                                               : Result<Name>::Failure(ErrorCode::OutOfMemory);
    return asset.count == static_cast<int32>(seed * 7) - 100 &&
           asset.tiny == static_cast<int8>(-(static_cast<int32>(seed % 100))) &&
           asset.big == ((uint64{seed} << 40) | 0xABCDu) &&
           asset.ratio == static_cast<float>(seed) * 0.25f &&
           asset.precise == static_cast<double>(seed) / 3.0 && asset.enabled == ((seed & 1) != 0) &&
           asset.kind == (seed % 2 == 0 ? FixtureKind::Large : FixtureKind::Small) &&
           asset.key == (Guid{0x1000 + seed, 0x2000 + seed}) && expectedTag &&
           asset.tag == expectedTag.Value() && LabelIs(asset.label.View(), "simple", seed);
}

// ---- ValueAsset ----

inline bool FillPoint(FixturePoint& point, uint32 seed, bool weighted) noexcept {
    point.x = static_cast<int32>(seed);
    point.y = -static_cast<int32>(seed) * 2;
    if (weighted) {
        point.weight.Emplace(seed + 5);
    }

    return Label(point.label, "point", seed);
}

inline bool CheckPoint(const FixturePoint& point, uint32 seed, bool weighted) noexcept {
    return point.x == static_cast<int32>(seed) && point.y == -static_cast<int32>(seed) * 2 &&
           point.weight.HasValue() == weighted && (!weighted || point.weight.Value() == seed + 5) &&
           LabelIs(point.label.View(), "point", seed);
}

inline bool FillValue(ValueAsset& asset, TypeRegistry& types, Allocator& allocator,
                      uint32 seed) noexcept {
    if (!FillPoint(asset.group.point, seed, true)) {
        return false;
    }

    for (uint32 index = 0; index < 3; ++index) {
        auto added = asset.group.points.TryEmplaceBack(allocator);
        if (!added || !FillPoint(*added.Value(), seed + index + 1, index % 2 == 0)) {
            return false;
        }
    }

    for (uint32 index = 0; index < 4; ++index) {
        asset.fixed[index] = static_cast<int32>(seed * (index + 1));
    }

    for (uint32 index = 0; index < 5; ++index) {
        if (!asset.numbers.TryPushBack(static_cast<int32>(seed + index * 11))) {
            return false;
        }
    }

    for (uint32 index = 0; index < 3; ++index) {
        String name(allocator);
        String word(allocator);
        if (!Label(name, "name", seed + index) || !Label(word, "word", seed + index) ||
            !asset.names.Insert(seed + index, Move(name)) || !asset.words.Add(Move(word))) {
            return false;
        }
    }

    String maybe(allocator);
    if (!Label(maybe, "maybe", seed)) {
        return false;
    }

    asset.maybe.Emplace(Move(maybe));
    FixturePoint& point = asset.maybePoint.Emplace(allocator);
    if (!FillPoint(point, seed + 9, false)) {
        return false;
    }

    auto title = Text::Invariant(allocator, "Fixture title");
    if (!title) {
        return false;
    }

    asset.title = Move(title.Value());
    for (uint32 index = 0; index < 300; ++index) {
        if (!asset.bytes.TryPushBack(static_cast<uint8>(seed + index))) {
            return false;
        }
    }

    if (!asset.any.Emplace<FixturePayload>(types, allocator)) {
        return false;
    }

    FixturePayload* payload = asset.any.Get<FixturePayload>();
    payload->factor = static_cast<float>(seed) + 0.5f;
    payload->steps = static_cast<int32>(seed) * 3;
    if (!asset.curve.TryAddKey(0.0f, static_cast<float>(seed)) ||
        !asset.curve.TryAddKey(1.0f, static_cast<float>(seed) * 2.0f) ||
        !asset.track.TryAddPoint(0.0f, 1.0f) ||
        !asset.track.TryAddPoint(2.0f, static_cast<float>(seed))) {
        return false;
    }

    // UE 단위: 위치 cm, 각도 도.
    asset.placement.translation = Vec3d(100.0 * seed, -50.0, 25.5);
    asset.placement.scale = Vec3d(1.0, 2.0, 0.5);
    asset.position = Vec3(1.0f, 2.0f, static_cast<float>(seed));
    asset.heading = Rotator<double>{10.0, static_cast<double>(seed), -90.0};
    return true;
}

inline bool CheckValue(const ValueAsset& asset, uint32 seed) noexcept {
    if (!CheckPoint(asset.group.point, seed, true) || asset.group.points.Size() != 3) {
        return false;
    }

    for (uint32 index = 0; index < 3; ++index) {
        if (!CheckPoint(asset.group.points[index], seed + index + 1, index % 2 == 0)) {
            return false;
        }
    }

    for (uint32 index = 0; index < 4; ++index) {
        if (asset.fixed[index] != static_cast<int32>(seed * (index + 1))) {
            return false;
        }
    }

    if (asset.numbers.Size() != 5 || asset.names.Count() != 3 || asset.words.Count() != 3) {
        return false;
    }

    for (uint32 index = 0; index < 5; ++index) {
        if (asset.numbers[index] != static_cast<int32>(seed + index * 11)) {
            return false;
        }
    }

    for (uint32 index = 0; index < 3; ++index) {
        const String* name = asset.names.Find(seed + index);
        String word(ScratchAllocator());
        if (name == nullptr || !LabelIs(name->View(), "name", seed + index) ||
            !Label(word, "word", seed + index) || !asset.words.Contains(word.View())) {
            return false;
        }
    }

    if (!asset.maybe.HasValue() || !LabelIs(asset.maybe.Value().View(), "maybe", seed) ||
        !asset.maybePoint.HasValue() || !CheckPoint(asset.maybePoint.Value(), seed + 9, false)) {
        return false;
    }

    TextPersistentView title;
    if (!asset.title.TryGetPersistentForm(title) || title.source != StringView("Fixture title")) {
        return false;
    }

    if (asset.bytes.Size() != 300) {
        return false;
    }

    for (uint32 index = 0; index < 300; ++index) {
        if (asset.bytes[index] != static_cast<uint8>(seed + index)) {
            return false;
        }
    }

    const FixturePayload* payload = asset.any.Get<FixturePayload>();
    return payload != nullptr && payload->factor == static_cast<float>(seed) + 0.5f &&
           payload->steps == static_cast<int32>(seed) * 3 && asset.curve.KeyCount() == 2 &&
           asset.curve.Evaluate(1.0f) == static_cast<float>(seed) * 2.0f &&
           asset.track.Points().Size() == 2 &&
           asset.track.Points()[1].outValue == static_cast<float>(seed) &&
           asset.placement.translation == Vec3d(100.0 * seed, -50.0, 25.5) &&
           asset.placement.scale == Vec3d(1.0, 2.0, 0.5) &&
           asset.position == Vec3(1.0f, 2.0f, static_cast<float>(seed)) &&
           asset.heading.pitch == 10.0 && asset.heading.yaw == static_cast<double>(seed) &&
           asset.heading.roll == -90.0;
}

// ---- 상속 ----

inline bool FillDerived(DerivedGameAsset& asset, uint32 seed) noexcept {
    asset.baseValue = static_cast<int32>(seed) + 1000;
    asset.extra = static_cast<float>(seed) * 1.5f;
    return Label(asset.baseName, "base", seed) && asset.extras.TryPushBack(1.0f) &&
           asset.extras.TryPushBack(static_cast<float>(seed));
}

inline bool CheckDerived(const DerivedGameAsset& asset, uint32 seed) noexcept {
    return asset.baseValue == static_cast<int32>(seed) + 1000 &&
           asset.extra == static_cast<float>(seed) * 1.5f &&
           LabelIs(asset.baseName.View(), "base", seed) && asset.extras.Size() == 2 &&
           asset.extras[0] == 1.0f && asset.extras[1] == static_cast<float>(seed);
}

// ---- 검증 hook ----

inline bool FillValidated(ValidatedAsset& asset, uint32 seed) noexcept {
    asset.range = FixtureRange{static_cast<int32>(seed), static_cast<int32>(seed) + 10};
    return asset.ranges.TryPushBack(FixtureRange{-1, 1}) &&
           asset.ranges.TryPushBack(
               FixtureRange{static_cast<int32>(seed), static_cast<int32>(seed)});
}

inline bool CheckValidated(const ValidatedAsset& asset, uint32 seed) noexcept {
    return asset.range.low == static_cast<int32>(seed) &&
           asset.range.high == static_cast<int32>(seed) + 10 && asset.ranges.Size() == 2 &&
           asset.ranges[0].low == -1 && asset.ranges[1].high == static_cast<int32>(seed);
}

// ---- bulk 무늬 ----

// 압축되는 무늬(반복이 많다).
inline void FillCompressible(Array<uint8>& bytes, usize count, uint32 seed) noexcept {
    (void)bytes.TryResize(count);
    for (usize index = 0; index < count; ++index) {
        bytes[index] = static_cast<uint8>(seed + (index / 256) % 5);
    }
}

// 압축되지 않는 무늬(고정 seed xorshift).
inline void FillIncompressible(Array<uint8>& bytes, usize count, uint32 seed) noexcept {
    (void)bytes.TryResize(count);
    uint64 state = 0x9E3779B97F4A7C15ull ^ seed;
    for (usize index = 0; index < count; ++index) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        bytes[index] = static_cast<uint8>(state >> 24);
    }
}

} // namespace rocks::asset_test
