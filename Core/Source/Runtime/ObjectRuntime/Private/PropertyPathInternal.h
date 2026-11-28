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
    // validateHooks는 Attach가 상자 값 트리의 검증 hook을 부르는지다(PathPrepareAttach).
    static void BindBox(PathCursor& cursor, DetachedValue& box, PreparedAttach* attach,
                        bool validateHooks = true) noexcept {
        cursor.box_ = &box;
        cursor.attach_ = attach;
        cursor.validateHooks_ = validateHooks;
    }

    // PathPrepareReference의 출력 자리를 건다(리플렉션 B9i3c2).
    static void BindReference(PathCursor& cursor, PreparedReference& out) noexcept {
        cursor.reference_ = &out;
    }

    // QueryInstanced의 출력 lease를 건다(리플렉션 B9j).
    static void BindInstancedType(PathCursor& cursor, TypeLease& out) noexcept {
        cursor.instancedOut_ = &out;
    }

    // PathSetInstancedType의 새 타입 ID를 건다(리플렉션 B9j — 등록 struct 조회·담기 조건·깊이 합
    // 검사는 Run의 검증 지점이 끝 단계 위치 깊이로 한다).
    static void BindInstancedRequest(PathCursor& cursor, TypeId type) noexcept {
        cursor.instancedRequest_ = type;
    }

    // InstancedStruct 단계 index의 담긴 타입을 같은 시작 위치에서 path 앞부분(그 단계까지)으로
    // 조회해 그 단계의 대상 struct로 둔다(리플렉션 B9j). 비어 있으면 NotFound(InstancedEmpty)다.
    static Result<void> ResolveInstancedStep(PathCursor& cursor, PropertyPath path,
                                             usize index) noexcept;

    // PathReadKeys의 출력 키 목록을 건다(리플렉션 B9i4).
    static void BindKeys(PathCursor& cursor, Array<FieldValue>& out) noexcept {
        cursor.keys_ = &out;
    }

    // PathPrepareStructElements의 출력 준비물을 건다(리플렉션 B9i3c4).
    static void BindElements(PathCursor& cursor, PreparedStructElements& out) noexcept {
        cursor.elements_ = &out;
    }

    static usize Length(const PathCursor& cursor) noexcept {
        return cursor.length_;
    }

    // 객체 시작 위치의 변경이 검증 hook 있는 struct 안이면 연산 대신 그 단계를 남기게 한다
    // (Docs/Design/CurveData/README.ko.md 9.1절 — 공개 경로 연산 진입점만 켠다).
    static void RedirectHooks(PathCursor& cursor) noexcept {
        cursor.redirectHooks_ = true;
    }

    // Run의 끝 단계가 Set 원소(키 단계)였는지다.
    static bool EndsAtSetElement(const PathCursor& cursor) noexcept {
        return cursor.setElement_;
    }

    // RedirectHooks를 건 Run이 남긴 가장 바깥 hook struct 단계다(없으면 IndexNone).
    static usize HookStep(const PathCursor& cursor) noexcept {
        return cursor.hookStep_;
    }

    // Run이 최상위 단일 값의 등록된 read·write 경로를 썼는지다(그 쓰기는 스스로 알린다 — CN-D10).
    static bool RanTopValue(const PathCursor& cursor) noexcept {
        return cursor.topValue_;
    }

    // RedirectHooks를 건 Run이 남긴 최상위 필드의 setter다(없으면 nullptr — CN-D14).
    static FieldSetterFunction SetterRedirect(const PathCursor& cursor) noexcept {
        return cursor.setter_;
    }

    // 상자가 담은 값의 주소다(setter thunk가 교환으로 가져간다 — 빈 상자는 nullptr).
    static void* BoxValue(DetachedValue& box) noexcept {
        return box.storage_;
    }
};

// setter 있는 필드 fieldId(최상위)의 등록된 write다(FieldWriteOperation — Docs/Design/
// ChangeNotification.ko.md CN-D14): 한 단계 경로의 Write를 상자 경유로 setter에 넣는다. 검증·실패는
// 경로 쓰기와 같다(ReadOnly는 막지 않는다).
Result<void> WriteFieldThroughSetter(ObjectRegistry& registry, const ObjectAccessContext& access,
                                     ObjectHandle object, Guid fieldId, const FieldValue& value,
                                     FieldSetterFunction setter) noexcept;

} // namespace rocks::detail
