#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Optional.h>
#include <Curves/PlainCurve.h>
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

} // namespace rocks::curves_fixture
