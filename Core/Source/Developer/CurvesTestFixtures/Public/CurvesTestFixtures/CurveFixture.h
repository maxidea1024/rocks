#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Optional.h>
#include <Curves/PackedTangentCurve.h>
#include <Curves/PlainCurve.h>
#include <Curves/StepCurves.h>
#include <Curves/TangentCurve.h>
#include <Meta/Annotations.h>
#include <Object/InstancedStruct.h>
#include <Object/Object.h>

namespace rocks::curves_fixture {

// 커브 값 타입 시험 fixture다(Docs/Design/CurveData.ko.md 16절 — Curves/Tests의 리플렉션·hook
// 시험). 커브를 모든 값 자리(클래스 필드·Array 원소·Optional 내부·Map 값·InstancedStruct 내용)에
// 담는다.
ROCKS_CLASS()
class CurveHolder : public Object {
public:
    explicit CurveHolder(Allocator& allocator) noexcept
        : tangent(allocator), plain(allocator), tangents(allocator), byKey(allocator) {
    }

    ROCKS_PROPERTY() TangentCurve tangent;
    ROCKS_PROPERTY() PlainCurve plain;
    ROCKS_PROPERTY() Array<TangentCurve> tangents;
    ROCKS_PROPERTY() Optional<PlainCurve> maybe;
    ROCKS_PROPERTY() OrderedMap<int32, PlainCurve> byKey;
    ROCKS_PROPERTY() InstancedStruct<ScalarCurveBase> any;
};

// 계단 커브 셋(CV6)을 필드·Array 원소·Optional 내부·Map 값·InstancedStruct 내용에 담는다(Name 값이
// 있어 Registry에 NamePool이 필요하다 — CurveHolder와 나눈 까닭이다).
ROCKS_CLASS()
class StepCurveHolder : public Object {
public:
    explicit StepCurveHolder(Allocator& allocator) noexcept
        : integers(allocator), names(allocator), strings(allocator), stringList(allocator),
          byKey(allocator) {
    }

    ROCKS_PROPERTY() IntegerStepCurve integers;
    ROCKS_PROPERTY() NameStepCurve names;
    ROCKS_PROPERTY() StringStepCurve strings;
    ROCKS_PROPERTY() Array<StringStepCurve> stringList;
    ROCKS_PROPERTY() Optional<NameStepCurve> maybeNames;
    ROCKS_PROPERTY() OrderedMap<int32, IntegerStepCurve> byKey;
    ROCKS_PROPERTY() InstancedStruct<NameStepCurve> anyNames;
};

// 압축 커브(CV8)를 필드·Array 원소·Optional 내부에 담는다(검증 hook·Bytes wire 시험).
ROCKS_CLASS()
class PackedCurveHolder : public Object {
public:
    explicit PackedCurveHolder(Allocator& allocator) noexcept : packed(allocator), list(allocator) {
    }

    ROCKS_PROPERTY() PackedTangentCurve packed;
    ROCKS_PROPERTY() Array<PackedTangentCurve> list;
    ROCKS_PROPERTY() Optional<PackedTangentCurve> maybe;
};

} // namespace rocks::curves_fixture
