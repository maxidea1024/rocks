#pragma once

#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Meta/TypeSchema.h>
#include <Object/ObjectRegistry.h>

// 리플렉션 속성 경로 접근이다(Docs/Design/ReflectionFoundations.ko.md 7·7.1절). 경로(PropertyPath —
// FieldId 체인 + 원소 index, MetaSchema)로 객체 하나 안의 단일 값을 읽고 쓰고, 배열 원소를 넣고
// 빼고 비운다. 문자열 경로("slots[2].material")는 ParsePropertyPath가 타입 스키마로 PropertyPath를
// 만들고 FormatPropertyPath가 되돌린다. 실제 접근은 생성 경로 thunk(RuntimeFieldOps::path·
// RuntimeStructFieldOps::path — Object/GeneratedPathAccess.h)가 정적 타입으로 하고, 검증은 쓰기의
// 단일 지점(detail::ValidateFieldWrite·ValidateArrayEdit)을 지난다. Archive에 의존하지 않는다.
namespace rocks {

// 경로 연산이 실패한 사유다(PropertyPathFailure::reason). 같은 ErrorCode의 원인을 구별해 도구가
// 모르는 속성·범위 오류·값 오류를 나눠 보고하게 한다. 괄호는 함께 돌아오는 ErrorCode다.
//   None              — 사유가 없는 오류(문맥·수명·Busy·OutOfMemory·Overflow·Registry 대상
//                       검사·게시 직전 재확인), 단계 0개·정규형이 아닌 단계(InvalidArgument),
//                       유효 플래그가 ReadOnly가 아닌데 등록된 write가 낸 Unsupported
//   Syntax            — 문자열 경로 문법(InvalidArgument)
//   TooDeep           — MaxPropertyPathSteps(17) 단계 초과(InvalidArgument)
//   UnknownProperty   — 문자열 경로의 모르는 이름(NotFound — 도구가 AS 4.6절 UnknownProperty
//                       진단으로 바꾼다)
//   UnknownField      — FieldId 체인의 없는 필드(스키마가 바뀐 옛 경로, NotFound)
//   NotArray          — 배열이 아닌 필드에 index(`[`)·길이·비우기(TypeMismatch)
//   NotStruct         — struct가 아닌 값(index 없는 배열 포함) 뒤의 단계(`.`)(TypeMismatch)
//   IndexRequired     — 삽입·삭제의 마지막 단계에 index가 없음(InvalidArgument)
//   IndexNotAllowed   — 길이·비우기의 마지막 단계에 index가 있음(InvalidArgument)
//   IndexOutOfRange   — 원소 index가 범위 밖(InvalidArgument)
//   InvalidValue      — 값 내용·종류(InvalidArgument — 값 종류나 enum ID가 필드와 다르면
//                       TypeMismatch), 삽입 값 누락
//   ReadOnly          — 유효 플래그(바깥 struct·배열 필드에서 전파)의 ReadOnly(Unsupported)
//   AggregateValue    — struct·배열을 통째로 읽기·쓰기, struct 원소 삽입에 값·제거에 removed
//                       (Unsupported, removed는 InvalidArgument)
//   NoPathAccess      — 경로 thunk(또는 최상위 read·write)가 없는 필드·멤버, Opaque 필드로 끝나는
//                       읽기·쓰기, previous를 요청했는데 최상위 필드에 read가 없음(Unsupported)
//   TooManyElements   — 삽입 뒤 길이가 필드 MaxElements 초과(CapacityExceeded)
//   StaticTypeMismatch — 생성 thunk의 정적 타입과 등록 스키마가 어긋남(TypeMismatch)
enum class PropertyPathError : uint8 {
    None,
    Syntax,
    TooDeep,
    UnknownProperty,
    UnknownField,
    NotArray,
    NotStruct,
    IndexRequired,
    IndexNotAllowed,
    IndexOutOfRange,
    InvalidValue,
    ReadOnly,
    AggregateValue,
    NoPathAccess,
    TooManyElements,
    StaticTypeMismatch
};

// 경로 연산·문자열 해석의 실패 위치와 사유다. step은 실패한 단계의 index다: 경로 연산에서 문맥·
// 수명(AcquireType)과 Borrow 획득 오류는 0, 17단계 초과는 17이고, 고정·게시 직전 재확인·Registry
// 호출 오류는 그 연산을 하던 단계다. offset은 문자열 해석의 byte 위치다(경로 연산은 0). 연산을
// 시작할 때 비우고 실패할 때 채운다.
struct PropertyPathFailure {
    PropertyPathError reason = PropertyPathError::None;
    usize step = 0;
    usize offset = 0;
};

// 해석한 경로를 담는 고정 크기 버퍼다(할당 없음). View()는 이 버퍼가 사는 동안 유효하다.
//
// 사용 예:
//   PropertyPathBuffer path;
//   if (auto parsed = ParsePropertyPath(lease, "slots[2].material", path); !parsed) { ... }
//   auto written = PathWrite(registry, access, mesh, path.View(), value);
struct PropertyPathBuffer {
    PathStep steps[MaxPropertyPathSteps];
    usize count = 0;

    PropertyPath View() const noexcept {
        return PropertyPath{Span<const PathStep>(steps, count)};
    }
};

// 문자열 경로를 type(클래스 또는 struct lease)의 스키마로 해석해 out에 쓴다. 문법은
// `name ('.' name | '[' index ']')*`이다: 이름은 ASCII 식별자이고 필드의 실효 도구 이름(toolName,
// 없으면 C++ 이름 — toolName이 있으면 C++ 이름은 받지 않는다)과 ASCII 대소문자 무시로 비교하며
// Opaque 필드는 찾지 않는다. index는 정규 10진(앞자리 0은 `0` 하나만, 부호·공백·uint32 초과
// 거부)이다. 파생 클래스 스키마는 상속 필드를 같은 ID로 담으므로 base lease로 해석한 경로를 파생
// 객체에 쓸 수 있다. 할당하지 않고 스레드와 무관하다(lease만 있으면 된다).
//
// 오류(failure가 있으면 사유·단계·byte 위치를 채운다, out은 실패하면 바뀌지 않는다):
//   InvalidArgument — 빈 lease·enum lease(None), 문법(Syntax), 17단계 초과(TooDeep)
//   NotFound        — 모르는 이름(UnknownProperty — offset은 이름 시작)
//   TypeMismatch    — 배열이 아닌 필드·원소 뒤의 `[`(NotArray), struct가 아닌 값 뒤의
//   `.`(NotStruct)
//
// 사용 예:
//   PropertyPathBuffer path;
//   PropertyPathFailure failure;
//   if (auto parsed = ParsePropertyPath(lease, text, path, &failure); !parsed) {
//       if (failure.reason == PropertyPathError::UnknownProperty) { ... }   // AS 4.6절 진단
//   }
Result<void> ParsePropertyPath(const TypeLease& type, StringView text, PropertyPathBuffer& out,
                               PropertyPathFailure* failure = nullptr) noexcept;

// path를 type(클래스 또는 struct lease)의 스키마로 문자열 경로로 바꿔 out에 쓴다(실효 도구 이름 —
// ParsePropertyPath와 왕복한다). out의 allocator로 한 번 할당하고 실패하면 out은 바뀌지 않는다.
// 오류: 빈·enum lease, 단계 0개·17 초과·정규형이 아닌 단계는 InvalidArgument, 없는 필드는 NotFound,
// 경로 thunk가 닿을 수 없는 Opaque 필드는 Unsupported, 모양 불일치(배열이 아닌 필드의
// index·struct가 아닌 값 뒤의 단계)는 TypeMismatch, 할당 실패는 OutOfMemory다.
//
// 사용 예:
//   String text(allocator);
//   if (auto formatted = FormatPropertyPath(lease, path.View(), text); formatted) { ... }
Result<void> FormatPropertyPath(const TypeLease& type, PropertyPath path, String& out) noexcept;

// ---- 경로 연산(소유 스레드 전용) ----
//
// 공통 계약:
// - 경로는 객체 하나 안에서 끝난다(참조를 따라 다른 객체로 들어가지 않는다). handle은 게시된 생존
//   객체여야 하고(staged 객체는 decode의 몫이다) 해석은 그 객체의 실제 타입 스키마로 한다.
// - 끝 지점: 최상위 단일 값 필드는 등록된 read·write(FieldWriteOperation — 수동 함수표도 같다),
//   struct 멤버 단일 값(String·Soft·enum 포함)과 Array<E> 원소는 경로 thunk, 참조 멤버는
//   SetReference(StructRefArray 원소 안이면 SetElementReference), ObjectRefArray 원소는 Registry
//   컨테이너 API(SetReferenceAt·InsertReferenceAt·RemoveReferenceAt·ClearReferences),
//   StructRefArray는 InsertElementAt·RemoveElementAt·ClearElements(원소 멤버는 멤버 단위)다.
//   struct·배열을 통째로 읽거나 쓰면 Unsupported(AggregateValue)이고 Resize·원소 이동·복제는 없다.
// - 검증: 바깥 struct·배열 필드의 플래그를 PropagateFieldFlags로 누적한다 — ReadOnly는 멤버·원소
//   쓰기와 삽입·삭제·비우기를 모두 막고(Unsupported), EditorOnly·Transient는 막지 않는다. 값 쓰기·
//   단일 값 원소 삽입은 ValidateFieldWrite, 구조 변경은 ValidateArrayEdit를 지난다. 삽입 뒤 길이는
//   필드 MaxElements를 넘을 수 없다(CapacityExceeded). 참조 값의 비정규 null(IsNull인 non-zero
//   handle)은 ObjectHandle{}로 정규화한다.
// - 원자성: 쓰기는 ResolveForWrite로 Borrow를 얻는다. owner가 encode 고정 중이면 Busy,
//   DestroyRequested owner는 Destroyed다(정리 중 제거·비우기는 Registry API를 직접 쓴다).
//   경로가 직접 할당하는 끝 지점(String·Soft 쓰기, Array<E> 삽입, 다른 allocator로의 옛 값
//   복사)은 할당부터 게시까지 owner를 encode 고정해 할당 콜백의 재진입 변경을 Busy로 막고,
//   게시 직전 owner 생존을 다시 확인한다(고정은 RequestDestroy를 막지 않는다 — Destroyed면
//   게시하지 않는다). 고정은 게시 또는 실패 직후 풀리고, 옛 값·옛 버퍼와 실패한 연산의 임시
//   버퍼(새 값·옛 값 사본·새 원소·새 배열 버퍼)는 그 뒤에 반환한다. 최상위 String·Soft 필드는
//   등록된 생성 write(WriteGeneratedField)가 같은 고정을 스스로 쥔다 — 수동 함수표의 write
//   callback에는 이 고정이 없다(할당하는 수동 callback은 같은 규칙을 스스로 지킨다). 실패하면
//   대상은 바뀌지 않는다(Registry 컨테이너는 그 API의 실패 보장). 같은 배열을 C++ 코드로 직접
//   바꾸는 콜백은 계약 밖이다(Array.h의 재진입 계약). 참조 struct는 struct 단위 원자성이 없다(멤버
//   하나의 쓰기만 원자적이다).
// - 오류 순서: 입력 형식(단계 0개·17 초과·정규형 위반·Opaque 값·index 유무) InvalidArgument → 문맥·
//   수명(AcquireType) → 단계 순서의 해석(없는 ID NotFound, 모양 불일치 TypeMismatch) → 통째 접근·
//   thunk 없음 Unsupported → 참조 값의 기대 타입·대상 lease(NotFound·StaleHandle) →
//   ValidateFieldWrite와 ValidateArrayEdit(TypeMismatch → Unsupported(ReadOnly) →
//   InvalidArgument) → Borrow와 고정(Busy, Destroyed) → 정적 타입 대조(TypeMismatch) →
//   index InvalidArgument → 길이 CapacityExceeded → 정수 폭 Overflow → Registry 대상
//   검사(StaleHandle·TypeMismatch) → 할당 OutOfMemory → 게시 직전 owner 재확인 Destroyed.
// - failure가 있으면 실패 단계와 사유를 채운다(PropertyPathError).

// path의 단일 값을 out에 읽는다(out의 allocator로 복사). 읽기는 Resolve + encode 고정 아래 복사하고
// out을 게시하기 전에 owner를 다시 확인한다(String·Soft 복사의 할당 콜백이 원본을 바꾸면 Busy).
// 실패하면 out은 바뀌지 않는다.
//
// 사용 예:
//   FieldValue value(allocator);
//   auto read = PathRead(registry, access, mesh, path.View(), value);
Result<void> PathRead(ObjectRegistry& registry, const ObjectAccessContext& access,
                      ObjectHandle handle, PropertyPath path, FieldValue& out,
                      PropertyPathFailure* failure = nullptr) noexcept;

// path의 단일 값(배열 원소 교체 포함)을 value로 쓴다. previous가 있으면 성공했을 때 쓰기 전 값을
// 담는다(CU4 Undo용): String·Soft는 previous의 allocator가 대상 값의 allocator와 같으면 옛 버퍼를
// 옮기고 (추가 할당 없음), 다르면 고정 아래 게시 전에 previous의 allocator로 복사한다. 최상위 단일
// 값 필드의 previous는 write 직전에 등록된 read로 얻는다(read와 write 사이의 재진입 변경은 계약
// 밖 — 그 read 전에 쓰기 검증(ValidateFieldWrite)과 고정 Busy를 먼저 판정한다). previous가 쥐던 옛
// 버퍼는 고정을 푼 뒤 반환하고, 실패하면 previous는 바뀌지 않는다. 참조의 옛 handle은 약한
// 참조다: 간선을 지운 뒤에는 그 대상을 살리는 것이 없으므로, 호출자(CU4)가 RootRef로 붙잡기
// 전까지(이 연산의 해제 콜백 안 GC 포함) 옛 대상이 회수될 수 있다 — 되살리기 전에 handle의 생존을
// 확인한다.
//
// 사용 예:
//   FieldValue previous(allocator);
//   auto written = PathWrite(registry, access, mesh, path.View(), value, &previous);
Result<void> PathWrite(ObjectRegistry& registry, const ObjectAccessContext& access,
                       ObjectHandle handle, PropertyPath path, const FieldValue& value,
                       FieldValue* previous = nullptr,
                       PropertyPathFailure* failure = nullptr) noexcept;

// path(마지막 단계는 index 없는 배열 필드)의 원소 수를 돌려준다. 마지막 단계에 index가 있으면
// InvalidArgument(IndexNotAllowed), 배열이 아니면 TypeMismatch(NotArray)다.
Result<usize> PathArrayLength(ObjectRegistry& registry, const ObjectAccessContext& access,
                              ObjectHandle handle, PropertyPath path,
                              PropertyPathFailure* failure = nullptr) noexcept;

// path 마지막 단계(index가 있어야 한다 — 없으면 InvalidArgument(IndexRequired))의 배열에서 index
// 위치에 원소를 넣는다(index == 길이면 끝, 넘으면 InvalidArgument(IndexOutOfRange)). 단일 값 원소는
// value가 필수이고(없으면 InvalidArgument(InvalidValue)) struct 원소(Array<S>·StructRefArray)는 값
// 없이 기본 원소(S(Allocator&) 또는 S())를 넣는다(value를 주면 Unsupported(AggregateValue)).
// ObjectRefArray는 InsertReferenceAt, StructRefArray는 InsertElementAt을 쓴다. Array<E>는 원소와
// (용량이 모자라면) 새 버퍼를 먼저 만든 뒤 무할당으로 옮겨 게시하고 옛 버퍼는 게시 뒤에 반환한다.
//
// 사용 예:
//   const PathStep steps[] = {{LodsId, lods.Size(), true}};   // 끝에 넣기
//   auto inserted = PathInsertElement(registry, access, mesh, PropertyPath{steps}, &value);
Result<void> PathInsertElement(ObjectRegistry& registry, const ObjectAccessContext& access,
                               ObjectHandle handle, PropertyPath path, const FieldValue* value,
                               PropertyPathFailure* failure = nullptr) noexcept;

// path 마지막 단계(index가 있어야 한다)의 배열 원소를 지운다(뒤 원소를 당긴다). removed가 있으면
// 성공했을 때 지운 단일 값 원소의 값을 담는다(allocator 규칙과 참조 handle이 약한 참조라는 점은
// PathWrite의 previous와 같다). struct
// 원소에 removed를 주면 InvalidArgument(AggregateValue)다 — struct 원소의 옛 값은 CU4가 연산 전
// PathRead로 잎마다 만들고 "기본 원소 삽입 + 잎 쓰기"로 재생한다. 떼어 낸 원소는 게시 뒤 소멸한다.
Result<void> PathRemoveElement(ObjectRegistry& registry, const ObjectAccessContext& access,
                               ObjectHandle handle, PropertyPath path,
                               FieldValue* removed = nullptr,
                               PropertyPathFailure* failure = nullptr) noexcept;

// path(마지막 단계는 index 없는 배열 필드)의 원소를 모두 지운다. Array<E>는 용량까지
// 반환한다(원소와 버퍼는 게시 뒤 반환). 옛 값은 CU4가 연산 전 PathRead로 만든다.
Result<void> PathClearArray(ObjectRegistry& registry, const ObjectAccessContext& access,
                            ObjectHandle handle, PropertyPath path,
                            PropertyPathFailure* failure = nullptr) noexcept;

} // namespace rocks
