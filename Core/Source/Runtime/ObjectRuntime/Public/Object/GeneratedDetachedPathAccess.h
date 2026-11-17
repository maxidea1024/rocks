#pragma once

#include <Object/GeneratedPathAccess.h>

// 경로 연산 Detach·Attach(리플렉션 B9i3c0 — Docs/Design/ReflectionExtensions.ko.md 10.2절,
// Object/DetachedPath.h)의 생성 thunk 템플릿이다. 선언은 Object/GeneratedPathAccess.h에 있고 이
// 헤더는 그 끝에서 include된다(생성 헤더는 GeneratedPathAccess.h만 include한다). 모든 멤버 thunk가
// 거치는 PathMember가 끝 단계의 Detach·Attach를 여기로 넘기며, 값 연산은 멤버 형식의 정적 표
// ValueOpsOf<Member>(Object/ValueCopy.h)다. 비템플릿 처리(고정·복사·상자 채우기·자리 기록)는
// PathCursor::DetachValue·AttachValue가 한다.
namespace rocks::detail {

// 상자 안 값(Member 형식)에서 경로를 시작한다(상자의 진입 함수 — 첫 단계는 상자에 담은 필드
// 자신이다).
template <ValueKind Target, class Member>
Result<void> PathEnterDetached(PathCursor& cursor, void* value) noexcept {
    return PathMember<Target>(cursor, *static_cast<Member*>(value));
}

// 끝 단계의 Detach·Attach다. 깊은 복사와 무할당 교환이 모두 되는 형식만 상자에 담는다 — 강한
// 참조·Registry 컨테이너(복사할 수 없다)와 복사 의미가 없는 NATIVE 멤버를 담은 형식은 값 연산 표를
// 만들지 않고 Unsupported(NotCopyable)다.
template <ValueKind Target, class Member>
Result<void> PathDetachedMember(PathCursor& cursor, Member& member) noexcept {
    if constexpr (ValueCopy<Member>::Supported && ValueSwap<Member>::Supported) {
        const RuntimeValueOps& ops = ValueOpsOf<Member>();
        if (cursor.Operation() == PathOperation::Detach) {
            return cursor.DetachValue(ops, &member, &PathEnterDetached<Target, Member>);
        }

        return cursor.AttachValue(ops, &member);
    } else {
        (void)member;
        return cursor.Fail(ErrorCode::Unsupported, PropertyPathError::NotCopyable);
    }
}

} // namespace rocks::detail
