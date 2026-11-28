#pragma once

#include <Core/Containers/Span.h>
#include <Core/Hash/Sha256.h>
#include <Core/Error.h>
#include <Meta/PropertyPathSchema.h>
#include <Meta/TypeSchema.h>

// 클래스 기본값 레코드다(Docs/Design/RuntimeClassMetadata.ko.md '클래스 기본값 구현 명세 — A5a
// 레코드'). 타입 등록 세대 하나에 붙는 불변 값 블록이며 객체가 아니다(UE CDO 대응 — 살아 있는 기본
// 객체를 두지 않는다). 만드는 쪽은 ObjectSerialization `AcquireClassDefaults`이고 이 헤더는 자료와
// 등록 레코드 슬롯의 계약만 둔다.
namespace rocks {

class TypeLease;
class TypeRegistry;

// 레코드 구성이다. 값은 지문의 구성 byte이며 구성이 늘면 다음 번호를 주고 번호를 다시 쓰지 않는다.
// Editor는 저장 정책 제외 {Transient}, Cooked는 {Transient, EditorOnly}다.
enum class ClassDefaultsConfiguration : uint8 {
    Editor = 0,
    Cooked = 1,
};

// 레코드를 만들지 못한 이유다.
enum class ClassDefaultsReason : uint8 {
    None,
    // 추상 타입(construct 없음) — 기본값 조회가 Unsupported다.
    Abstract,
    // completeFieldLayout이 거짓인 타입(저장 자체가 Unsupported).
    IncompleteLayout,
    // base 사슬에 생성 코덱 표지(RuntimeTypeOps::generatedFieldCodecs)가 없는 레코드가 있다(손
    // 함수표 — 레코드 없이 전부 저장한다).
    ManualFields,
    // 생성자를 두 번 실행한 인코딩이 다르다(생성자 결정성 계약 위반 — fieldPath 첫 단계가 첫 다른
    // 필드).
    NonDeterministic,
    // 기본값에 non-null 객체 참조(강한·weak·Soft)가 있다(참조 기본값은 null만 — fieldPath).
    NonNullReference,
    // 그 밖의 codec 오류(error에 원본 오류 코드).
    EncodeFailed,
};

// 레코드 진단이다. 복사할 수 있는 고정 크기 값이라 실패 기억과 함께 등록 레코드에 남는다.
// fieldPath는 그 타입 스키마 기준의 경로 단계다(Map key 단계는 key 값 없이 단계만 —
// FormatPropertyPath로 텍스트를 만들 때 key 단계는 쓰지 못한다).
struct ClassDefaultsDiagnostic {
    ClassDefaultsReason reason = ClassDefaultsReason::None;
    ErrorCode error = ErrorCode::None;
    TypeId type{};
    PathStep fieldPath[MaxPropertyPathSteps]{};
    usize fieldPathSteps = 0;

    Span<const PathStep> FieldPath() const noexcept {
        return Span<const PathStep>(fieldPath, fieldPathSteps);
    }

    void SetFieldPath(Span<const PathStep> steps) noexcept {
        fieldPathSteps = steps.Size() < MaxPropertyPathSteps ? steps.Size() : MaxPropertyPathSteps;
        for (usize index = 0; index < fieldPathSteps; ++index) {
            fieldPath[index] = steps[index];
        }
    }
};

// 게시된 레코드다(불변 — TypeRegistry allocator의 한 블록, 등록 레코드가 해제한다). byte는 그래프
// 저장과 같은 wire의 클래스 Record다(머리에 schema id·version).
class ClassDefaults {
public:
    Span<const uint8> Record(ClassDefaultsConfiguration configuration) const noexcept {
        return configuration == ClassDefaultsConfiguration::Editor ? editor_ : cooked_;
    }

    const Sha256Digest& Fingerprint(ClassDefaultsConfiguration configuration) const noexcept {
        return configuration == ClassDefaultsConfiguration::Editor ? editorFingerprint_
                                                                   : cookedFingerprint_;
    }

private:
    friend class TypeLease;

    Span<const uint8> editor_{};
    Span<const uint8> cooked_{};
    Sha256Digest editorFingerprint_{};
    Sha256Digest cookedFingerprint_{};
};

// 만드는 쪽(builder)이 채우는 초안이다. byte는 builder의 작업 allocator에 있고 게시가 TypeRegistry
// allocator로 복사한다.
struct ClassDefaultsDraft {
    Span<const uint8> editorRecord{};
    Span<const uint8> cookedRecord{};
    Sha256Digest editorFingerprint{};
    Sha256Digest cookedFingerprint{};
};

// 생성 잠금 안에서 부르는 builder다. 성공이면 draft를 채운다. 실패면 그 오류를 돌려주고
// diagnostic에 이유를 둔다(기억 여부는 IsRememberedClassDefaultsFailure). types는 그 lease의
// TypeRegistry다 (ClassRef 판정 — 잠금 보호 Acquire). user는 호출자 문맥이다.
using ClassDefaultsBuilder = Result<void> (*)(void* user, const TypeLease& lease,
                                              TypeRegistry& types, ClassDefaultsDraft& draft,
                                              ClassDefaultsDiagnostic& diagnostic) noexcept;

// 소유 클래스의 선언 subobject 기본값 레코드를 만드는 함수다(Docs/Design/Prefab 10.6절 —
// TypeLease::ResolveSubobjectDefaults가 부른다). lease는 소유 클래스, drafts는 펼친 subobject 표와
// 같은 수·순서의 초안이다(초안 byte는 user가 가진 저장소를 빌리고 게시가 복사한다).
using SubobjectDefaultsBuilder = Result<void> (*)(void* user, const TypeLease& lease,
                                                  TypeRegistry& types,
                                                  Span<ClassDefaultsDraft> drafts,
                                                  ClassDefaultsDiagnostic& diagnostic) noexcept;

// 이유의 영어 이름이다(진단·경고 출력 — "None"·"Abstract"·…·"EncodeFailed").
constexpr const char* ClassDefaultsReasonName(ClassDefaultsReason reason) noexcept {
    switch (reason) {
    case ClassDefaultsReason::None:
        return "None";
    case ClassDefaultsReason::Abstract:
        return "Abstract";
    case ClassDefaultsReason::IncompleteLayout:
        return "IncompleteLayout";
    case ClassDefaultsReason::ManualFields:
        return "ManualFields";
    case ClassDefaultsReason::NonDeterministic:
        return "NonDeterministic";
    case ClassDefaultsReason::NonNullReference:
        return "NonNullReference";
    case ClassDefaultsReason::EncodeFailed:
        return "EncodeFailed";
    }

    return "Unknown";
}

// 레코드 없는 타입의 이유인지다(RuntimeClassMetadata 'A5c 지문 연결' 레코드 유무 분류): 추상·불완전
// 레이아웃·손 함수표는 정상적으로 레코드가 없다(차이 저장은 전부 저장, 지문 없음). 그 밖의 이유는
// 레코드 실패(타입 계약 위반)다.
constexpr bool IsClassWithoutDefaults(ClassDefaultsReason reason) noexcept {
    return reason == ClassDefaultsReason::Abstract ||
           reason == ClassDefaultsReason::IncompleteLayout ||
           reason == ClassDefaultsReason::ManualFields;
}

// 등록 세대 동안 기억하는 실패인지다: 결정적 이유(같은 타입이면 늘 같은 실패)만 기억한다. 일시
// 오류(OutOfMemory·Busy·ShuttingDown·NotFound — ClassRef 대상이 아직 등록 전·접근 오류)는 기억하지
// 않아 다음 호출이 다시 만든다.
constexpr bool
IsRememberedClassDefaultsFailure(const ClassDefaultsDiagnostic& diagnostic) noexcept {
    switch (diagnostic.error) {
    case ErrorCode::OutOfMemory:
    case ErrorCode::Busy:
    case ErrorCode::ShuttingDown:
    case ErrorCode::NotFound:
    case ErrorCode::WrongThread:
    case ErrorCode::WrongContext:
        return false;
    default:
        break;
    }

    return diagnostic.reason != ClassDefaultsReason::None;
}

} // namespace rocks
