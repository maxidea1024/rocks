#pragma once

#include "AssetFixtureValues.h"

#include <AssetTestFixtures/CombinationAssets.generated.h>
#include <AssetTestFixtures/CombinationAssets.h>
#include <Base/TypeTraits.h>

// "지원하는 모든 조합" fixture(AssetTestFixtures/CombinationAssets.h)의 결정적 생성기와 비교기다
// (Docs/Design/AssetSystem 14.1절). 종류별 조합 struct의 멤버 이름이 같으므로(value·array·
// fixed·keyed·mapped·set·maybe) 템플릿 하나가 모든 종류를 채운다. 값 하나는 seed에서 정해지고, 같은
// seed로 기대값을 다시 만들어 비교한다. 참조는 대상 표(ComboTargets)의 순번으로 정한다 — 쓴 쪽과
// 읽은 쪽 Registry가 같은 순번에 같은 정체성의 객체를 둔다.
namespace rocks::asset_test {

using namespace asset_fixture;

// 참조 대상 표의 순번이다. Null을 뺀 나머지는 쓴 쪽·읽은 쪽 Registry에서 같은 정체성이다.
enum class ComboTarget : uint32 {
    Null,
    Self,   // 같은 패키지 대표 에셋
    Body,   // 같은 패키지 하위 객체
    Socket, // 같은 패키지 깊은 하위 객체(Body.Socket)
    Other,  // 다른 패키지 대표 에셋
    Gear,   // 다른 패키지 하위 객체
    Tooth,  // 다른 패키지 깊은 하위 객체(Gear.Tooth)
    Count
};

inline constexpr uint32 ComboTargetCount = static_cast<uint32>(ComboTarget::Count);

// 다른 패키지 대상인지다(weak는 그래프 밖이라 저장이 null로 바꾼다).
constexpr bool IsOutsideTarget(uint32 index) noexcept {
    return index >= static_cast<uint32>(ComboTarget::Other);
}

// 자리 종류다. 키 자리(Map 키·Set 원소)는 null 참조·같은 값을 만들지 않는다.
enum class ComboSlot : uint8 {
    Value,
    Key
};

// 생성·비교 문맥이다.
struct ComboContext {
    ComboContext(ObjectRegistry& registryIn, TypeRegistry& typesIn, NamePool& namesIn) noexcept
        : registry(registryIn), types(typesIn), names(namesIn),
          allocator(registryIn.ObjectAllocator()) {
    }

    ObjectRegistry& registry;
    TypeRegistry& types;
    NamePool& names;
    Allocator& allocator;
    // 참조 필드를 가진 객체(SetReference·컨테이너 API의 owner)다.
    ObjectHandle owner{};
    ObjectHandle targets[ComboTargetCount] = {};
    // 쓴 쪽(self)·다른 패키지(other)·없는 패키지(absent) id다(Soft 값).
    AssetId self{};
    AssetId other{};
    AssetId absent{};
    // 읽은 쪽이면 참이다(weak의 다른 패키지 대상은 null로 돌아온다).
    bool loaded = false;
    // 참이면 비교기가 seed와 무관하게 기본값인지 본다(쿠킹 패키지의 EditorOnly 필드).
    bool expectDefault = false;
};

// ---- 값 하나 ----

inline uint64 Mix(uint32 seed) noexcept {
    uint64 state = 0x9E3779B97F4A7C15ull * (uint64{seed} + 1);
    state ^= state >> 29;
    state *= 0xBF58476D1CE4E5B9ull;
    return state ^ (state >> 32);
}

template <class T>
    requires(IsIntegralV<T> && !IsSameV<T, bool>)
T IntegerOf(uint32 seed) noexcept {
    // seed마다 다른 값(키 자리의 이웃 seed도 다르다)이고 부호·높은 비트를 고루 쓴다.
    return static_cast<T>(Mix(seed) ^ seed);
}

inline bool Put(ComboContext&, bool& out, uint32 seed, ComboSlot) noexcept {
    out = (seed & 1) != 0;
    return true;
}

inline bool Same(const ComboContext& context, const bool& value, uint32 seed) noexcept {
    return value == (context.expectDefault ? false : (seed & 1) != 0);
}

template <class T>
    requires(IsIntegralV<T> && !IsSameV<T, bool>)
bool Put(ComboContext&, T& out, uint32 seed, ComboSlot) noexcept {
    out = IntegerOf<T>(seed);
    return true;
}

template <class T>
    requires(IsIntegralV<T> && !IsSameV<T, bool>)
bool Same(const ComboContext& context, const T& value, uint32 seed) noexcept {
    return value == (context.expectDefault ? T{} : IntegerOf<T>(seed));
}

template <class T>
    requires IsFloatingPointV<T>
T RealOf(uint32 seed) noexcept {
    // 정확히 표현되는 값(부호 포함)이다.
    return static_cast<T>((static_cast<int32>(seed % 2001) - 1000)) * static_cast<T>(0.125);
}

template <class T>
    requires IsFloatingPointV<T>
bool Put(ComboContext&, T& out, uint32 seed, ComboSlot) noexcept {
    out = RealOf<T>(seed);
    return true;
}

template <class T>
    requires IsFloatingPointV<T>
bool Same(const ComboContext& context, const T& value, uint32 seed) noexcept {
    return value == (context.expectDefault ? T{} : RealOf<T>(seed));
}

inline Guid GuidOf(uint32 seed) noexcept {
    return Guid{Mix(seed) | 1, Mix(seed + 0x10000) ^ seed};
}

inline bool Put(ComboContext&, Guid& out, uint32 seed, ComboSlot) noexcept {
    out = GuidOf(seed);
    return true;
}

inline bool Same(const ComboContext& context, const Guid& value, uint32 seed) noexcept {
    return value == (context.expectDefault ? Guid{} : GuidOf(seed));
}

inline bool Put(ComboContext&, AssetId& out, uint32 seed, ComboSlot) noexcept {
    out = AssetId{GuidOf(seed + 0x20000)};
    return true;
}

inline bool Same(const ComboContext& context, const AssetId& value, uint32 seed) noexcept {
    return value == (context.expectDefault ? AssetId{} : AssetId{GuidOf(seed + 0x20000)});
}

inline Sha256Digest DigestOf(uint32 seed) noexcept {
    Sha256Digest digest{};
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] =
            static_cast<uint8>(Mix(seed) >> (index % 8 * 8)) ^ static_cast<uint8>(index);
    }

    return digest;
}

inline bool Put(ComboContext&, Sha256Digest& out, uint32 seed, ComboSlot) noexcept {
    out = DigestOf(seed);
    return true;
}

inline bool Same(const ComboContext& context, const Sha256Digest& value, uint32 seed) noexcept {
    return value == (context.expectDefault ? Sha256Digest{} : DigestOf(seed));
}

inline ComboTone ToneOf(uint32 seed) noexcept {
    return static_cast<ComboTone>(1 + seed % 3);
}

inline bool Put(ComboContext&, ComboTone& out, uint32 seed, ComboSlot) noexcept {
    out = ToneOf(seed);
    return true;
}

inline bool Same(const ComboContext& context, const ComboTone& value, uint32 seed) noexcept {
    return value == (context.expectDefault ? ComboTone::None : ToneOf(seed));
}

inline ComboMask MaskOf(uint32 seed) noexcept {
    // 1..7 — 이웃 seed는 다른 조합이다(키 자리).
    return static_cast<ComboMask>(1 + seed % 7);
}

inline bool Put(ComboContext&, ComboMask& out, uint32 seed, ComboSlot) noexcept {
    out = MaskOf(seed);
    return true;
}

inline bool Same(const ComboContext& context, const ComboMask& value, uint32 seed) noexcept {
    return value == (context.expectDefault ? ComboMask{} : MaskOf(seed));
}

inline bool Put(ComboContext&, String& out, uint32 seed, ComboSlot) noexcept {
    return Label(out, "text", seed);
}

inline bool Same(const ComboContext& context, const String& value, uint32 seed) noexcept {
    return context.expectDefault ? value.ByteCount() == 0 : LabelIs(value.View(), "text", seed);
}

inline bool Put(ComboContext& context, Name& out, uint32 seed, ComboSlot) noexcept {
    String text(ScratchAllocator());
    if (!Label(text, "name", seed)) {
        return false;
    }

    auto interned = context.names.TryIntern(text.View());
    if (!interned) {
        return false;
    }

    out = interned.Value();
    return true;
}

inline bool Same(const ComboContext& context, const Name& value, uint32 seed) noexcept {
    if (context.expectDefault) {
        return value == Name{};
    }

    String text(ScratchAllocator());
    if (!Label(text, "name", seed)) {
        return false;
    }

    auto found = context.names.TryFind(text.View());
    return found && value == found.Value();
}

// ClassRef<Asset> 값 표다(0 = null — 키 자리는 쓰지 않는다).
inline TypeId ClassOf(uint32 index) noexcept {
    switch (index % 6) {
    case 1:
        return GeneratedType<ComboAsset>::Id();
    case 2:
        return GeneratedType<ComboEditorAsset>::Id();
    case 3:
        return GeneratedType<ComboRequiredAsset>::Id();
    case 4:
        return GeneratedType<TestAsset>::Id();
    case 5:
        return GeneratedType<SimpleAsset>::Id();
    default:
        return TypeId{};
    }
}

inline uint32 TableIndex(uint32 seed, uint32 count, ComboSlot slot) noexcept {
    return slot == ComboSlot::Key ? 1 + seed % (count - 1) : seed % count;
}

inline bool Put(ComboContext&, ClassRef<Asset>& out, uint32 seed, ComboSlot slot) noexcept {
    out = ClassRef<Asset>(ClassOf(TableIndex(seed, 6, slot)));
    return true;
}

// 비교는 키 자리 여부를 모르므로 두 표 위치 중 하나와 같으면 받는다 — 생성기가 seed마다 다른 칸을
// 쓰므로 Value·Key 위치는 비교 함수 인자로 넘긴다.
inline bool SameClass(const ComboContext& context, const ClassRef<Asset>& value, uint32 seed,
                      ComboSlot slot) noexcept {
    return value.Id() == (context.expectDefault ? TypeId{} : ClassOf(TableIndex(seed, 6, slot)));
}

// Soft 값 표다(0 = null). 다른 패키지 대표·하위·깊은 하위, 없는 패키지, 자기 패키지 하위.
struct SoftEntry {
    AssetId asset{};
    StringView path{};
};

inline SoftEntry SoftOf(const ComboContext& context, uint32 index) noexcept {
    switch (index % 6) {
    case 1:
        return {context.other, StringView()};
    case 2:
        return {context.other, "Gear"};
    case 3:
        return {context.other, "Gear.Tooth"};
    case 4:
        return {context.absent, StringView()};
    case 5:
        return {context.self, "Body"};
    default:
        return {};
    }
}

template <class T>
bool Put(ComboContext& context, SoftObjectRef<T>& out, uint32 seed, ComboSlot slot) noexcept {
    const SoftEntry entry = SoftOf(context, TableIndex(seed, 6, slot));
    return out.TryAssign(entry.asset, entry.path).IsOk();
}

template <class T>
bool SameSoft(const ComboContext& context, const SoftObjectRef<T>& value, uint32 seed,
              ComboSlot slot) noexcept {
    const SoftEntry entry =
        context.expectDefault ? SoftEntry{} : SoftOf(context, TableIndex(seed, 6, slot));
    return value.asset == entry.asset && value.objectPath.View() == entry.path;
}

// weak·강한 참조 대상 순번이다(값 자리만 — 키가 될 수 없다).
inline uint32 TargetOf(uint32 seed) noexcept {
    return seed % ComboTargetCount;
}

// 읽은 쪽에서 기대하는 대상이다(weak의 다른 패키지 대상은 null).
inline ObjectHandle ExpectedTarget(const ComboContext& context, uint32 seed, bool weak) noexcept {
    if (context.expectDefault) {
        return ObjectHandle{};
    }

    const uint32 index = TargetOf(seed);
    if (weak && context.loaded && IsOutsideTarget(index)) {
        return ObjectHandle{};
    }

    return context.targets[index];
}

template <class T>
bool Put(ComboContext& context, WeakObjectRef<T>& out, uint32 seed, ComboSlot) noexcept {
    out = WeakObjectRef<T>(context.targets[TargetOf(seed)]);
    return true;
}

template <class T>
bool Same(const ComboContext& context, const WeakObjectRef<T>& value, uint32 seed) noexcept {
    return value.Handle() == ExpectedTarget(context, seed, true);
}

inline bool Put(ComboContext&, ComboPoint& out, uint32 seed, ComboSlot) noexcept {
    out.x = static_cast<int32>(Mix(seed));
    return Label(out.label, "point", seed);
}

inline bool Same(const ComboContext& context, const ComboPoint& value, uint32 seed) noexcept {
    if (context.expectDefault) {
        return value.x == 0 && value.label.ByteCount() == 0;
    }

    return value.x == static_cast<int32>(Mix(seed)) && LabelIs(value.label.View(), "point", seed);
}

inline bool Put(ComboContext&, ComboChecked& out, uint32 seed, ComboSlot) noexcept {
    out.low = static_cast<int32>(seed % 1000) - 500;
    out.high = out.low + static_cast<int32>(seed % 7);
    return true;
}

inline bool Same(const ComboContext& context, const ComboChecked& value, uint32 seed) noexcept {
    if (context.expectDefault) {
        return value.low == 0 && value.high == 0;
    }

    const int32 low = static_cast<int32>(seed % 1000) - 500;
    return value.low == low && value.high == low + static_cast<int32>(seed % 7);
}

// InstancedStruct: seed % 3 — 비어 있음, ComboPayload, ComboChecked(검증 hook 내용).
inline bool Put(ComboContext& context, InstancedStruct<>& out, uint32 seed, ComboSlot) noexcept {
    switch (seed % 3) {
    case 1: {
        if (!out.Emplace<ComboPayload>(context.types, context.allocator)) {
            return false;
        }

        ComboPayload* payload = out.Get<ComboPayload>();
        payload->factor = RealOf<float>(seed);
        payload->steps = IntegerOf<int64>(seed);
        return true;
    }

    case 2: {
        if (!out.Emplace<ComboChecked>(context.types, context.allocator)) {
            return false;
        }

        ComboContext& self = context;
        return Put(self, *out.Get<ComboChecked>(), seed, ComboSlot::Value);
    }

    default:
        out.Reset();
        return true;
    }
}

inline bool Same(const ComboContext& context, const InstancedStruct<>& value,
                 uint32 seed) noexcept {
    if (context.expectDefault || seed % 3 == 0) {
        return value.IsEmpty();
    }

    if (seed % 3 == 1) {
        const ComboPayload* payload = value.Get<ComboPayload>();
        return payload != nullptr && payload->factor == RealOf<float>(seed) &&
               payload->steps == IntegerOf<int64>(seed);
    }

    const ComboChecked* checked = value.Get<ComboChecked>();
    return checked != nullptr && Same(context, *checked, seed);
}

// Text: seed % 3 — 비어 있음, Invariant, Localized.
inline bool Put(ComboContext& context, Text& out, uint32 seed, ComboSlot) noexcept {
    String source(ScratchAllocator());
    if (!Label(source, "text", seed)) {
        return false;
    }

    Result<Text> made = Result<Text>::Failure(ErrorCode::None);
    switch (seed % 3) {
    case 1:
        made = Text::Invariant(context.allocator, source.View());
        break;
    case 2:
        made = Text::Localized(context.allocator, GuidOf(seed + 0x30000), seed, source.View());
        break;
    default:
        out.Reset();
        return true;
    }

    if (!made) {
        return false;
    }

    out = Move(made.Value());
    return true;
}

inline bool Same(const ComboContext& context, const Text& value, uint32 seed) noexcept {
    TextPersistentView view;
    if (!value.TryGetPersistentForm(view)) {
        return false;
    }

    if (context.expectDefault || seed % 3 == 0) {
        return view.form == TextPersistentForm::Empty;
    }

    String source(ScratchAllocator());
    if (!Label(source, "text", seed) || view.source != source.View()) {
        return false;
    }

    if (seed % 3 == 1) {
        return view.form == TextPersistentForm::Invariant;
    }

    return view.form == TextPersistentForm::Localized && view.id == GuidOf(seed + 0x30000) &&
           view.sourceRevision == seed;
}

// ---- 값 하나의 비교 진입점(키 자리를 아는 종류는 slot을 쓴다) ----

template <class T>
bool SameAt(const ComboContext& context, const T& value, uint32 seed, ComboSlot slot) noexcept {
    if constexpr (requires { SameClass(context, value, seed, slot); }) {
        return SameClass(context, value, seed, slot);
    } else if constexpr (requires { SameSoft(context, value, seed, slot); }) {
        return SameSoft(context, value, seed, slot);
    } else {
        (void)slot;
        return Same(context, value, seed);
    }
}

// allocator를 받는 형식은 allocator로, 아니면 기본 생성한다(컨테이너 원소 임시값).
template <class T>
T MakeTemporary(ComboContext& context) noexcept {
    if constexpr (requires(Allocator& allocator) { T(allocator); }) {
        return T(context.allocator);
    } else {
        return T{};
    }
}

// ---- 컨테이너 ----

inline constexpr uint32 ComboArrayLength = 3;
inline constexpr uint32 ComboMapLength = 2;

template <class T>
bool Put(ComboContext& context, Array<T>& out, uint32 seed, ComboSlot) noexcept {
    out.Clear();
    for (uint32 index = 0; index < ComboArrayLength; ++index) {
        T element = MakeTemporary<T>(context);
        if (!Put(context, element, seed * 16 + index, ComboSlot::Value) ||
            !out.TryPushBack(Move(element))) {
            return false;
        }
    }

    return true;
}

template <class T>
bool Same(const ComboContext& context, const Array<T>& value, uint32 seed) noexcept {
    if (context.expectDefault) {
        return value.IsEmpty();
    }

    if (value.Size() != ComboArrayLength) {
        return false;
    }

    for (uint32 index = 0; index < ComboArrayLength; ++index) {
        if (!SameAt(context, value[index], seed * 16 + index, ComboSlot::Value)) {
            return false;
        }
    }

    return true;
}

template <class T, usize N>
bool Put(ComboContext& context, T (&out)[N], uint32 seed, ComboSlot) noexcept {
    for (usize index = 0; index < N; ++index) {
        if (!Put(context, out[index], seed * 16 + static_cast<uint32>(index), ComboSlot::Value)) {
            return false;
        }
    }

    return true;
}

template <class T, usize N>
bool Same(const ComboContext& context, const T (&value)[N], uint32 seed) noexcept {
    for (usize index = 0; index < N; ++index) {
        if (!SameAt(context, value[index], seed * 16 + static_cast<uint32>(index),
                    ComboSlot::Value)) {
            return false;
        }
    }

    return true;
}

template <class T>
bool Put(ComboContext& context, Optional<T>& out, uint32 seed, ComboSlot) noexcept {
    // 홀수 seed만 값을 둔다(두 상태 모두 왕복 — 시험은 홀수·짝수 seed 둘로 돈다).
    out.Reset();
    if (seed % 2 == 0) {
        return true;
    }

    T value = MakeTemporary<T>(context);
    if (!Put(context, value, seed, ComboSlot::Value)) {
        return false;
    }

    out.Emplace(Move(value));
    return true;
}

template <class T>
bool Same(const ComboContext& context, const Optional<T>& value, uint32 seed) noexcept {
    if (context.expectDefault || seed % 2 == 0) {
        return !value.HasValue();
    }

    return value.HasValue() && SameAt(context, value.Value(), seed, ComboSlot::Value);
}

template <class K, class V, class H, class E>
bool Put(ComboContext& context, OrderedMap<K, V, H, E>& out, uint32 seed, ComboSlot) noexcept {
    out.Clear();
    for (uint32 index = 0; index < ComboMapLength; ++index) {
        K key = MakeTemporary<K>(context);
        V value = MakeTemporary<V>(context);
        if (!Put(context, key, seed * 16 + index, ComboSlot::Key) ||
            !Put(context, value, seed * 16 + 8 + index, ComboSlot::Value)) {
            return false;
        }

        auto inserted = out.Insert(Move(key), Move(value));
        if (!inserted || inserted.Value() != InsertOutcome::Inserted) {
            return false;
        }
    }

    return true;
}

template <class K, class V, class H, class E>
bool Same(const ComboContext& context, const OrderedMap<K, V, H, E>& value, uint32 seed) noexcept {
    if (context.expectDefault) {
        return value.IsEmpty();
    }

    if (value.Count() != ComboMapLength) {
        return false;
    }

    uint32 index = 0;
    bool same = true;
    value.ForEach([&](const K& key, const V& item) noexcept {
        same = same && SameAt(context, key, seed * 16 + index, ComboSlot::Key) &&
               SameAt(context, item, seed * 16 + 8 + index, ComboSlot::Value);
        ++index;
    });
    return same;
}

template <class K, class H, class E>
bool Put(ComboContext& context, OrderedSet<K, H, E>& out, uint32 seed, ComboSlot) noexcept {
    out.Clear();
    for (uint32 index = 0; index < ComboMapLength; ++index) {
        K key = MakeTemporary<K>(context);
        if (!Put(context, key, seed * 16 + index, ComboSlot::Key)) {
            return false;
        }

        auto added = out.Add(Move(key));
        if (!added || added.Value() != InsertOutcome::Inserted) {
            return false;
        }
    }

    return true;
}

template <class K, class H, class E>
bool Same(const ComboContext& context, const OrderedSet<K, H, E>& value, uint32 seed) noexcept {
    if (context.expectDefault) {
        return value.IsEmpty();
    }

    if (value.Count() != ComboMapLength) {
        return false;
    }

    uint32 index = 0;
    bool same = true;
    value.ForEach([&](const K& key) noexcept {
        same = same && SameAt(context, key, seed * 16 + index, ComboSlot::Key);
        ++index;
    });
    return same;
}

// ---- 종류별 조합 struct(멤버 이름이 같다) ----

template <class S>
concept ComboKindStruct = requires(S& s) {
    s.value;
    s.array;
    s.fixed;
    s.mapped;
};

template <ComboKindStruct S>
bool Put(ComboContext& context, S& out, uint32 seed, ComboSlot) noexcept {
    bool ok = Put(context, out.value, seed * 8 + 1, ComboSlot::Value) &&
              Put(context, out.array, seed * 8 + 2, ComboSlot::Value) &&
              Put(context, out.fixed, seed * 8 + 3, ComboSlot::Value) &&
              Put(context, out.mapped, seed * 8 + 4, ComboSlot::Value);
    if constexpr (requires { out.keyed; }) {
        ok = ok && Put(context, out.keyed, seed * 8 + 5, ComboSlot::Value);
    }

    if constexpr (requires { out.set; }) {
        ok = ok && Put(context, out.set, seed * 8 + 6, ComboSlot::Value);
    }

    if constexpr (requires { out.maybe; }) {
        ok = ok && Put(context, out.maybe, seed * 8 + 7, ComboSlot::Value);
    }

    return ok;
}

template <ComboKindStruct S>
bool Same(const ComboContext& context, const S& value, uint32 seed) noexcept {
    bool same = SameAt(context, value.value, seed * 8 + 1, ComboSlot::Value) &&
                Same(context, value.array, seed * 8 + 2) &&
                Same(context, value.fixed, seed * 8 + 3) &&
                Same(context, value.mapped, seed * 8 + 4);
    if constexpr (requires { value.keyed; }) {
        same = same && Same(context, value.keyed, seed * 8 + 5);
    }

    if constexpr (requires { value.set; }) {
        same = same && Same(context, value.set, seed * 8 + 6);
    }

    if constexpr (requires { value.maybe; }) {
        same = same && Same(context, value.maybe, seed * 8 + 7);
    }

    return same;
}

inline bool Put(ComboContext&, ComboBits& out, uint32 seed, ComboSlot) noexcept {
    const uint64 bits = Mix(seed);
    out.first = static_cast<uint8>(bits & 1);
    out.second = static_cast<uint8>((bits >> 1) & 1);
    out.wide = static_cast<uint16>((bits >> 2) & 1);
    out.wider = static_cast<uint32>((bits >> 3) & 1);
    out.plain = static_cast<uint8>(bits >> 8);
    return true;
}

inline bool Same(const ComboContext& context, const ComboBits& value, uint32 seed) noexcept {
    const uint64 bits = context.expectDefault ? 0 : Mix(seed);
    return value.first == (bits & 1) && value.second == ((bits >> 1) & 1) &&
           value.wide == ((bits >> 2) & 1) && value.wider == ((bits >> 3) & 1) &&
           value.plain == static_cast<uint8>(bits >> 8);
}

inline bool Put(ComboContext& context, ComboLeaf& out, uint32 seed, ComboSlot) noexcept {
    out.depth = static_cast<int32>(seed);
    return Put(context, out.values, seed * 4 + 1, ComboSlot::Value) &&
           Put(context, out.tags, seed * 4 + 2, ComboSlot::Value);
}

inline bool Same(const ComboContext& context, const ComboLeaf& value, uint32 seed) noexcept {
    return value.depth == (context.expectDefault ? 0 : static_cast<int32>(seed)) &&
           Same(context, value.values, seed * 4 + 1) && Same(context, value.tags, seed * 4 + 2);
}

inline bool Put(ComboContext& context, ComboMiddle& out, uint32 seed, ComboSlot) noexcept {
    return Put(context, out.leaf, seed * 8 + 1, ComboSlot::Value) &&
           Put(context, out.leaves, seed * 8 + 2, ComboSlot::Value) &&
           Put(context, out.byKey, seed * 8 + 3, ComboSlot::Value) &&
           Put(context, out.maybe, seed * 8 + 4, ComboSlot::Value);
}

inline bool Same(const ComboContext& context, const ComboMiddle& value, uint32 seed) noexcept {
    return Same(context, value.leaf, seed * 8 + 1) && Same(context, value.leaves, seed * 8 + 2) &&
           Same(context, value.byKey, seed * 8 + 3) && Same(context, value.maybe, seed * 8 + 4);
}

inline bool Put(ComboContext& context, ComboTop& out, uint32 seed, ComboSlot) noexcept {
    return Put(context, out.middle, seed * 4 + 1, ComboSlot::Value) &&
           Put(context, out.middles, seed * 4 + 2, ComboSlot::Value);
}

inline bool Same(const ComboContext& context, const ComboTop& value, uint32 seed) noexcept {
    return Same(context, value.middle, seed * 4 + 1) && Same(context, value.middles, seed * 4 + 2);
}

// ComboValues의 멤버 목록이다(채우기·비교가 같은 순서·seed를 쓴다).
template <class Values, class Fn>
bool ForEachComboMember(Values& values, Fn&& fn) noexcept {
    return fn(values.cBool, 1) && fn(values.cUInt8, 2) && fn(values.cUInt16, 3) &&
           fn(values.cUInt32, 4) && fn(values.cUInt64, 5) && fn(values.cInt8, 6) &&
           fn(values.cInt16, 7) && fn(values.cInt32, 8) && fn(values.cInt64, 9) &&
           fn(values.cFloat, 10) && fn(values.cDouble, 11) && fn(values.cGuid, 12) &&
           fn(values.cString, 13) && fn(values.cName, 14) && fn(values.cAssetId, 15) &&
           fn(values.cDigest, 16) && fn(values.cEnum, 17) && fn(values.cFlags, 18) &&
           fn(values.cClassRef, 19) && fn(values.cSoft, 20) && fn(values.cWeak, 21) &&
           fn(values.cStruct, 22) && fn(values.cInstanced, 23) && fn(values.cText, 24) &&
           fn(values.cChecked, 25) && fn(values.bits, 26) && fn(values.top, 27);
}

inline bool Put(ComboContext& context, ComboValues& out, uint32 seed, ComboSlot) noexcept {
    return ForEachComboMember(out, [&](auto& member, uint32 salt) noexcept {
        return Put(context, member, seed * 64 + salt, ComboSlot::Value);
    });
}

inline bool Same(const ComboContext& context, const ComboValues& value, uint32 seed) noexcept {
    return ForEachComboMember(value, [&](const auto& member, uint32 salt) noexcept {
        return Same(context, member, seed * 64 + salt);
    });
}

// ---- 강한 참조(Registry API로 쓴다 — owner의 Borrow 안에서 부른다) ----

inline bool PutRef(ComboContext& context, ObjectRef<Object>& field, uint32 seed) noexcept {
    return context.registry
        .SetReference(context.registry.OwnerAccess(), context.owner, field,
                      context.targets[TargetOf(seed)])
        .IsOk();
}

inline bool SameRef(const ComboContext& context, const ObjectRef<Object>& field,
                    uint32 seed) noexcept {
    return field.Handle() == ExpectedTarget(context, seed, false);
}

inline bool PutRefs(ComboContext& context, ObjectRefArray<Object>& array, uint32 seed) noexcept {
    const ObjectAccessContext access = context.registry.OwnerAccess();
    if (!context.registry.ClearReferences(access, context.owner, array)) {
        return false;
    }

    // 모든 대상(null 포함)을 한 번씩 담는다.
    for (uint32 index = 0; index < ComboTargetCount; ++index) {
        if (!context.registry
                 .AddReference(access, context.owner, array,
                               context.targets[TargetOf(seed + index)])
                 .IsOk()) {
            return false;
        }
    }

    return true;
}

inline bool SameRefs(const ComboContext& context, const ObjectRefArray<Object>& array,
                     uint32 seed) noexcept {
    if (context.expectDefault) {
        return array.Size() == 0;
    }

    if (array.Size() != ComboTargetCount) {
        return false;
    }

    for (uint32 index = 0; index < ComboTargetCount; ++index) {
        if (!(array[index] == ExpectedTarget(context, seed + index, false))) {
            return false;
        }
    }

    return true;
}

inline bool PutRefMap(ComboContext& context, ObjectRefMap<int32, Object>& map,
                      uint32 seed) noexcept {
    for (uint32 index = 0; index < ComboTargetCount; ++index) {
        if (!context.registry
                 .SetMapReference(context.registry.OwnerAccess(), context.owner, map,
                                  static_cast<int32>(index) - 3,
                                  context.targets[TargetOf(seed + index)])
                 .IsOk()) {
            return false;
        }
    }

    return true;
}

inline bool SameRefMap(const ComboContext& context, const ObjectRefMap<int32, Object>& map,
                       uint32 seed) noexcept {
    if (context.expectDefault) {
        return map.IsEmpty();
    }

    if (map.Count() != ComboTargetCount) {
        return false;
    }

    uint32 index = 0;
    bool same = true;
    map.View().ForEach([&](const int32& key, const ObjectHandle& target) noexcept {
        same = same && key == static_cast<int32>(index) - 3 &&
               target == ExpectedTarget(context, seed + index, false);
        ++index;
    });
    return same;
}

inline bool PutLinks(ComboContext& context, StructRefArray<ComboRefLink>& links,
                     uint32 seed) noexcept {
    const ObjectAccessContext access = context.registry.OwnerAccess();
    if (!context.registry.ClearElements(access, context.owner, links)) {
        return false;
    }

    for (uint32 index = 0; index < 3; ++index) {
        auto added = context.registry.AddElement(access, context.owner, links);
        if (!added) {
            return false;
        }

        ComboRefLink& link = links[added.Value()];
        link.weight = static_cast<int32>(seed + index);
        if (!context.registry
                 .SetElementReference(access, context.owner, links, added.Value(), link.target,
                                      context.targets[TargetOf(seed + index * 2)])
                 .IsOk()) {
            return false;
        }
    }

    return true;
}

inline bool SameLinks(const ComboContext& context, const StructRefArray<ComboRefLink>& links,
                      uint32 seed) noexcept {
    if (context.expectDefault) {
        return links.Size() == 0;
    }

    if (links.Size() != 3) {
        return false;
    }

    for (uint32 index = 0; index < 3; ++index) {
        if (links[index].weight != static_cast<int32>(seed + index) ||
            !(links[index].target.Handle() == ExpectedTarget(context, seed + index * 2, false))) {
            return false;
        }
    }

    return true;
}

inline bool PutLink(ComboContext& context, ComboRefLink& link, uint32 seed) noexcept {
    link.weight = static_cast<int32>(seed);
    return PutRef(context, link.target, seed);
}

inline bool SameLink(const ComboContext& context, const ComboRefLink& link, uint32 seed) noexcept {
    return link.weight == (context.expectDefault ? 0 : static_cast<int32>(seed)) &&
           SameRef(context, link.target, seed);
}

inline bool PutObjectRefs(ComboContext& context, ComboObjectRefs& refs, uint32 seed) noexcept {
    return PutRef(context, refs.value, seed) && PutRefs(context, refs.array, seed + 1) &&
           PutRef(context, refs.fixed[0], seed + 2) && PutRef(context, refs.fixed[1], seed + 3) &&
           PutRefMap(context, refs.mapped, seed + 4) && PutLink(context, refs.link, seed + 5) &&
           PutLinks(context, refs.links, seed + 6) &&
           PutLink(context, refs.linkFixed[0], seed + 7) &&
           PutLink(context, refs.linkFixed[1], seed + 8);
}

inline bool SameObjectRefs(const ComboContext& context, const ComboObjectRefs& refs,
                           uint32 seed) noexcept {
    return SameRef(context, refs.value, seed) && SameRefs(context, refs.array, seed + 1) &&
           SameRef(context, refs.fixed[0], seed + 2) && SameRef(context, refs.fixed[1], seed + 3) &&
           SameRefMap(context, refs.mapped, seed + 4) && SameLink(context, refs.link, seed + 5) &&
           SameLinks(context, refs.links, seed + 6) &&
           SameLink(context, refs.linkFixed[0], seed + 7) &&
           SameLink(context, refs.linkFixed[1], seed + 8);
}

// ---- 에셋 클래스(ComboAsset·ComboEditorAsset — 같은 이름의 필드) ----

template <class A>
bool PutComboAsset(ComboContext& context, A& asset, uint32 seed) noexcept {
    bool ok = Put(context, asset.fBool, seed + 1, ComboSlot::Value) &&
              Put(context, asset.fUInt8, seed + 2, ComboSlot::Value) &&
              Put(context, asset.fUInt16, seed + 3, ComboSlot::Value) &&
              Put(context, asset.fUInt32, seed + 4, ComboSlot::Value) &&
              Put(context, asset.fUInt64, seed + 5, ComboSlot::Value) &&
              Put(context, asset.fInt8, seed + 6, ComboSlot::Value) &&
              Put(context, asset.fInt16, seed + 7, ComboSlot::Value) &&
              Put(context, asset.fInt32, seed + 8, ComboSlot::Value) &&
              Put(context, asset.fInt64, seed + 9, ComboSlot::Value) &&
              Put(context, asset.fFloat, seed + 10, ComboSlot::Value) &&
              Put(context, asset.fDouble, seed + 11, ComboSlot::Value) &&
              Put(context, asset.fGuid, seed + 12, ComboSlot::Value) &&
              Put(context, asset.fString, seed + 13, ComboSlot::Value) &&
              Put(context, asset.fName, seed + 14, ComboSlot::Value) &&
              Put(context, asset.fAssetId, seed + 15, ComboSlot::Value) &&
              Put(context, asset.fDigest, seed + 16, ComboSlot::Value) &&
              Put(context, asset.fEnum, seed + 17, ComboSlot::Value) &&
              Put(context, asset.fFlags, seed + 18, ComboSlot::Value) &&
              Put(context, asset.fClassRef, seed + 19, ComboSlot::Value) &&
              Put(context, asset.fSoft, seed + 20, ComboSlot::Value) &&
              Put(context, asset.fWeak, seed + 21, ComboSlot::Value) &&
              PutRef(context, asset.fObjectRef, seed + 22) &&
              Put(context, asset.fStruct, seed + 23, ComboSlot::Value) &&
              Put(context, asset.fInstanced, seed + 24, ComboSlot::Value) &&
              Put(context, asset.fText, seed + 25, ComboSlot::Value) &&
              Put(context, asset.fChecked, seed + 26, ComboSlot::Value) &&
              Put(context, asset.fArray, seed + 27, ComboSlot::Value) &&
              Put(context, asset.fFixed, seed + 28, ComboSlot::Value) &&
              Put(context, asset.fMap, seed + 29, ComboSlot::Value) &&
              Put(context, asset.fSet, seed + 30, ComboSlot::Value) &&
              Put(context, asset.fMaybe, seed + 31, ComboSlot::Value) &&
              PutRefs(context, asset.fRefArray, seed + 32) &&
              PutRef(context, asset.fRefFixed[0], seed + 33) &&
              PutRef(context, asset.fRefFixed[1], seed + 34) &&
              PutRefMap(context, asset.fRefMap, seed + 35) &&
              PutLink(context, asset.fLink, seed + 36) &&
              PutLinks(context, asset.fLinks, seed + 37) &&
              PutLink(context, asset.fLinkFixed[0], seed + 38) &&
              PutLink(context, asset.fLinkFixed[1], seed + 39) &&
              Put(context, asset.values, seed + 40, ComboSlot::Value) &&
              PutObjectRefs(context, asset.refs, seed + 41);
    asset.fBit = static_cast<uint8>(seed & 1);
    asset.fOtherBit = static_cast<uint8>((seed >> 1) & 1);
    return ok;
}

template <class A>
bool SameComboAsset(const ComboContext& context, const A& asset, uint32 seed) noexcept {
    const uint32 bits = context.expectDefault ? 0 : seed;
    return Same(context, asset.fBool, seed + 1) && Same(context, asset.fUInt8, seed + 2) &&
           Same(context, asset.fUInt16, seed + 3) && Same(context, asset.fUInt32, seed + 4) &&
           Same(context, asset.fUInt64, seed + 5) && Same(context, asset.fInt8, seed + 6) &&
           Same(context, asset.fInt16, seed + 7) && Same(context, asset.fInt32, seed + 8) &&
           Same(context, asset.fInt64, seed + 9) && Same(context, asset.fFloat, seed + 10) &&
           Same(context, asset.fDouble, seed + 11) && Same(context, asset.fGuid, seed + 12) &&
           Same(context, asset.fString, seed + 13) && Same(context, asset.fName, seed + 14) &&
           Same(context, asset.fAssetId, seed + 15) && Same(context, asset.fDigest, seed + 16) &&
           Same(context, asset.fEnum, seed + 17) && Same(context, asset.fFlags, seed + 18) &&
           SameAt(context, asset.fClassRef, seed + 19, ComboSlot::Value) &&
           SameAt(context, asset.fSoft, seed + 20, ComboSlot::Value) &&
           Same(context, asset.fWeak, seed + 21) && SameRef(context, asset.fObjectRef, seed + 22) &&
           Same(context, asset.fStruct, seed + 23) && Same(context, asset.fInstanced, seed + 24) &&
           Same(context, asset.fText, seed + 25) && Same(context, asset.fChecked, seed + 26) &&
           Same(context, asset.fArray, seed + 27) && Same(context, asset.fFixed, seed + 28) &&
           Same(context, asset.fMap, seed + 29) && Same(context, asset.fSet, seed + 30) &&
           Same(context, asset.fMaybe, seed + 31) &&
           SameRefs(context, asset.fRefArray, seed + 32) &&
           SameRef(context, asset.fRefFixed[0], seed + 33) &&
           SameRef(context, asset.fRefFixed[1], seed + 34) &&
           SameRefMap(context, asset.fRefMap, seed + 35) &&
           SameLink(context, asset.fLink, seed + 36) &&
           SameLinks(context, asset.fLinks, seed + 37) &&
           SameLink(context, asset.fLinkFixed[0], seed + 38) &&
           SameLink(context, asset.fLinkFixed[1], seed + 39) &&
           Same(context, asset.values, seed + 40) &&
           SameObjectRefs(context, asset.refs, seed + 41) && asset.fBit == (bits & 1) &&
           asset.fOtherBit == ((bits >> 1) & 1);
}

} // namespace rocks::asset_test
