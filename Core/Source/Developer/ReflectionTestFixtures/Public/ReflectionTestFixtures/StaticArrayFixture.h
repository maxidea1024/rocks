#pragma once

#include <Core/Encoding/Sha256.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/AssetId.h>
#include <Object/ClassRef.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>
#include <Object/SoftObjectRef.h>

namespace rocks::reflection_fixture {

struct FixedOwner;

// 리플렉션 B9g 정적 배열 PROPERTY fixture다(Docs/Design/ReflectionExtensions.ko.md 8절).
// FixedOwner는 성질 표의 정적 배열 원소 종류마다 필드 하나(정수 8종·Bool·실수 둘·Guid·String·
// Name·ObjectRef·WeakRef·ClassRef·enum·값 struct·참조 struct·AssetId·SoftObjectRef·Sha256Digest)를
// 담고 그래프 왕복·Trace(ObjectRef 원소·참조 struct 원소로만 닿는 객체)·순환·경로 연산으로
// 시험한다(StaticArrayGraphTests·StaticArrayPathTests). FixedPoint는 정적 배열 멤버를 담은 값
// struct, FixedLink는 참조 struct 원소, FixedRequired는 Required 원소(원소마다 non-null)와 Required
// 멤버 struct(FixedNeed) 원소다.
ROCKS_ENUM()
enum class FixedMode : uint8 {
    First = 1,
    Second = 2
};

ROCKS_STRUCT()
struct FixedPoint {
    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int16 pair[2] = {};
};

ROCKS_STRUCT()
struct FixedLink {
    ROCKS_PROPERTY() ObjectRef<FixedOwner> target;
    ROCKS_PROPERTY() uint32 weight = 0;
};

ROCKS_CLASS()
struct FixedOwner final : Object {
    explicit FixedOwner(Allocator& allocator) noexcept
        : labels{String(allocator), String(allocator)},
          softs{SoftObjectRef<>(allocator), SoftObjectRef<>(allocator)} {
        // 선언되지 않은 enum 값 0을 남기지 않는다(저장이 거부한다).
        modes[0] = FixedMode::First;
        modes[1] = FixedMode::First;
    }

    ROCKS_PROPERTY() bool flags[3] = {};
    ROCKS_PROPERTY() uint8 bytes[2] = {};
    ROCKS_PROPERTY() uint16 words[2] = {};
    ROCKS_PROPERTY() uint32 counts[2] = {};
    ROCKS_PROPERTY() uint64 bigs[2] = {};
    ROCKS_PROPERTY() int8 tiny[2] = {};
    ROCKS_PROPERTY() int16 smalls[2] = {};
    ROCKS_PROPERTY() int32 ints[4] = {};
    ROCKS_PROPERTY() int64 wides[2] = {};
    ROCKS_PROPERTY() float ratios[2] = {};
    ROCKS_PROPERTY() double precise[2] = {};
    ROCKS_PROPERTY() Guid guids[2] = {};
    ROCKS_PROPERTY() String labels[2];
    ROCKS_PROPERTY() Name names[2];
    ROCKS_PROPERTY() ObjectRef<FixedOwner> refs[3];
    ROCKS_PROPERTY() WeakRef<FixedOwner> watchers[2];
    ROCKS_PROPERTY() ClassRef<FixedOwner> kinds[2];
    ROCKS_PROPERTY() FixedMode modes[2];
    ROCKS_PROPERTY() FixedPoint points[2];
    ROCKS_PROPERTY() FixedLink links[2];
    ROCKS_PROPERTY() AssetId assets[2] = {};
    ROCKS_PROPERTY() SoftObjectRef<> softs[2];
    ROCKS_PROPERTY() Sha256Digest digests[2] = {};
    // 원소 크기만 다르고 wire(U32)·원소 수가 같은 긴 정적 배열 둘이다(2단계 몫이 원소 메모리를 세지
    // 않는지 — StaticArrayGraph_ApplyChargeIgnoresElementSize).
    ROCKS_PROPERTY() uint8 octets[256] = {};
    ROCKS_PROPERTY() uint32 quads[256] = {};
};

// Required 멤버를 담은 값 struct다(정적 배열 원소 안 Required — 진단 경로 [f[i].m]).
ROCKS_STRUCT()
struct FixedNeed {
    ROCKS_PROPERTY(Required) ClassRef<FixedOwner> kind;
};

ROCKS_CLASS()
struct FixedRequired final : Object {
    ROCKS_PROPERTY(Required) ObjectRef<FixedOwner> anchors[2];
    ROCKS_PROPERTY(Required) ClassRef<FixedOwner> kinds[1];
    ROCKS_PROPERTY() FixedNeed needs[2];
};

} // namespace rocks::reflection_fixture
