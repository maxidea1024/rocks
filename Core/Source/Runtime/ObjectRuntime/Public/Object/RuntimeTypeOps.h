#pragma once

#include <Base/TypeTraits.h>
#include <Core/Containers/Array.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Lifetime/ModuleLease.h>
#include <Core/Move.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Localization/Text.h>
#include <Meta/PropertyPathSchema.h>
#include <Object/ObjectAccessContext.h>
#include <Object/Object.h>
#include <new>

namespace rocks {

class ConstructionContext;
class ArchiveWriter;
struct FieldView;
class FieldEncodeContext;
class FieldDecodeContext;
struct RuntimeFieldOps;
struct RuntimeStructFieldOps;

namespace detail {
// 경로 접근 cursor다(선언·인터페이스는 공개 detail 헤더 Object/PathCursor.h, 구현은
// Private/PropertyPathCursor.cpp — Docs/Design/ReflectionFoundations.ko.md 7.1절).
class PathCursor;
} // namespace detail

// 경로 접근(B6) thunk다(Docs/Design/ReflectionFoundations.ko.md 7.1절, 생성 템플릿은
// Object/GeneratedPathAccess.h). 필드 하나에 thunk 하나이며 자기 단계(멤버 주소·원소 index)만
// 처리하고, 경로가 끝나지 않았으면 cursor.Descend(struct 값 주소)로 다음 단계 멤버의 thunk를
// 부른다. 클래스 Struct·Array 필드의 path는 cursor가 소유 객체 주소를 준다. struct 멤버의 path는
// structValue(멤버를 가진 struct 값)를 받는다. void*는 생성 thunk와 경로 라이브러리 사이에서만
// 오가며 공개 API는 handle + PropertyPath(Object/PropertyPath.h)뿐이다. 직접 부르지 않는다.
using FieldPathFunction = Result<void> (*)(const RuntimeFieldOps& self,
                                           detail::PathCursor& cursor) noexcept;
using StructFieldPathFunction = Result<void> (*)(const RuntimeStructFieldOps& self,
                                                 detail::PathCursor& cursor,
                                                 void* structValue) noexcept;

// 필드 codec thunk다(Docs/Design/ReflectionFoundations.ko.md 6.4절, 문맥은 Object/FieldCodec.h).
// encode는 필드 값을 payload에 쓰고 wire tag를 돌려준다. decode는 construction이 nullptr이면 1단계
// (검증·참조 기록·진단, 값을 게시하지 않음), 아니면 2단계(staged 객체에 게시)다. self는 이 thunk를
// 가진 함수표다(같은 표의 read·writeStaged를 쓰는 범용 codec용). encode는 context.StagedSource()가
// 있으면(그래프 로드의 Required 실효 값 검사) handle이 그 트랜잭션의 공개 전 객체이므로 Registry
// Resolve(Reserved 객체는 StaleHandle) 대신 ConstructionContext::Get으로 읽는다(생성 thunk는 그렇게
// 하고, 범용 codec은 readStaged를 쓴다). 2단계 decode가 owner에 값을 쓸 때는 GetForWrite로 얻는다.
using FieldEncodeFunction = Result<uint32> (*)(const RuntimeFieldOps& self, ObjectRegistry&,
                                               const ObjectAccessContext&, ObjectHandle,
                                               ArchiveWriter& payload,
                                               FieldEncodeContext& context) noexcept;
using FieldDecodeFunction = Result<void> (*)(const RuntimeFieldOps& self,
                                             ConstructionContext* construction, ObjectHandle,
                                             const FieldView& field,
                                             FieldDecodeContext& context) noexcept;

// 필드 조회 결과를 소유하는 이동 전용 값이다. 문자열도 복제하므로 임시 Borrow의 view를 유출하지
// 않는다. kind와 대응하는 payload만 읽는다. 같은 래퍼의 변경/allocator callback 재진입은 금지다.
// allocator는 값과 진행 중 호출보다 오래 살아야 한다. primitive의 폭 축소는 setter에서 검사한다.
struct FieldValue {
    explicit FieldValue(Allocator& allocator) noexcept : string(allocator), allocator_(&allocator) {
    }

    FieldValue(FieldValue&&) noexcept = default;
    FieldValue& operator=(FieldValue&&) noexcept = default;
    FieldValue(const FieldValue&) = delete;
    FieldValue& operator=(const FieldValue&) = delete;

    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

    ValueKind kind = ValueKind::Opaque;
    bool boolean = false;
    // UInt8·UInt16·UInt32·UInt64 값이다.
    uint64 unsignedInteger = 0;
    // Int8·Int16·Int32·Int64 값과 Enum 값(underlying 8종 모두 int64)이다.
    int64 signedInteger = 0;
    float float32 = 0;
    double float64 = 0;
    // Guid·AssetId 값, SoftObjectReference의 asset, ClassReference 값의 클래스 TypeId(0 = null)다.
    Guid guid{};
    // ObjectReference·WeakObjectReference 값의 대상 handle이다(null = ObjectHandle{}).
    ObjectHandle reference{};
    // Utf8String 값, SoftObjectReference의 객체 경로다.
    String string;
    // Digest256 값이다.
    Sha256Digest digest{};
    // Name 값(실행 중 pool 토큰 — 리플렉션 B9c2)이다. 쓰기는 토큰이 대상 Registry의
    // pool(RegistryOptions::names) 것인지 확인하고(다르면 WrongContext, pool이 없으면 None 말고는
    // Unsupported) 표시 문자열의 내용 규칙(Object/NameField.h)을 본다. 읽기는 토큰을 복사할 뿐
    // 할당하지 않는다(원문은 그 pool의 Acquire로 읽는다).
    Name name{};
    // Text 값이다(리플렉션 B9l — Docs/Design/ReflectionExtensions.ko.md 13절). 노드는 이
    // FieldValue의 allocator(GetAllocator)로 만든다: 읽기는 멤버를 DeepClone하고, 쓰기는 대상
    // Registry의 객체 allocator로 다시 DeepClone하므로 한 Registry의 노드를 다른 수명의 소유자가
    // 쥐지 않는다. 비어 있음(IsValid 거짓)도 값이다. 저장은 비어 있음·Invariant·Localized만 받는다
    // (Format·Integer 이력은 encode Unsupported).
    Text text;
    // Enum 값의 enum 스키마 ID다(B3b).
    TypeId type{};
    // 값이 있는지다(리플렉션 B9e — Docs/Design/ReflectionExtensions.ko.md 6절). Optional<T>
    // 필드(내부가 단일 값·String·Name)는 kind가 내부 종류이고 이 칸이 set 여부다: 읽기는 unset이면
    // false(값 칸은 기본값 — enum이면 type은 그 enum ID), 쓰기는 false면 unset으로 되돌린다(값 칸은
    // 보지 않는다 — enum ID도). 기본은 true라 Optional이 아닌 필드는 이 칸을 신경 쓰지 않아도 된다.
    // Optional이 아닌 필드·배열 원소에 false를 쓰면 InvalidArgument다(unset은 Optional만의 값이다).
    // 생성 읽기·경로 읽기·복원 해석은 이 칸을 늘 정한다(값을 담으면 true — 재사용한 FieldValue에
    // 앞선 unset 읽기의 false가 남지 않는다). 수동 read callback도 같은 규칙을 지킨다.
    //
    // 사용 예(Optional<uint32> 필드를 unset으로):
    //   FieldValue value(allocator);
    //   value.kind = ValueKind::UInt32;
    //   value.present = false;
    bool present = true;

private:
    Allocator* allocator_;
};

class TypeRegistry;
class TypeLease;

namespace detail {

// 필드 쓰기의 바인딩(등록된 원본 스키마 쪽)이다: FieldValue로 쓰는 값 형식(Optional 필드면 내부
// 형식), Optional 필드인지, 읽기 전용 정책.
struct FieldWriteBinding {
    ValueType value{};
    bool optional = false;
    bool readOnly = false;
};

// 필드 쓰기의 실제 대상이다. actual은 대상 객체 스키마의 그 필드, effectiveFlags는 전파된 유효
// 플래그(최상위 필드면 필드 플래그 — B6 경로 쓰기는 바깥 ReadOnly를 전파한다), actualEnum은 그
// 필드의 등록된 enum 스키마(원소 모드면 원소 enum — TypeLease::FieldEnum이 돌려주는 값),
// sourceExpectedNative·targetExpectedNative는 원본·대상 스키마의 참조 기대 타입 native 키(0이면
// Object), referenced는 non-null 참조 값의 대상 타입 lease, element는 원소 모드다: 배열 필드의 원소
// 하나나 Map 필드의 값 하나(Map은 리플렉션 B9f3)를 쓰며, actual은 Array·Map이어야 하고 binding을
// actual->element와 대조한다. null 참조는 단일 참조·ObjectRefArray 원소 모두 받는다(B5 — 원소 null
// 허용). null 판정은 ObjectHandle::IsNull이다(registry 0 또는 generation 0인 비정규 null도 null —
// 쓰는 쪽이 ObjectHandle{}로 정규화한다, 7.1절). lease는 호출자가 얻어 쓰기 완료까지 쥔다.
struct FieldWriteTarget {
    const FieldSchema* actual = nullptr;
    uint32 effectiveFlags = 0;
    const EnumSchema* actualEnum = nullptr;
    const void* sourceExpectedNative = nullptr;
    const void* targetExpectedNative = nullptr;
    const TypeLease* referenced = nullptr;
    bool element = false;
};

// 필드 쓰기 검증의 한 곳이다(Docs/Design/ReflectionFoundations.ko.md 3.6·5.6·6.4절).
// FieldWriteOperation과 경로 쓰기(B6)가 같이 쓴다. lease를 얻지 않는 순수 검사이며 오류 순서는 값
// 종류(원소 모드면 actual이 Array가 아니어도, Optional 여부가 binding.optional과 다르면)·enum
// ID·참조 대상 타입(TypeMismatch) → aggregate 통째 쓰기(Unsupported — Struct·Array, Optional<값
// struct>, 원소 모드의 struct 원소) → 읽기 전용(Unsupported) → 값 내용(Optional이 아닌 자리의
// present = false·소프트 참조 규칙·선언되지 않은 enum 값 InvalidArgument; 정수 폭 Overflow는
// thunk가 검사한다)이다. Optional 필드(리플렉션 B9e)는 actual->element(내부 형식)와 대조하고
// present = false(unset)면 enum ID와 값 내용을 보지 않는다. 그래프 복원의 staged 쓰기는 이 검사를
// 거치지 않는다(읽기 전용은 복원을 막지 않는다).
//
// 사용 예(B6 경로 쓰기가 배열 원소 하나를 쓸 때):
//   detail::FieldWriteTarget target;
//   target.actual = &field;
//   target.actualEnum = lease.FieldEnum(index);   // 원소 enum
//   target.element = true;
//   if (auto valid = detail::ValidateFieldWrite(binding, target, value); !valid) { ... }
Result<void> ValidateFieldWrite(const FieldWriteBinding& binding, const FieldWriteTarget& target,
                                const FieldValue& value) noexcept;

// 컨테이너 필드(Array·Map·Set — Map·Set은 리플렉션 B9f3)의 새 길이를 검사한다(경로 접근의 삽입·
// 크기 변경 — B6, 저장의 MaxElements와 같은 규칙). field가 Array·Map·Set이 아니면 TypeMismatch,
// newLength > field.maxElements면 CapacityExceeded다.
// 로드는 필드별 상한을 걸지 않으므로 이 검사를 거치지 않는다(MaxArrayElements만 — 5.5절).
//
// 사용 예:
//   if (auto fits = detail::ValidateArrayLength(field, array.Size() + 1); !fits) { ... }
Result<void> ValidateArrayLength(const FieldSchema& field, usize newLength) noexcept;

// 배열의 구조 변경 종류다(경로 접근의 원소 삽입·삭제·비우기).
enum class ArrayEdit : uint8 {
    Insert,
    Remove,
    Clear
};

// 컨테이너 구조 변경(배열 원소·Map 항목·Set 원소의 삽입·삭제·비우기 — Map·Set은 리플렉션 B9f3)의
// 검증 한 곳이다(Docs/Design/ReflectionFoundations.ko.md 7.1절). target.actual이 없거나
// Array·Map·Set이 아니면 TypeMismatch, 유효 플래그(target.effectiveFlags —
// 바깥 struct·배열 필드에서 전파된다)가 ReadOnly면 Unsupported다. lease를 얻지 않는 순수 검사다.
// 삽입하는 단일 값 원소 자체는 ValidateFieldWrite(원소 모드)가 따로 검사한다. 고정 길이 배열 같은
// UI 정책(UE EditFixedSize)은 나중에 이 한 곳에 더한다.
//
// 사용 예:
//   detail::FieldWriteTarget target;
//   target.actual = &field;
//   target.effectiveFlags = effectiveFlags;
//   if (auto valid = detail::ValidateArrayEdit(target, detail::ArrayEdit::Clear); !valid) { ... }
Result<void> ValidateArrayEdit(const FieldWriteTarget& target, ArrayEdit edit) noexcept;

// Optional<값 struct> 필드의 기본 값 만들기(EmplaceOptional)·비우기(ResetOptional)의 검증 한
// 곳이다(리플렉션 B9e — Docs/Design/ReflectionExtensions.ko.md 6절). target.actual이 없거나
// Optional<struct>가 아니면 TypeMismatch(단일 값 Optional은 FieldValue::present로 쓴다), 유효
// 플래그(target.effectiveFlags — 바깥 struct·배열 필드에서 전파된다)가 ReadOnly면 Unsupported다.
// lease를 얻지 않는 순수 검사다.
//
// 사용 예:
//   detail::FieldWriteTarget target;
//   target.actual = &field;
//   target.effectiveFlags = effectiveFlags;
//   if (auto valid = detail::ValidateOptionalEdit(target); !valid) { ... }
Result<void> ValidateOptionalEdit(const FieldWriteTarget& target) noexcept;

// InstancedStruct 값의 형 바꾸기(SetInstancedType)·비우기(ClearInstanced)의 검증 한 곳이다(리플렉션
// B9j — Docs/Design/ReflectionExtensions.ko.md 11절). target.actual이 없거나 그 자리(원소 모드면
// element — Array·정적 배열 원소·Map 값, 아니면 value)가 InstancedStruct가 아니면 TypeMismatch,
// 유효 플래그(target.effectiveFlags — 바깥 struct·배열 필드에서 전파된다)가 ReadOnly면
// Unsupported다. lease를 얻지 않는 순수 검사다(새 타입의 등록·담기 조건·깊이 합은 경로 실행기가
// 본다).
//
// 사용 예:
//   detail::FieldWriteTarget target;
//   target.actual = &field;
//   target.effectiveFlags = effectiveFlags;
//   if (auto valid = detail::ValidateInstancedEdit(target); !valid) { ... }
Result<void> ValidateInstancedEdit(const FieldWriteTarget& target) noexcept;

// weak 참조 값(WeakObjectReference — 리플렉션 B9d1)의 대상을 검사한다(Docs/Design/
// ReflectionExtensions.ko.md 5.1절 쓰기 검증). target이 null(IsNull — 비정규 null 포함)이면 성공,
// 다른 Registry가 발급한 handle이면 WrongContext(ObjectRegistry::OwnsHandle)이고, 아니면
// registry.AcquireType(access, target)으로 생존 객체인지 본다(그 오류 — 회수·재사용 StaleHandle,
// 파괴 요청 Destroyed, 스레드·문맥 오류). 대상이 staticNative(NativeTypeKey<T>, nullptr이면
// Object)의 native 파생이 아니면 TypeMismatch다. 스키마 기대 타입은
// ValidateFieldWrite(FieldWriteTarget::referenced)가 본다. 생성 write thunk(WriteGeneratedField)가
// 대입 전에 부른다 — 수동 함수표는 FieldWriteOperation이 스키마 기대 타입으로 검사한다. lease를
// 쥐지 않는 순수 검사다.
//
// 사용 예:
//   if (auto valid = detail::ValidateWeakTarget(registry, access, value.reference,
//                                               NativeTypeKey<Actor>());
//       !valid) {
//       return valid;
//   }
Result<void> ValidateWeakTarget(ObjectRegistry& registry, const ObjectAccessContext& access,
                                ObjectHandle target, const void* staticNative) noexcept;

// ClassRef 값(ValueKind::ClassReference — 리플렉션 B9d2)의 쓰기 검사다(Docs/Design/
// ReflectionExtensions.ko.md 5.2절 표의 "쓰기"). value가 0(null)이면 성공이고, 아니면
// registry.AcquireRegisteredType(access, value)로 연 클래스가 base(스키마의 Base 클래스 ID — 0이면
// 보지 않는다) 자신이거나 그 파생이고 baseNative(정적 Base의 NativeTypeKey — nullptr이면 보지
// 않는다)의 C++ 파생이어야 한다(판정은 detail::CheckClassReferenceLease, Object/ClassRef.h). 미등록
// NotFound, 클래스가 아닌 레코드(struct·enum)·파생 위반 InvalidArgument, 접근·스레드·종료·재진입
// 오류는 그 오류다. FieldWriteOperation(스키마 base)·생성 write와 staged write thunk(정적
// Base)·경로 쓰기가 값 내용 검사로 부른다(읽기 전용 정책 뒤). Deprecated 클래스 거부는 리플렉션
// B9h2가 더한다. lease를 쥐지 않는 순수 검사다.
//
// 사용 예:
//   if (auto valid = detail::ValidateClassReference(registry, access, value.guid, TypeId{},
//                                                   NativeTypeKey<Pawn>());
//       !valid) {
//       return valid;
//   }
Result<void> ValidateClassReference(ObjectRegistry& registry, const ObjectAccessContext& access,
                                    TypeId value, TypeId base, const void* baseNative) noexcept;

} // namespace detail

// 등록된 필드의 실제 대상 schema, 타입과 값 형식을 검사한다. 오류 순서는 context·수명, 타입·필드,
// 전달 값 종류(TypeMismatch — enum은 FieldValue::type이 필드의 enum ID와 다를 때도), 읽기
// 전용(Unsupported), 값 내용(정수 폭 Overflow·소프트 참조의 null 규칙과 경로 상한·선언되지 않은
// enum 값 InvalidArgument, weak 참조는 강한 참조와 같은 대상 검사 — 이 Registry의 생존 객체·기대
// 타입의 파생, ClassRef 값은 detail::ValidateClassReference — 등록된 클래스이고 스키마 Base의 파생,
// 미등록 NotFound·그 밖 InvalidArgument, Name 토큰은 Object/NameField.h의 ValidateNameFieldToken —
// 이 Registry의 pool이 없으면 None 말고 Unsupported, 다른 pool의 토큰 WrongContext, 게시되지 않은
// 토큰 StaleHandle, 표시 문자열 규칙 InvalidArgument)이다. Optional 필드(내부가 단일 값 — 리플렉션
// B9e)는 value.kind가 내부 종류이고 present = false면 unset으로 되돌린다(값 내용 검사 없음).
// Optional이 아닌 필드에 present = false는 InvalidArgument다. aggregate 필드(FieldIsAggregate —
// Struct·Array·Optional<값 struct>)는 FieldValue로 통째 쓰지 않으므로 문맥·수명·대상 타입·필드
// 검사 뒤 Unsupported다(멤버·원소 단위는 B6 경로 접근). 참조
// 값의 비정규 null(IsNull인 non-zero handle)은 null로 정규화한다 — 검증은 null로 하고 callback에는
// reference를 ObjectHandle{}로 바꾼 사본(할당 없음)을 넘긴다(수동 함수표가 비정규 handle을 저장하지
// 않게, 7.1절). 이 연산 자체는 encode 고정을 쥐지 않는다: 생성 callback(WriteGeneratedField)은
// String·Soft 새 값 할당부터 게시까지 스스로 고정해 할당 콜백의 재진입 쓰기를 Busy로 막지만, 수동
// callback은 그렇지 않으므로 할당하는 수동 callback은 같은 규칙을 스스로 지킨다. callback 코드
// 수명은 TypeLease가 보유하는 ModuleLease에 종속되므로 이 값을 lease보다 오래 보관하거나 호출하지
// 않는다.
class FieldWriteOperation {
public:
    using Callback = Result<void> (*)(ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                                      const FieldValue&) noexcept;

    constexpr FieldWriteOperation() noexcept = default;

    // noexcept 함수 포인터로의 예외 없는 변환만 허용한다. 이 생성자로 직접 변환하여
    // 기존 함수표 aggregate 초기화에 두 번의 암묵적 사용자 정의 변환이 필요하지 않게 한다.
    template <class Callable>
        requires requires(Callable value) {
            { static_cast<Callback>(value) } noexcept;
        }
    constexpr FieldWriteOperation(Callable value) noexcept
        : callback_(static_cast<Callback>(value)) {
    }

    explicit constexpr operator bool() const noexcept {
        return callback_ != nullptr;
    }

    Result<void> operator()(ObjectRegistry& registry, const ObjectAccessContext& access,
                            ObjectHandle handle, const FieldValue& value) const noexcept;

private:
    friend class TypeRegistry;

    // 등록된 필드(field — 원본 스키마)의 쓰기 정책을 묶는다. FieldValue로 쓰는 값 형식은 Optional
    // 필드면 내부 형식(element), 아니면 값 형식이다.
    void Bind(TypeRegistry& registry, const void* nativeType, const FieldSchema& field) noexcept {
        const bool optional = field.value.kind == ValueKind::Optional;
        const ValueType& written = optional ? field.element : field.value;
        registry_ = &registry;
        nativeType_ = nativeType;
        fieldId_ = field.id;
        referencedType_ = written.type;
        valueKind_ = written.kind;
        optional_ = optional;
        aggregate_ = FieldIsAggregate(field);
        readOnly_ = HasFlag(field.flags, FieldFlags::ReadOnly);
        bound_ = true;
    }

    Callback callback_ = nullptr;
    TypeRegistry* registry_ = nullptr;
    TypeId referencedType_{};
    const void* nativeType_ = nullptr;
    Guid fieldId_{};
    ValueKind valueKind_ = ValueKind::Opaque;
    bool optional_ = false;
    bool aggregate_ = false;
    bool readOnly_ = false;
    bool bound_ = false;
};

// 함수는 owner context/handle을 검사하는 generated thunk다. offset이나 raw field 주소를 공개하지
// 않는다. read 실패는 out 불변, write 실패는 해당 필드에 새 값을 게시하지 않는다.
// String commit 뒤 이전 buffer 반환 callback의 논리 파괴는 이미 완료된 성공을 되돌리지 않는다.
struct RuntimeFieldOps {
    Guid fieldId{};
    Result<void> (*read)(ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                         FieldValue&) noexcept = nullptr;
    FieldWriteOperation write;
    Result<void> (*writeStaged)(ConstructionContext&, ObjectHandle,
                                const FieldValue&) noexcept = nullptr;
    // 그래프 저장·복원 codec이다. 둘 다 있거나 둘 다 없어야 하고, completeFieldLayout인 타입은
    // Transient가 아닌 모든 필드에 있어야 한다. 수동 함수표는 detail::EncodeThroughFieldValue·
    // DecodeThroughFieldValue(Object/GeneratedFieldCodec.h)를 그대로 쓸 수 있다.
    FieldEncodeFunction encode = nullptr;
    FieldDecodeFunction decode = nullptr;
    // Enum·Struct 필드(또는 Enum·Struct 원소 배열, Map·Set의 값·원소)가 쓰는 C++ 타입의
    // NativeTypeKey다(그 밖의 필드는 nullptr). 등록이 필드의 대상 형식(FieldTargetType — 배열이면
    // 원소)으로 찾은 레코드의 native 키와 같은지 확인한다 — 스키마로 해석한 대상과 정적 thunk의
    // 타입이 어긋나지 않게 한다(어긋나면 TypeMismatch).
    const void* valueNativeType = nullptr;
    // 경로 접근 thunk다(B6 — 생성 코드는 detail::PathClassField). 선택이다(없으면 그 필드 아래 경로
    // 연산은 Unsupported). 생성 코드는 모든 필드에 싣는다(리플렉션 B9i3c0): 게시된 객체의 최상위
    // 단일 값(Optional<단일 값> 포함) 읽기·쓰기는 그래도 read·write 한 경로이고, 단일 값 필드의
    // thunk는 staged 객체의 경로 연산과 Detach·Attach(Object/DetachedPath.h)가 쓴다.
    FieldPathFunction path = nullptr;
    // 공개 전(staged) 객체의 값을 읽는다(선택 — Docs/Design/ObjectFoundations.ko.md 4절). 그래프
    // 로드의 Required 실효 값 검사(적용 뒤·Commit 전)가 수동 함수표의 범용 encode
    // (detail::EncodeThroughFieldValue)로 staged 객체를 읽을 때 read(Registry Resolve — 공개된
    // 객체만) 대신 쓴다. 없으면 Required를 품은 이 필드의 검사가 Unsupported다(그 로드는 rollback).
    // 검사는 로드에 진단 hook이 있을 때만 돌므로 이 Unsupported도 그때만 난다 — 등록은 이 칸이
    // 필요한지 판정할 수 없다(encode thunk가 read를 거치는 범용 codec인지 멤버를 직접 읽는 생성
    // thunk인지 모르고, 생성 코드는 이 칸을 채우지 않는다). 생성 코드는 쓰지 않는다(생성 encode
    // thunk는 멤버를 직접 읽는다). 단일 값 필드만 가질 수 있다 (struct·배열 필드는 등록이
    // InvalidArgument로 막는다). 실패하면 out은 정해지지 않는다. 검사 중 표시가 그 트랜잭션의
    // 쓰기를 막으므로 같은 트랜잭션에 쓰지 않는다.
    //
    // 사용 예(수동 함수표):
    //   ops.readStaged = [](ConstructionContext& transaction, ObjectHandle object,
    //                       FieldValue& out) noexcept -> Result<void> {
    //       auto owner = transaction.Get<Mesh>(object);
    //       if (!owner) {
    //           return Result<void>::Failure(owner.Error());
    //       }
    //
    //       out.kind = ValueKind::ObjectReference;
    //       out.reference = owner.Value()->material.Handle();
    //       return Result<void>::Success();
    //   };
    Result<void> (*readStaged)(ConstructionContext&, ObjectHandle, FieldValue&) noexcept = nullptr;
    // Map 필드의 키가 Enum일 때 키 enum C++ 타입의 NativeTypeKey다(리플렉션 B9f2 — 그 밖의 필드는
    // nullptr). 등록이 키 형식(FieldKeyType)으로 찾은 enum 레코드의 native 키와 대조한다(어긋나면
    // TypeMismatch — valueNativeType과 같은 규칙). ClassRef 키의 Base·Soft 키의 기대 타입은
    // 레코드를 붙잡지 않으므로(값 자리와 같다) 싣지 않는다.
    const void* keyNativeType = nullptr;
};

// 리플렉션 enum 등록의 실행 정보다(TypeRegistry::RegisterEnum). nativeType은 NativeTypeKey<E>()이며
// Enum 필드의 valueNativeType과 대조한다. 동적 모듈이 등록하면 module로 그 키(정적 객체 주소)의
// 수명을 유지한다.
struct RuntimeEnumOps {
    const void* nativeType = nullptr;
    ModuleLease module;
};

class TraceVisitor;

// 리플렉션 struct 멤버의 codec thunk다(Docs/Design/ReflectionFoundations.ko.md 4.6절, 문맥은
// Object/FieldCodec.h). encode는 structValue(멤버를 가진 struct 값)의 멤버를 out에 쓰고 wire tag를
// 돌려준다. decode는 세 방식이다: 검증(construction·structValue null — 값에 손대지 않는다),
// 관찰(construction null, structValue = scratch 임시 값 — 직접 쓴다), 적용(둘 다 non-null — owner
// 객체 안의 struct 주소이며 참조 멤버는 construction으로 연결한다).
using StructFieldEncodeFunction = Result<uint32> (*)(const RuntimeStructFieldOps& self,
                                                     const void* structValue, ArchiveWriter& out,
                                                     FieldEncodeContext& context) noexcept;
using StructFieldDecodeFunction = Result<void> (*)(const RuntimeStructFieldOps& self,
                                                   ConstructionContext* construction,
                                                   ObjectHandle owner, void* structValue,
                                                   const FieldView& field,
                                                   FieldDecodeContext& context) noexcept;

// struct 멤버 하나의 함수표다. valueNativeType은 Enum·Struct 멤버(또는 Enum·Struct 원소 배열
// 멤버의 원소, Map·Set 멤버의 값·원소)가 쓰는 C++ 타입의 NativeTypeKey다(그 밖은 nullptr — 등록이
// 대상 레코드의 native 키와 대조한다). path는 경로 접근 thunk다(B6, 선택 — 생성 코드는
// detail::PathStructMember. 없으면 이 멤버로 들어가는 경로 연산은 Unsupported).
struct RuntimeStructFieldOps {
    Guid fieldId{};
    StructFieldEncodeFunction encode = nullptr;
    StructFieldDecodeFunction decode = nullptr;
    const void* valueNativeType = nullptr;
    StructFieldPathFunction path = nullptr;
    // Map 멤버의 키가 Enum일 때 키 enum의 NativeTypeKey다(리플렉션 B9f2 — RuntimeFieldOps::
    // keyNativeType과 같은 규칙, 그 밖은 nullptr). 생성 코드는 detail::StructFieldOps가 채운다.
    const void* keyNativeType = nullptr;
};

// struct의 옛 버전 Record를 현재 버전으로 올린다(ObjectGraphMigration과 같은 계약 — 전달
// allocator로 소유 결과를 반환하고, 결과는 정확히 targetVersion이어야 한다).
using StructMigration = Result<Array<uint8>> (*)(Allocator&, TypeId type, uint32 sourceVersion,
                                                 uint32 targetVersion,
                                                 Span<const uint8> sourceRecord) noexcept;

// Trace 호출 동안만 유효한 방문자다. 보관하거나 다른 스레드에 전달하지 않는다.
// 객체가 가진 모든 강한 ObjectRef를 빠짐없이 Visit해야 한다. WeakObjectRef는 방문하지
// 않는다(WeakObjectRef PROPERTY 포함 — 생성 Trace는 weak 필드·원소를 방문하지 않는다,
// Docs/Design/ReflectionExtensions.ko.md 5.1절).
class TraceVisitor {
public:
    using VisitFn = void (*)(void*, ObjectHandle) noexcept;

    TraceVisitor(void* state, VisitFn visit) noexcept : state_(state), visit_(visit) {
    }

    void Visit(ObjectHandle handle) noexcept {
        visit_(state_, handle);
    }

    template <class Ref>
    void Visit(const Ref& reference) noexcept {
        Visit(reference.Handle());
    }

private:
    void* state_;
    VisitFn visit_;
};

// 리플렉션 struct 등록의 실행 정보다(TypeRegistry::RegisterStruct, 4.6절). construct·destroy는
// struct hook 관찰용 임시 값을 만들고 없앤다(필수). trace는 참조 struct(ObjectRef·ObjectRefArray·
// StructRefArray 멤버가 있는 — Transient 포함, 중첩 struct 포함 — struct)에만 있어야 하고 생성
// Trace는 모든 배열 멤버에 detail::TraceArrayField를 부른다. trace는 GeneratedType<S>::Trace와 같은
// 간선을 방문해야 한다: 관찰자 Registry는 StructRefArray 원소를 떼어 낼 때 이 trace로, Commit·
// Discard는 소유자 Trace 안의 GeneratedType<S>::Trace로 세므로 둘이 다르면 계수가 어긋난다(생성
// 코드는 trace를 GeneratedType<S>::Trace로 만든다). migrate는 선택이며 타입이 자기 옛
// 버전을 올린다(로드 옵션 override가 먼저다). requiresWorldOnlyOwner는 생성 형질(NATIVE 멤버의
// WorldOnly 요구)이다. fields는 스키마 필드와 1:1이며 등록이 스키마 순서로 깊게 복사한다. 동적
// 모듈이면 module로 코드 수명을 유지한다. 등록된 레코드의 StructOps()에서 module은 비어 있고(수명은
// lease의 Ops().module이 쥔다) requiresWorldOnlyOwner는 멤버 struct에서 전파된 값이다.
struct RuntimeStructOps {
    usize size = 0;
    usize alignment = 0;
    const void* nativeType = nullptr;
    void (*construct)(void*, Allocator&) noexcept = nullptr;
    void (*destroy)(void*) noexcept = nullptr;
    void (*trace)(const void*, TraceVisitor&) noexcept = nullptr;
    StructMigration migrate = nullptr;
    bool requiresWorldOnlyOwner = false;
    ModuleLease module;
    Span<const RuntimeStructFieldOps> fields;
    // 깊은 복사다(리플렉션 B9i3c0 — Docs/Design/ReflectionExtensions.ko.md 10.2절, UE
    // UScriptStruct::CopyScriptStruct 대응): destination(살아 있는 S 값)을 source의 깊은 복사로
    // 바꾼다. 새 값은 allocator로 따로 다 만든 뒤 무할당 이동으로 넣으므로 실패하면(OutOfMemory 등)
    // destination·source 모두 그대로다. 생성 코드는 GeneratedType<S>::CopyValue(PROPERTY·NATIVE
    // 멤버 모두)를 싣고, 복사할 수 없는 멤버(ObjectRef·Registry 컨테이너·복사 의미가 없는 NATIVE
    // 타입)가 있으면 null이다. 수동 함수표는 null이어도 된다(그 struct의 분리된 값 연산이
    // Unsupported).
    Result<void> (*copy)(void* destination, const void* source,
                         Allocator& allocator) noexcept = nullptr;
    // 두 S 값의 내용을 무할당·무실패로 맞바꾼다(MakeRuntimeStructOps — S가 nothrow 이동 생성·대입을
    // 가지면 채운다, 아니면 null). 버퍼는 각자의 allocator와 함께 옮겨 간다.
    void (*swap)(void* first, void* second) noexcept = nullptr;
    // struct 검증 hook이다(Docs/Design/CurveData.ko.md 9.1절 — `static Result<void> Validate(const
    // S&) noexcept`를 선언한 struct만 채운다, MetaCompiler 감지). 리플렉션 경로(graph decode·대입
    // 묶음·JSON import·경로 쓰기)가 바뀐 값을 게시하기 전에 S 값 하나를 검증한다. 계약: 할당하지
    // 않고 전역 상태·Registry를 읽지 않는 순수 함수이고 아무 스레드에서나 불린다. 값을 고치지
    // 않는다. 결과는 Success 또는 InvalidArgument다(생성 thunk가 다른 오류를 InvalidArgument로
    // 바꾼다). 강한 참조를 담은 struct는 hook을 가질 수 없다(등록이 InvalidArgument로 거부한다 —
    // hook struct는 언제나 통째로 복사해 검증할 수 있는 값이다).
    Result<void> (*validate)(const void* value) noexcept = nullptr;
    // S 값 트리 전체의 검증이다: 멤버(base 하위 객체 먼저, 그다음 PROPERTY 멤버 순서 — Array·정적
    // 배열 원소·Optional 내부·Map 키와 값·InstancedStruct 내용 포함) 안의 hook 있는 struct를
    // 안쪽부터 검증하고 마지막에 S 자신의 validate를 부른다. 트리 어디에도 hook이 없으면
    // null이다(InstancedStruct 멤버가 있으면 담긴 타입이 정해지지 않으므로 채운다). 같은 계약이고
    // 첫 거부에서 멈춘다.
    Result<void> (*validateTree)(const void* value) noexcept = nullptr;
};

// 필드·멤버 값 하나의 타입 무관 연산 표다(리플렉션 B9i3c0 — Docs/Design/ReflectionExtensions.ko.md
// 10.2절, UE FProperty::InitializeValue·CopyCompleteValue·DestroyValue 대응). C++ 타입마다
// detail::ValueOpsOf<T>()(Object/ValueCopy.h)가 정적 표를 준다. 대입 묶음이 값 부분 트리의 사본을
// 만들고(copy) 게시 때 옮기는(swap) 데 쓴다.
//
// 사용 예:
//   const RuntimeValueOps& ops = detail::ValueOpsOf<Array<String>>();
//   alignas(Array<String>) unsigned char storage[sizeof(Array<String>)];
//   ops.construct(storage, allocator);
//   auto copied = ops.copy(storage, &source, allocator);   // 실패하면 storage는 빈 배열 그대로
//   ops.destroy(storage);
struct RuntimeValueOps {
    usize size = 0;
    usize alignment = 0;
    // NativeTypeKey<T>()다(분리된 값과 경로 thunk의 멤버 형식을 대조한다).
    const void* nativeType = nullptr;
    // storage(size·alignment를 지키는 빈 저장소)에 기본 값을 만든다(noexcept T(Allocator&)가 있으면
    // allocator를 넘기고 없으면 T() — 정적 배열은 원소마다).
    void (*construct)(void* storage, Allocator& allocator) noexcept = nullptr;
    // 값을 소멸한다(저장소는 반환하지 않는다).
    void (*destroy)(void* value) noexcept = nullptr;
    // 깊은 복사(RuntimeStructOps::copy와 같은 계약 — 실패하면 destination 그대로). 복사할 수 없는
    // 타입은 null이다.
    Result<void> (*copy)(void* destination, const void* source,
                         Allocator& allocator) noexcept = nullptr;
    // 무할당 교환이다. 교환할 수 없는 타입은 null이다.
    void (*swap)(void* first, void* second) noexcept = nullptr;
    // 값 트리 안의 hook 있는 struct를 안쪽부터 검증한다(RuntimeStructOps::validateTree와 같은 계약
    // — Docs/Design/CurveData.ko.md 9.1절). 트리에 hook이 없는 타입은 null이다.
    Result<void> (*validateTree)(const void* value) noexcept = nullptr;
};

// 생성·추적·소멸 콜백은 noexcept이며 Registry 잠금 밖에서 실행된다.
// Trace는 읽기만 수행한다. 콜백 중 생성·간선 변경·수집 재진입은 Busy로 거부한다.
// lease 반환과 통계 조회는 허용한다. 활성 ConstructionContext의 롤백/소멸 재진입은 금지한다.
// 동적 모듈의 함수 포인터를 제공한다면 module로 코드 수명도 유지해야 한다.
struct RuntimeTypeOps {
    usize size = 0;
    usize alignment = 0;
    const void* nativeType = nullptr;
    Object* (*construct)(void*, Allocator&) noexcept = nullptr;
    void (*destroy)(Object*) noexcept = nullptr;
    void (*trace)(const Object&, TraceVisitor&) noexcept = nullptr;
    ModuleLease module;
    // 등록 시 배열 자체도 깊게 복사한다. callback 코드 수명은 위 module이 보존한다.
    Span<const RuntimeFieldOps> fields;
    // 기반 C++ 타입의 NativeTypeKey다. Resolve<기반>이 Object*에서 기반 포인터로 static_cast하는
    // 근거이므로, 등록 타입이 이 기반을 단일 public 비가상 상속한다는 것을 설정하는 쪽이 컴파일 시
    // 보증해야 한다(RegisterNativeClass·메타 생성 코드는 static_assert(IsBaseOfV)를 거친다).
    // 수동 함수표에서 임의로 설정하지 않는다. TypeRegistry는 기반 레코드 일치·크기·정렬 하한만
    // 검사한다.
    const void* nativeBaseType = nullptr;
    // 기본 수동 Trace는 owner 전용이다. side effect 없는 generated Trace만 명시 opt-in한다.
    bool parallelTraceSafe = false;
    // 모든 영속 필드와 모든 강한 간선이 typed metadata에 포함된 경우만 true다. ROCKS_NATIVE 멤버는
    // 영속 필드가 아니고 강한 참조를 담을 수 없으므로(MetaCompiler RMC2205·생성 static_assert) 생성
    // 타입은 계속 true다. graph serializer는 이 선언 없는 수동/부분 metadata를 완전한 객체로
    // 저장하지 않는다. true면 trace도 Registry API로 쓰는 모든 참조(ObjectRef·컨테이너 원소)를
    // 방문해야 한다 — 패키지 정책 ObjectRegistry는 이 선언이 있는 타입만 만든다(관찰자 계수가
    // 편집은 Registry API로, Commit·Discard는 trace로 센다).
    bool completeFieldLayout = false;
};

// noexcept T(Allocator&)가 있으면 registry allocator를 주입하고, 없으면 noexcept T()를 사용한다.
// Abstract 메타데이터는 생성 callback 없이 등록하고 생성 시도는 Unsupported로 거부한다.
// 실패 가능한 초기화는 생성자 밖의 ConstructionContext 단계에서 처리한다.
// Trace가 없는 타입은 강한 간선이 없는 것으로 취급한다. 메타데이터 자동 추적은 없다.
template <class T, bool Abstract = IsAbstractV<T>>
RuntimeTypeOps MakeRuntimeTypeOps(ModuleLease module = {}) noexcept {
    static_assert(IsBaseOfV<Object, T>);
    static_assert(
        Abstract || IsNothrowConstructibleV<T, Allocator&> || IsNothrowConstructibleV<T>,
        "Object types need `explicit T(Allocator&) noexcept` or `T() noexcept`; a derived "
        "class whose base takes Allocator& must forward it (e.g. `using Super::Super;`)");
    static_assert(IsNothrowDestructibleV<T>, "Object types need a noexcept destructor");

    RuntimeTypeOps ops;
    ops.size = sizeof(T);
    ops.alignment = alignof(T);
    ops.nativeType = NativeTypeKey<T>();
    if constexpr (!Abstract) {
        ops.construct = [](void* memory, Allocator& allocator) noexcept -> Object* {
            if constexpr (IsConstructibleV<T, Allocator&>) {
                static_assert(IsNothrowConstructibleV<T, Allocator&>);
                return ::new (memory) T(allocator);
            } else {
                return ::new (memory) T();
            }
        };
    }

    ops.destroy = [](Object* object) noexcept { static_cast<T*>(object)->T::~T(); };
    ops.trace = [](const Object& object, TraceVisitor& visitor) noexcept {
        if constexpr (requires(const T& value) { value.Trace(visitor); }) {
            static_assert(noexcept(static_cast<const T&>(object).Trace(visitor)));
            static_cast<const T&>(object).Trace(visitor);
        }
    };
    ops.module = Move(module);
    return ops;
}

// 리플렉션 struct 등록 정보(RuntimeStructOps)의 타입 공통 칸을 채운다(Docs/Design/
// ReflectionFoundations.ko.md 4.6절). MetaCompiler 생성 코드 GeneratedType<S>::Ops가 부르고 trace·
// migrate·requiresWorldOnlyOwner·fields는 생성 코드가 채운다(migrate는 struct가 Migrate를 선언했을
// 때
// `&S::Migrate` — 생성 특수화는 ROCKS_GENERATED_BODY의 friend이므로 private Migrate도 연결되고,
// 시그니처가 다르면 컴파일 오류다). 채우는 것: size·alignment·nativeType, construct(noexcept
// S(Allocator&)가 있으면 allocator를 주입하고 없으면 noexcept S()), destroy, swap(nothrow 이동이
// 있을 때), module. copy는 생성 코드가 GeneratedType<S>::CopyValue로 채운다.
//
// 리플렉션 struct와 배열 원소의 일반 규칙(7.1절): 이동된(moved-from) 값의 소멸자는 allocator를
// 부르지 않는다(NATIVE 멤버 포함 — String·Array처럼 버퍼를 넘겨주고 비어야 한다). 경로 연산의 원소
// 삭제·비우기(원소를 지역으로 옮겨 뺀 뒤 게시하고 소멸), Array::TryEmplaceAt의 용량 안 삽입과
// TryRemoveAt의 원소 이동이 이것에 기대어 게시 전에 allocator 콜백이 끼지 않게 한다.
//
// 사용 예(생성 코드가 내는 형태):
//   auto ops = MakeRuntimeStructOps<BulkDataRef>(Move(module));
//   ops.fields = Span<const RuntimeStructFieldOps>(fields, 3);
template <class S>
RuntimeStructOps MakeRuntimeStructOps(ModuleLease module = {}) noexcept {
    static_assert(!IsBaseOfV<Object, S>, "Reflected structs are values, not Object types");
    static_assert(IsNothrowConstructibleV<S, Allocator&> || IsNothrowConstructibleV<S>,
                  "Reflected structs need `explicit S(Allocator&) noexcept` or `S() noexcept`");
    static_assert(IsNothrowDestructibleV<S>, "Reflected structs need a noexcept destructor");

    RuntimeStructOps ops;
    ops.size = sizeof(S);
    ops.alignment = alignof(S);
    ops.nativeType = NativeTypeKey<S>();
    ops.construct = [](void* memory, Allocator& allocator) noexcept {
        if constexpr (IsConstructibleV<S, Allocator&>) {
            static_assert(IsNothrowConstructibleV<S, Allocator&>);
            ::new (memory) S(allocator);
        } else {
            ::new (memory) S();
        }
    };
    ops.destroy = [](void* memory) noexcept { static_cast<S*>(memory)->S::~S(); };
    if constexpr (IsNothrowMoveConstructibleV<S> && IsNothrowMoveAssignableV<S>) {
        ops.swap = [](void* first, void* second) noexcept {
            S& left = *static_cast<S*>(first);
            S& right = *static_cast<S*>(second);
            S held(Move(left));
            left = Move(right);
            right = Move(held);
        };
    }

    ops.module = Move(module);
    return ops;
}

} // namespace rocks
