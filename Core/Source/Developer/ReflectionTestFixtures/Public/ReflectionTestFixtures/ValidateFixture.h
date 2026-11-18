#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Optional.h>
#include <Core/Result.h>
#include <Meta/Annotations.h>
#include <Object/InstancedStruct.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>

namespace rocks::reflection_fixture {

// struct 검증 hook fixture다(Docs/Design/CurveData.ko.md 9.1절 — ValidateHookGraphTests·
// ValidateHookPathTests·TextValidateHookTests). ValidRange(low ≤ high), ValidBox(ValidRange 멤버와
// range.high ≤ limit — 중첩 순서), ValidDerived(base ValidRange의 hook을 물려받고 extra ≥ 0을
// 더한다), ValidRefElement(강한 참조와 hook 멤버 — StructRefArray 원소)와 그것들을 모든 자리(struct
// 필드·Array 원소·Optional 내부·Map 값·InstancedStruct 내용·정적 배열 원소·StructRefArray 원소
// 멤버)에 담는 ValidHolder다.
//
// 시험이 호출 순서를 보도록 hook마다 스레드별 호출 기록(ValidateLog)에 글자 하나를 남긴다(r·b·d).
// 실제 hook은 전역 상태를 쓰지 않는다 — 이 기록은 시험 관찰용이다.
struct ValidateLog {
    char calls[64] = {};
    usize count = 0;

    void Clear() noexcept {
        count = 0;
    }

    void Note(char call) noexcept {
        if (count < sizeof(calls)) {
            calls[count] = call;
        }

        ++count;
    }

    // 기록이 text와 같은지다.
    bool Is(const char* text) const noexcept {
        usize length = 0;
        while (text[length] != '\0') {
            ++length;
        }

        if (length != count || count > sizeof(calls)) {
            return false;
        }

        for (usize index = 0; index < length; ++index) {
            if (calls[index] != text[index]) {
                return false;
            }
        }

        return true;
    }
};

inline ValidateLog& CurrentValidateLog() noexcept {
    thread_local ValidateLog log;
    return log;
}

ROCKS_STRUCT()
struct ValidRange {
    ROCKS_PROPERTY() int32 low = 0;
    ROCKS_PROPERTY() int32 high = 0;

    static Result<void> Validate(const ValidRange& value) noexcept {
        CurrentValidateLog().Note('r');
        return value.low <= value.high ? Result<void>::Success()
                                       : Result<void>::Failure(ErrorCode::InvalidArgument);
    }
};

ROCKS_STRUCT()
struct ValidBox {
    ROCKS_PROPERTY() ValidRange range;
    ROCKS_PROPERTY() int32 limit = 100;

    // 다른 오류 코드도 InvalidArgument로 다뤄진다(생성 thunk).
    static Result<void> Validate(const ValidBox& value) noexcept {
        CurrentValidateLog().Note('b');
        return value.range.high <= value.limit ? Result<void>::Success()
                                               : Result<void>::Failure(ErrorCode::Overflow);
    }
};

ROCKS_STRUCT()
struct ValidDerived : ValidRange {
    ROCKS_PROPERTY() int32 extra = 0;

    static Result<void> Validate(const ValidDerived& value) noexcept {
        CurrentValidateLog().Note('d');
        return value.extra >= 0 ? Result<void>::Success()
                                : Result<void>::Failure(ErrorCode::InvalidArgument);
    }
};

// 기본값이 hook을 어기는 struct다(count > 0 — 값을 만드는 경로 연산이 기본값을 검증하는지 본다, n).
ROCKS_STRUCT()
struct ValidNonEmpty {
    ROCKS_PROPERTY() int32 count = 0;

    static Result<void> Validate(const ValidNonEmpty& value) noexcept {
        CurrentValidateLog().Note('n');
        return value.count > 0 ? Result<void>::Success()
                               : Result<void>::Failure(ErrorCode::InvalidArgument);
    }
};

// 자기 hook은 없고 hook 멤버를 담는다(트리에만 hook이 있다).
ROCKS_STRUCT()
struct ValidWrapper {
    ROCKS_PROPERTY() ValidNonEmpty item;
};

// 컨테이너·Optional·InstancedStruct를 담은 hook struct다(values ≤ 3개, 키 집합 map ≤ 2개 — l).
// 경로의 원소 넣기· 지우기·비우기·키 넣기·지우기·Optional 만들기·비우기·형 바꾸기·비우기가 hook
// 안을 바꾸는 경우다.
ROCKS_STRUCT()
struct ValidList {
    explicit ValidList(Allocator& allocator) noexcept : values(allocator), map(allocator) {
    }

    ROCKS_PROPERTY() Array<int32> values;
    ROCKS_PROPERTY() OrderedSet<int32> map;
    ROCKS_PROPERTY() Optional<ValidRange> opt;
    ROCKS_PROPERTY() InstancedStruct<> inner;

    static Result<void> Validate(const ValidList& value) noexcept {
        CurrentValidateLog().Note('l');
        return value.values.Size() <= 3 && value.map.Count() <= 2
                   ? Result<void>::Success()
                   : Result<void>::Failure(ErrorCode::InvalidArgument);
    }
};

// hook 없는 바깥 struct다(hook struct가 경로의 둘째 단계 — 실패 단계 재매핑 시험).
ROCKS_STRUCT()
struct ValidOuter {
    explicit ValidOuter(Allocator& allocator) noexcept : list(allocator) {
    }

    ROCKS_PROPERTY() ValidList list;
};

class ValidHolder;

ROCKS_STRUCT()
struct ValidRefElement {
    ROCKS_PROPERTY() ObjectRef<ValidHolder> link;
    ROCKS_PROPERTY() ValidRange range;
};

ROCKS_CLASS()
class ValidHolder : public Object {
public:
    explicit ValidHolder(Allocator& allocator) noexcept
        : ranges(allocator), byKey(allocator), elements(allocator), outer(allocator),
          wrappers(allocator), byKeyEmpty(allocator) {
    }

    ROCKS_PROPERTY() ValidBox box;
    ROCKS_PROPERTY() Array<ValidRange> ranges;
    ROCKS_PROPERTY() Optional<ValidRange> maybe;
    ROCKS_PROPERTY() OrderedMap<int32, ValidRange> byKey;
    ROCKS_PROPERTY() InstancedStruct<> any;
    ROCKS_PROPERTY() ValidRange fixed[2];
    ROCKS_PROPERTY() ValidDerived derived;
    ROCKS_PROPERTY() StructRefArray<ValidRefElement> elements;
    ROCKS_PROPERTY() int32 plain = 0;
    ROCKS_PROPERTY() ValidOuter outer;
    ROCKS_PROPERTY() Array<ValidWrapper> wrappers;
    ROCKS_PROPERTY() Optional<ValidNonEmpty> maybeEmpty;
    ROCKS_PROPERTY() OrderedMap<int32, ValidNonEmpty> byKeyEmpty;
};

} // namespace rocks::reflection_fixture
