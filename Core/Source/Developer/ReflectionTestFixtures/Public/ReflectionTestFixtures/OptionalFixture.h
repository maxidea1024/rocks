#pragma once

#include <Core/Containers/Array.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Optional.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/AssetId.h>
#include <Object/Object.h>
#include <Object/SoftObjectRef.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9e Optional PROPERTY fixture다(Docs/Design/ReflectionExtensions.ko.md 6절).
// OptionalMode는 값 0이 선언되지 않은 enum이고, OptionalPoint는 allocator를 받는 값 struct(String
// 멤버와 Optional 멤버를 담는다), OptionalSlot은 Optional 멤버(단일 값·String·값 struct)만 담는
// struct다. OptionalHolder는 내부 종류별 Optional 클래스 필드, 생성자가 set으로 만든
// 필드(preset·presetPoint — 누락 = 기준 시험), Optional 멤버 struct와 그 원소 배열, ReadOnly
// Optional<struct>, 버전 2 struct·Optional<Name> 멤버 struct·Required 멤버 struct를 담고 그래프
// 왕복·마이그레이션·관찰·Required 검사·FieldValue 쓰기·경로 연산으로
// 시험한다(OptionalGraphTests·OptionalGraphLoadTests·OptionalFieldTests).
ROCKS_ENUM()
enum class OptionalMode : uint8 {
    First = 1,
    Second = 2
};

ROCKS_STRUCT()
struct OptionalPoint {
    ROCKS_GENERATED_BODY()

    explicit OptionalPoint(Allocator& allocator) noexcept : label(allocator) {
    }

    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
    ROCKS_PROPERTY() String label;
    ROCKS_PROPERTY() Optional<uint32> weight;
};

ROCKS_STRUCT()
struct OptionalSlot {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Optional<uint32> count;
    ROCKS_PROPERTY() Optional<String> note;
    ROCKS_PROPERTY() Optional<OptionalPoint> point;
};

// 버전 2 struct다(옛 버전 1 내부 Record의 struct 마이그레이션 시험).
ROCKS_STRUCT(Version = 2)
struct OptionalVersioned {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 value = 0;
    ROCKS_PROPERTY() int32 extra = 0;
};

// Optional<Name> 멤버 struct다(관찰 struct 제한 — Name 담김 형질이 Optional 내부로 전파된다).
ROCKS_STRUCT()
struct OptionalTag {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Optional<Name> tag;
};

// Required 멤버를 담는 값 struct다(Optional<struct> 안의 Required 검사 — 저장·복원 2c).
ROCKS_STRUCT()
struct OptionalLink {
    ROCKS_GENERATED_BODY()

    explicit OptionalLink(Allocator& allocator) noexcept : asset(allocator) {
    }

    ROCKS_PROPERTY(Required) SoftObjectRef<> asset;
    ROCKS_PROPERTY() uint32 weight = 0;
};

// 멤버가 없는 struct다(Optional<struct>의 set 여부를 멤버 없이 조회 — PathIsOptionalSet).
ROCKS_STRUCT()
struct OptionalEmpty {
    ROCKS_GENERATED_BODY()
};

ROCKS_CLASS()
struct OptionalHolder final : Object {
    ROCKS_GENERATED_BODY()

    explicit OptionalHolder(Allocator& allocator) noexcept : slots(allocator) {
        preset.Emplace(7u);
        presetPoint.Emplace(allocator);
        presetPoint->x = 3;
        presetPoint->y = 4;
    }

    ROCKS_PROPERTY() Optional<uint32> count;
    ROCKS_PROPERTY() Optional<int8> small;
    ROCKS_PROPERTY() Optional<float> ratio;
    ROCKS_PROPERTY() Optional<bool> flag;
    ROCKS_PROPERTY() Optional<Guid> guid;
    ROCKS_PROPERTY() Optional<Sha256Digest> digest;
    ROCKS_PROPERTY() Optional<String> text;
    ROCKS_PROPERTY() Optional<Name> name;
    ROCKS_PROPERTY() Optional<OptionalMode> mode;
    ROCKS_PROPERTY() Optional<OptionalPoint> point;
    ROCKS_PROPERTY() Optional<uint32> preset;
    ROCKS_PROPERTY() Optional<OptionalPoint> presetPoint;
    ROCKS_PROPERTY() OptionalSlot slot;
    ROCKS_PROPERTY() Array<OptionalSlot> slots;
    ROCKS_PROPERTY(ReadOnly) Optional<OptionalPoint> locked;
    ROCKS_PROPERTY() Optional<AssetId> asset;
    ROCKS_PROPERTY() Optional<double> precise;
    ROCKS_PROPERTY() Optional<uint64> big;
    ROCKS_PROPERTY() Optional<int64> wide;
    ROCKS_PROPERTY() Optional<OptionalVersioned> versioned;
    ROCKS_PROPERTY() OptionalTag tag;
    ROCKS_PROPERTY() Optional<OptionalLink> link;
    ROCKS_PROPERTY() Optional<OptionalEmpty> empty;
};

} // namespace rocks::reflection_fixture
