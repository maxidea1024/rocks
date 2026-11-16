#pragma once

#include <Object/PathCursor.h>

// 경로 접근 구현 파일(PropertyPathCursor.cpp·PropertyPathRunner.cpp·PropertyPathText.cpp)이 함께
// 쓰는 내부 정의다. 연산 실행기(detail::PathRunner — PathCursor의 friend)를 둔다.
namespace rocks::detail {

// 경로 연산의 실행 순서다(Docs/Design/ReflectionFoundations.ko.md 7.1절 "오류 순서"):
// 입력 형식 → 문맥과 수명 → 단계 순서의 해석(key 값 포함) → 끝 지점 분류(통째, thunk 없음) →
// 참조 값 lease → 단일 검증 지점 → Borrow와 고정 → 생성 thunk 실행(정적 대조, index·key, 길이,
// 키가 없을 때만 넣기 준비, 할당, 게시 직전 재확인).
struct PathRunner {
    static Result<void> Run(PathCursor& cursor, PropertyPath path) noexcept;

    static Result<void> RunTopRead(PathCursor& cursor) noexcept;
    static Result<void> RunTopWrite(PathCursor& cursor) noexcept;

    // key 단계(frame — 필드·키 enum을 채운 뒤)의 키를 해석한다(리플렉션 B9f3): 종류(TypeMismatch
    // (InvalidKey)), wire 정규형·미선언 enum 키(InvalidArgument(InvalidKey)), Name 키의 pool
    // 조회(없으면 keyFound 거짓 — pool을 늘리지 않는다).
    static Result<void> ResolveKey(PathCursor& cursor, PathFrame& frame,
                                   const PathKey& key) noexcept;

    // 마지막 key 단계에 키를 새로 넣기 전의 준비다(생성 thunk가 키가 없을 때만 고정 아래
    // PathCursor::PrepareInsertedKey로 부른다):
    // ClassRef 키의 등록·Base 파생 lease(keyClass_), Name 키의 내용 규칙과 intern(keyName).
    static Result<void> PrepareInsertedKey(PathCursor& cursor, PathFrame& frame) noexcept;

    static PathCursor Make(ObjectRegistry& registry, const ObjectAccessContext& access,
                           const PathTarget& target, PathOperation operation,
                           const FieldValue* input, FieldValue* output,
                           PropertyPathFailure* failure) noexcept {
        return PathCursor(registry, access, target, operation, input, output, failure);
    }

    // Detach의 출력 상자·Attach의 입력 상자와 Attach의 출력 자리를 건다(리플렉션 B9i3c0).
    static void BindBox(PathCursor& cursor, DetachedValue& box, PreparedAttach* attach) noexcept {
        cursor.box_ = &box;
        cursor.attach_ = attach;
    }

    static usize Length(const PathCursor& cursor) noexcept {
        return cursor.length_;
    }
};

} // namespace rocks::detail
