#pragma once

#include <Object/PathCursor.h>

// 경로 접근 구현 파일(PropertyPathCursor.cpp·PropertyPathRunner.cpp·PropertyPathText.cpp)이 함께
// 쓰는 내부 정의다. 연산 실행기(detail::PathRunner — PathCursor의 friend)를 둔다.
namespace rocks::detail {

// 경로 연산의 실행 순서다(Docs/Design/ReflectionFoundations.ko.md 7.1절 "오류 순서"):
// 입력 형식 → 문맥과 수명 → 단계 순서의 해석 → 끝 지점 분류(통째, thunk 없음) → 참조 값
// lease → 단일 검증 지점 → Borrow와 고정 → 생성 thunk 실행(정적 대조, index, 길이, 할당,
// 게시 직전 재확인).
struct PathRunner {
    static Result<void> Run(PathCursor& cursor, PropertyPath path) noexcept;

    static Result<void> RunTopRead(PathCursor& cursor) noexcept;
    static Result<void> RunTopWrite(PathCursor& cursor) noexcept;

    static PathCursor Make(ObjectRegistry& registry, const ObjectAccessContext& access,
                           ObjectHandle owner, PathOperation operation, const FieldValue* input,
                           FieldValue* output, PropertyPathFailure* failure) noexcept {
        return PathCursor(registry, access, owner, operation, input, output, failure);
    }

    static usize Length(const PathCursor& cursor) noexcept {
        return cursor.length_;
    }
};

} // namespace rocks::detail
