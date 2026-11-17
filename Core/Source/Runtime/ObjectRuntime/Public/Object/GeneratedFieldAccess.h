#pragma once

#include <Base/TypeTraits.h>
#include <Meta/GeneratedType.h>
#include <Object/AssetId.h>
#include <Object/ClassRef.h>
#include <Object/ConstructionContext.h>
#include <Object/InstancedStruct.h>
#include <Object/NameField.h>
#include <Object/SoftObjectRef.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Optional.h>
#include <Core/ScopeExit.h>

namespace rocks::detail {

template <class T>
struct ObjectReferenceField {
    static constexpr bool Supported = false;
};

template <class T>
struct ObjectReferenceField<ObjectRef<T>> {
    static constexpr bool Supported = true;
    using Target = T;
};

// weak 참조 PROPERTY(WeakRef<T> — 리플렉션 B9d1, Docs/Design/ReflectionExtensions.ko.md 5.1절)다.
// GC가 추적하지 않는 값 필드이고 Target은 정적 기대 대상 타입이다.
template <class T>
struct WeakReferenceField {
    static constexpr bool Supported = false;
};

template <class T>
struct WeakReferenceField<WeakRef<T>> {
    static constexpr bool Supported = true;
    using Target = T;
};

// 클래스 참조 PROPERTY(ClassRef<Base> — 리플렉션 B9d2, Docs/Design/ReflectionExtensions.ko.md
// 5.2절)다. 클래스 TypeId 값 필드이고 Base는 정적 Base 클래스다(판정은 NativeTypeKey<Base> — Base의
// 생성 헤더가 없어도 된다).
template <class T>
struct ClassReferenceField {
    static constexpr bool Supported = false;
};

template <class Base>
struct ClassReferenceField<ClassRef<Base>> {
    static constexpr bool Supported = true;
    using Target = Base;
};

template <class T>
struct SoftReferenceField {
    static constexpr bool Supported = false;
};

template <class T>
struct SoftReferenceField<SoftObjectRef<T>> {
    static constexpr bool Supported = true;
};

// 리플렉션 enum(ROCKS_ENUM)이다: 생성 헤더가 GeneratedType<E>::Id·IsDeclared를 내는 enum class다.
template <class T>
struct ReflectedEnumField {
    static constexpr bool Supported = IsEnumV<T> && requires {
        GeneratedType<T>::Id();
        GeneratedType<T>::IsDeclared(int64{});
    };
};

// 리플렉션 struct(ROCKS_STRUCT)다: 생성 헤더가 GeneratedType<S>::Id·ContainsStrongReference를 내는
// 비enum 클래스다(Docs/Design/ReflectionFoundations.ko.md 4.6절).
template <class T>
struct ReflectedStructField {
    static constexpr bool Supported = !IsEnumV<T> && IsClassV<T> && requires {
        GeneratedType<T>::Id();
        GeneratedType<T>::ContainsStrongReference;
    };
};

// 리플렉션 배열 필드의 컨테이너다(Docs/Design/ReflectionFoundations.ko.md 5.6절). 부분 특수화로만
// 고르며(requires 탐지 없음) Element는 원소의 C++ 형식이다: Array<E>는 E(스칼라·String·Name·Guid·
// AssetId·Digest·enum·SoftObjectRef·WeakRef·ClassRef·참조 없는 struct), ObjectRefArray<T>는
// ObjectRef<T>(원소 null 허용), StructRefArray<S>는 참조 struct S다. Registry는 원소를 Registry가
// 관리하는 컨테이너(ObjectRefArray·StructRefArray)인지다.
template <class T>
struct ArrayField {
    static constexpr bool Supported = false;
};

template <class E>
struct ArrayField<Array<E>> {
    static constexpr bool Supported = true;
    static constexpr bool Registry = false;
    using Element = E;
};

template <class T>
struct ArrayField<ObjectRefArray<T>> {
    static constexpr bool Supported = true;
    static constexpr bool Registry = true;
    using Element = ObjectRef<T>;
};

template <class S>
struct ArrayField<StructRefArray<S>> {
    static constexpr bool Supported = true;
    static constexpr bool Registry = true;
    using Element = S;
};

// 리플렉션 Optional 필드(Optional<T> — 리플렉션 B9e, Docs/Design/ReflectionExtensions.ko.md 6절)다.
// 부분 특수화로만 고르며 Inner는 내부 C++ 형식(단일 값·String·Name·enum·값 struct)이다.
template <class T>
struct OptionalField {
    static constexpr bool Supported = false;
};

template <class T>
struct OptionalField<Optional<T>> {
    static constexpr bool Supported = true;
    using Inner = T;
};

// 리플렉션 Map·Set 필드(OrderedMap<K, V>·OrderedSet<T> — 리플렉션 B9f2, ObjectRefMap<K, T> —
// B9f4, Docs/Design/ReflectionExtensions.ko.md 7.2·7.4절)다. 부분 특수화로만 고르며 기본 해시·같음
// 정책(SeededHash·Equal)인 컨테이너만 받는다(다른 정책은 Supported가 거짓 — 리플렉션하지 않는다).
// Key는 키(Set이면 원소) C++ 형식, Value는 값(Set이면 원소) C++ 형식(ObjectRefMap은 ObjectRef<T> —
// 값 null 허용), IsSet은 OrderedSet인지, Registry는 Registry가 관리하는 컨테이너(ObjectRefMap —
// 항목을 Registry API로만 바꾼다)인지다.
template <class T>
struct MapField {
    static constexpr bool Supported = false;
};

template <class K, class V>
struct MapField<OrderedMap<K, V>> {
    static constexpr bool Supported = true;
    static constexpr bool IsSet = false;
    static constexpr bool Registry = false;
    using Key = K;
    using Value = V;
};

template <class T>
struct MapField<OrderedSet<T>> {
    static constexpr bool Supported = true;
    static constexpr bool IsSet = true;
    static constexpr bool Registry = false;
    using Key = T;
    using Value = T;
};

template <class K, class T>
struct MapField<ObjectRefMap<K, T>> {
    static constexpr bool Supported = true;
    static constexpr bool IsSet = false;
    static constexpr bool Registry = true;
    using Key = K;
    using Value = ObjectRef<T>;
};

// 리플렉션 정적 배열 필드(T x[N] — 리플렉션 B9g, Docs/Design/ReflectionExtensions.ko.md 8절)다.
// 부분 특수화로만 고르며 Element는 원소 C++ 형식(단일 값·ObjectRef·WeakRef·ClassRef·SoftObjectRef·
// enum·struct — 참조 struct 포함), Count는 N이다. 원소가 소유 객체 저장소 안에 있으므로 Registry
// 컨테이너가 아니다(ObjectRef 원소의 쓰기는 SetReference의 주소 범위 검사를 그대로 지난다).
// 다차원 배열(T x[M][N])은 Element가 다시 배열이라 FieldValueKind가 StaticArray이고 성질 표의
// 정적 배열 원소가 아니므로 생성 codec의 static_assert가 거부한다(MetaCompiler RMC2307이 먼저
// 막는다).
template <class T>
struct StaticArrayField {
    static constexpr bool Supported = false;
};

template <class E, usize N>
struct StaticArrayField<E[N]> {
    static constexpr bool Supported = true;
    static constexpr usize Count = N;
    using Element = E;
};

// 빈 자리에 새로 만드는 값이다: noexcept E(Allocator&)가 있으면 allocator를 넘기고 없으면 E()다
// (Optional의 새 내부 값·경로가 넣는 원소 — MakeRuntimeStructOps와 같은 규칙). allocator는 보통
// 객체 allocator(ObjectRegistry::ObjectAllocator)다.
//
// 사용 예:
//   Point point = detail::MakeAllocatedValue<Point>(registry.ObjectAllocator());
template <class E>
E MakeAllocatedValue(Allocator& allocator) noexcept {
    if constexpr (IsConstructibleV<E, Allocator&>) {
        static_assert(IsNothrowConstructibleV<E, Allocator&>);
        return E(allocator);
    } else {
        static_assert(IsNothrowConstructibleV<E>);
        return E();
    }
}

// struct 멤버 포인터의 소유 struct·멤버 형식이다(생성 codec·경로 thunk가 같이 쓴다 — 경로 헤더가
// Serialization에 의존하지 않도록 여기에 둔다).
template <class T>
struct StructMemberPointer;

template <class S, class M>
struct StructMemberPointer<M S::*> {
    using Owner = S;
    using Member = M;
};

// C++ 형식의 단일 값 종류다(enum·struct·컨테이너는 Opaque — 그 셋은 Target과 정적 대조로 가린다).
// requires 탐지 없이 IsSameV와 부분 특수화(ObjectReferenceField·SoftReferenceField)만 쓴다. 평범한
// char는 signed char(int8)·unsigned char(uint8)와 다른 형식이라 Opaque다(부호가 구현 정의 —
// MetaCompiler도 거부한다).
template <class T>
constexpr ValueKind ScalarValueKind() noexcept {
    if constexpr (IsSameV<T, bool>) {
        return ValueKind::Bool;
    } else if constexpr (IsSameV<T, uint8>) {
        return ValueKind::UInt8;
    } else if constexpr (IsSameV<T, uint16>) {
        return ValueKind::UInt16;
    } else if constexpr (IsSameV<T, uint32>) {
        return ValueKind::UInt32;
    } else if constexpr (IsSameV<T, uint64>) {
        return ValueKind::UInt64;
    } else if constexpr (IsSameV<T, int8>) {
        return ValueKind::Int8;
    } else if constexpr (IsSameV<T, int16>) {
        return ValueKind::Int16;
    } else if constexpr (IsSameV<T, int32>) {
        return ValueKind::Int32;
    } else if constexpr (IsSameV<T, int64>) {
        return ValueKind::Int64;
    } else if constexpr (IsSameV<T, float>) {
        return ValueKind::Float32;
    } else if constexpr (IsSameV<T, double>) {
        return ValueKind::Float64;
    } else if constexpr (IsSameV<T, Guid>) {
        return ValueKind::Guid;
    } else if constexpr (IsSameV<T, String>) {
        return ValueKind::Utf8String;
    } else if constexpr (IsSameV<T, Name>) {
        return ValueKind::Name;
    } else if constexpr (IsSameV<T, Text>) {
        return ValueKind::Text;
    } else if constexpr (ObjectReferenceField<T>::Supported) {
        return ValueKind::ObjectReference;
    } else if constexpr (WeakReferenceField<T>::Supported) {
        return ValueKind::WeakObjectReference;
    } else if constexpr (ClassReferenceField<T>::Supported) {
        return ValueKind::ClassReference;
    } else if constexpr (IsSameV<T, AssetId>) {
        return ValueKind::AssetId;
    } else if constexpr (IsSameV<T, Sha256Digest>) {
        return ValueKind::Digest256;
    } else if constexpr (SoftReferenceField<T>::Supported) {
        return ValueKind::SoftObjectReference;
    } else {
        return ValueKind::Opaque;
    }
}

// C++ 멤버 형식의 필드 값 종류다: 정적 배열 T[N]은 StaticArray, 리플렉션 배열 컨테이너는 Array,
// Optional<T>는 Optional, OrderedMap<K, V>·OrderedSet<T>는 Map·Set, InstancedStruct<Base>는
// InstancedStruct(리플렉션 B9j), 단일 값은 ScalarValueKind, 리플렉션 enum·struct는 Enum·Struct, 그
// 밖은 Opaque다(단일 값 형식 대응은 ScalarValueKind 한 곳에 있다).
template <class T>
constexpr ValueKind FieldValueKind() noexcept {
    if constexpr (StaticArrayField<T>::Supported) {
        return ValueKind::StaticArray;
    } else if constexpr (ArrayField<T>::Supported) {
        return ValueKind::Array;
    } else if constexpr (OptionalField<T>::Supported) {
        return ValueKind::Optional;
    } else if constexpr (MapField<T>::Supported) {
        return MapField<T>::IsSet ? ValueKind::Set : ValueKind::Map;
    } else if constexpr (InstancedStructField<T>::Supported) {
        return ValueKind::InstancedStruct;
    } else if constexpr (ScalarValueKind<T>() != ValueKind::Opaque) {
        return ScalarValueKind<T>();
    } else if constexpr (ReflectedEnumField<T>::Supported) {
        return ValueKind::Enum;
    } else if constexpr (ReflectedStructField<T>::Supported) {
        return ValueKind::Struct;
    } else {
        return ValueKind::Opaque;
    }
}

// C++ 멤버 형식을 FieldValue로 읽고 쓸 때의 값 종류(FieldValue::kind)다: Optional<T>는 내부 종류
// (FieldValueKind<T>), 그 밖은 FieldValueKind다(Docs/Design/ReflectionExtensions.ko.md 1.1절 —
// Optional 단일 값의 kind는 내부 종류이고 present가 set 여부다).
template <class T>
constexpr ValueKind FieldAccessKind() noexcept {
    if constexpr (OptionalField<T>::Supported) {
        return FieldValueKind<typename OptionalField<T>::Inner>();
    } else {
        return FieldValueKind<T>();
    }
}

// 대상 종류 Target과 C++ 대상 형식 T(배열이면 원소, Optional이면 내부)가 맞는지의 컴파일 시
// 대조다(경로 thunk는 MetaCompiler가 명시한 TargetKind — 틀리면 컴파일 오류, 기존 생성 접근기는
// FieldValueKind<T>()).
template <ValueKind Target, class T>
struct TargetValueCheck {
    static constexpr bool Valid =
        Target == ValueKind::Enum ? IsEnumV<T>
        : Target == ValueKind::Struct
            ? IsClassV<T> && ScalarValueKind<T>() == ValueKind::Opaque &&
                  !ArrayField<T>::Supported && !OptionalField<T>::Supported &&
                  !MapField<T>::Supported && !InstancedStructField<T>::Supported
        : Target == ValueKind::InstancedStruct
            ? InstancedStructField<T>::Supported
            : Target != ValueKind::Opaque && Target != ValueKind::Array &&
                  ScalarValueKind<T>() == Target;
    static_assert(Valid, "path thunk target kind does not match the C++ member type");
};

// 단일 값 하나를 Target 종류로 FieldValue에 복사한다(String·Soft 경로는 value의 allocator로 할당 —
// OutOfMemory). enum은 정수와 GeneratedType<E>::Id()를 싣는다. 참조(강한·weak)는 handle을, Name은
// 토큰을, ClassRef는 클래스 TypeId를 guid에 싣는다(할당 없음 — weak는 대상이 죽었어도 저장된 handle
// 그대로, ClassRef는 클래스가 해제됐어도 저장된 ID 그대로다). present는 true로 둔다.
//
// 사용 예:
//   FieldValue value(allocator);
//   auto copied = detail::CopyTargetValue<ValueKind::UInt32>(slot.index, value);
template <ValueKind Target, class T>
Result<void> CopyTargetValue(const T& source, FieldValue& value) noexcept {
    static_assert(TargetValueCheck<Target, T>::Valid);
    static_assert(Target != ValueKind::Struct, "struct values are not FieldValue");
    value.kind = Target;
    // 값을 담았으므로 present다(재사용한 FieldValue에 앞선 unset 읽기의 false가 남지 않게).
    value.present = true;
    if constexpr (Target == ValueKind::Utf8String) {
        return value.string.TryAssign(source.View());
    } else if constexpr (Target == ValueKind::Text) {
        // Text는 value의 allocator로 노드를 새로 만든다(리플렉션 B9l — 멤버 노드를 공유하지
        // 않는다).
        auto copied = source.DeepClone(value.GetAllocator());
        if (!copied) {
            return Result<void>::Failure(copied.Error());
        }

        value.text = Move(copied.Value());
    } else if constexpr (Target == ValueKind::SoftObjectReference) {
        value.guid = source.asset.value;
        return value.string.TryAssign(source.objectPath.View());
    } else if constexpr (Target == ValueKind::AssetId) {
        value.guid = source.value;
    } else if constexpr (Target == ValueKind::ClassReference) {
        value.guid = source.Id();
    } else if constexpr (Target == ValueKind::Name) {
        value.name = source;
    } else if constexpr (Target == ValueKind::Digest256) {
        value.digest = source;
    } else if constexpr (Target == ValueKind::Enum) {
        value.signedInteger = static_cast<int64>(source);
        value.type = GeneratedType<T>::Id();
    } else if constexpr (Target == ValueKind::ObjectReference ||
                         Target == ValueKind::WeakObjectReference) {
        value.reference = source.Handle();
    } else if constexpr (Target == ValueKind::Bool) {
        value.boolean = source;
    } else if constexpr (Target == ValueKind::Guid) {
        value.guid = source;
    } else if constexpr (Target == ValueKind::Float32) {
        value.float32 = source;
    } else if constexpr (Target == ValueKind::Float64) {
        value.float64 = source;
    } else if constexpr (Target == ValueKind::Int8 || Target == ValueKind::Int16 ||
                         Target == ValueKind::Int32 || Target == ValueKind::Int64) {
        value.signedInteger = source;
    } else {
        value.unsignedInteger = source;
    }

    return Result<void>::Success();
}

// FieldValue를 Target 종류의 단일 값 target에 쓴다. value.present가 거짓이면(unset — Optional만의
// 값) 종류 대조 뒤 InvalidArgument다. 정수 폭이 넘치면 Overflow(Int8·Int16·Int32는
// signedInteger, 부호 없는 정수는 unsignedInteger가 대상 폭 밖일 때), enum은 값의 enum
// ID가 GeneratedType<E>::Id()가 아니면 TypeMismatch·선언되지 않은 값이면 InvalidArgument, Soft는
// null 규칙·경로 상한 위반이 InvalidArgument다(실패하면 target은 그대로다). String·Soft는 target의
// allocator로 할당한다(OutOfMemory — 이때는 제자리 대입이므로 원자적 게시가 필요한 쓰기는 새 값에
// 쓴 뒤 Swap한다). 강한 참조는 Registry로만 쓰므로 받지 않는다(컴파일 오류). Name은 토큰을 그대로
// 대입한다 — 토큰이 대상 Registry pool의 것인지와 내용 규칙(ValidateNameFieldToken)은 호출
// 경로(FieldWriteOperation·WriteGeneratedStagedField·경로 쓰기)가 먼저 검사한다. weak 참조는
// handle을 그대로 대입한다(비정규 null은 ObjectHandle{}로 정규화) — 대상이 같은 Registry의 생존
// 객체이고 기대 타입의 파생인지는 호출 경로(FieldWriteOperation·WriteGeneratedField·
// WriteGeneratedStagedField·경로 쓰기)가 먼저 검사하고, 복원 2단계는 1단계가 검증한 노드 handle을
// 쓴다. ClassRef는 클래스 TypeId를 그대로 대입한다 — 등록·클래스·Base 파생은 호출 경로
// (FieldWriteOperation·WriteGeneratedField·WriteGeneratedStagedField·경로 쓰기 —
// ValidateClassReference)가 먼저 검사하고, 복원 2단계는 1단계가 판정한 값을 쓴다. Text(리플렉션
// B9l)는 value.text를 valueAllocator(대상 객체의 allocator — ObjectRegistry·ConstructionContext::
// ObjectAllocator, 경로 문맥의 ValueAllocator)로 DeepClone해 대입한다(valueAllocator가 없으면
// Unsupported, 복사 실패는 그 오류 — target은 그대로다).
template <ValueKind Target, class T>
Result<void> WriteTargetValue(T& target, const FieldValue& value,
                              Allocator* valueAllocator = nullptr) noexcept {
    static_assert(TargetValueCheck<Target, T>::Valid);
    static_assert(Target != ValueKind::Struct && Target != ValueKind::ObjectReference,
                  "struct values and references are not written through FieldValue");
    if (value.kind != Target) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    // unset(present = false)은 Optional 필드만의 값이다 — Optional 쓰기는 set일 때만 내부 값을 이
    // 함수로 쓴다(WriteOptionalValue).
    if (!value.present) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if constexpr (Target == ValueKind::Utf8String) {
        return target.TryAssign(value.string.View());
    } else if constexpr (Target == ValueKind::Text) {
        if (!valueAllocator) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        auto copied = value.text.DeepClone(*valueAllocator);
        if (!copied) {
            return Result<void>::Failure(copied.Error());
        }

        target = Move(copied.Value());
    } else if constexpr (Target == ValueKind::SoftObjectReference) {
        return target.TryAssign(AssetId{value.guid}, value.string.View());
    } else if constexpr (Target == ValueKind::AssetId) {
        target = AssetId{value.guid};
    } else if constexpr (Target == ValueKind::ClassReference) {
        target = T(value.guid);
    } else if constexpr (Target == ValueKind::Name) {
        target = value.name;
    } else if constexpr (Target == ValueKind::WeakObjectReference) {
        target = T(value.reference.IsNull() ? ObjectHandle{} : value.reference);
    } else if constexpr (Target == ValueKind::Digest256) {
        target = value.digest;
    } else if constexpr (Target == ValueKind::Enum) {
        if (value.type != GeneratedType<T>::Id()) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        if (!GeneratedType<T>::IsDeclared(value.signedInteger)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        target = static_cast<T>(value.signedInteger);
    } else if constexpr (Target == ValueKind::Bool) {
        target = value.boolean;
    } else if constexpr (Target == ValueKind::Guid) {
        target = value.guid;
    } else if constexpr (Target == ValueKind::Float32) {
        target = value.float32;
    } else if constexpr (Target == ValueKind::Float64) {
        target = value.float64;
    } else if constexpr (Target == ValueKind::Int8 || Target == ValueKind::Int16 ||
                         Target == ValueKind::Int32) {
        // 좁은 부호 있는 정수는 대상 폭(int8·int16·int32) 밖이면 Overflow다(값은 signedInteger).
        if (value.signedInteger < NumericLimits<T>::Min() ||
            value.signedInteger > NumericLimits<T>::Max()) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        target = static_cast<T>(value.signedInteger);
    } else if constexpr (Target == ValueKind::Int64) {
        target = value.signedInteger;
    } else {
        const uint64 maximum = static_cast<uint64>(NumericLimits<T>::Max());
        if (value.unsignedInteger > maximum) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        target = static_cast<T>(value.unsignedInteger);
    }

    return Result<void>::Success();
}

// 멤버 값을 FieldValue로 복사한다(변환 규칙은 CopyTargetValue 한 벌 — 값 종류는 FieldValueKind).
// String·Soft 경로는 value의 allocator로 새 버퍼를 할당한 뒤 원본을 읽으므로, owner 객체의 멤버를
// 복사하는 호출자는 owner를 encode 고정해 할당 콜백이 원본을 바꾸거나 해제하지 못하게
// 한다(ReadGeneratedField — 고정 중 재진입 쓰기는 Busy).
// Optional<T>(내부가 단일 값 — 리플렉션 B9e)는 kind가 내부 종류이고 present가 set 여부다(unset이면
// 값 칸은 기본값이고 enum이면 type만 그 enum ID다).
template <class Member>
Result<void> CopyFieldValue(const Member& source, FieldValue& value) noexcept {
    static_assert(FieldValueKind<Member>() != ValueKind::Opaque);
    if constexpr (OptionalField<Member>::Supported) {
        using Inner = typename OptionalField<Member>::Inner;
        constexpr ValueKind kind = FieldValueKind<Inner>();
        static_assert(kind != ValueKind::Struct, "Optional struct values are not FieldValue");
        value.kind = kind;
        value.present = source.HasValue();
        if (!source.HasValue()) {
            if constexpr (kind == ValueKind::Enum) {
                value.type = GeneratedType<Inner>::Id();
            }

            return Result<void>::Success();
        }

        return CopyTargetValue<kind>(source.Value(), value);
    } else {
        return CopyTargetValue<FieldValueKind<Member>()>(source, value);
    }
}

// FieldValue(kind = 내부 종류, present)를 Optional<T> slot에 쓴다(리플렉션 B9e — 소유 객체 재확인·
// 고정이 필요 없는 자리: 복원 2단계·관찰 임시 값·경로가 고정을 쥔 뒤의 게시). present가 거짓이면
// 옛 값을 지역으로 옮긴 뒤 비우고(게시 뒤 소멸), 참이면 내부 값을 쓴다: String은 slot이 set이면 그
// 값의 allocator, unset이면 allocator로 새 값을 만든 뒤 할당 없는 Swap·이동으로 게시하고, 그 밖의
// 단일 값은 WriteTargetValue 규칙(정수 폭 Overflow, enum ID TypeMismatch·미선언
// InvalidArgument)으로 새 값을 만든 뒤 대입한다. 실패하면 slot은 그대로다. Name 토큰·내용 검사는
// 호출 경로의 몫이다.
//
// 사용 예(복원 2단계):
//   Allocator& objects = construction.ObjectAllocator();
//   auto written = detail::AssignOptionalValue(owner->ratio, value, objects);
template <class T>
Result<void> AssignOptionalValue(Optional<T>& slot, const FieldValue& value,
                                 Allocator& allocator) noexcept {
    constexpr ValueKind kind = FieldValueKind<T>();
    static_assert(kind != ValueKind::Struct, "Optional struct values are not FieldValue");
    if (value.kind != kind) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (!value.present) {
        Optional<T> previous(Move(slot));
        slot.Reset();
        return Result<void>::Success();
    }

    if constexpr (IsSameV<T, String>) {
        String replacement(slot.HasValue() ? slot.Value().GetAllocator() : allocator);
        if (auto copied = replacement.TryAssign(value.string.View()); !copied) {
            return copied;
        }

        if (slot.HasValue()) {
            slot.Value().Swap(replacement);
        } else {
            slot.Emplace(Move(replacement));
        }

        return Result<void>::Success();
    } else {
        T candidate{};
        if (auto written = WriteTargetValue<kind>(candidate, value); !written) {
            return written;
        }

        slot.Emplace(candidate);
        return Result<void>::Success();
    }
}

// Optional<T> 멤버 쓰기의 공통 본체다(살아 있는 객체 WriteGeneratedField, staged 쓰기). 규칙은
// AssignOptionalValue와 같고, String은 새 값을 만든 뒤 get()으로 owner를 다시 확인하고 게시한다.
// pin은 살아 있는 객체 쓰기가 할당 전에 쥔 encode 고정이다(staged는 nullptr — WriteGeneratedValue와
// 같은 규칙: 게시 또는 실패 직후 풀고 옛 값·실패한 새 값은 고정 밖에서 반환한다). allocator는 unset
// 자리에 새 String을 만들 객체 allocator(ObjectRegistry·ConstructionContext::ObjectAllocator)다.
template <class Owner, class MemberOwner, class T, class Getter>
Result<void> WriteOptionalValue(Optional<T> MemberOwner::* member, const FieldValue& value,
                                Getter&& get, Allocator& allocator,
                                EncodePin* pin = nullptr) noexcept {
    static_assert(IsBaseOfV<MemberOwner, Owner>);
    constexpr ValueKind kind = FieldValueKind<T>();
    static_assert(kind != ValueKind::Struct, "Optional struct values are not FieldValue");
    auto target = get();
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    if (value.kind != kind) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if constexpr (IsSameV<T, String>) {
        Optional<T>& current = target.Value()->*member;
        if (!value.present) {
            // 옛 값은 고정을 푼 뒤(이 함수를 나갈 때) 반환한다.
            Optional<T> previous(Move(current));
            current.Reset();
            if (pin) {
                pin->Reset();
            }

            return Result<void>::Success();
        }

        String replacement(current.HasValue() ? current.Value().GetAllocator() : allocator);
        // replacement보다 뒤에 선언하므로 모든 경로에서 replacement 반환보다 먼저 고정을 푼다.
        auto release = MakeScopeExit([pin]() noexcept {
            if (pin) {
                pin->Reset();
            }
        });
        if (auto copied = replacement.TryAssign(value.string.View()); !copied) {
            return copied;
        }

        target = get();
        if (!target) {
            return Result<void>::Failure(target.Error());
        }

        // 할당·반환 없는 Swap·이동이 commit이다(옛 버퍼는 replacement가 고정 밖에서 반환한다).
        Optional<T>& live = target.Value()->*member;
        if (live.HasValue()) {
            live.Value().Swap(replacement);
        } else {
            live.Emplace(Move(replacement));
        }

        return Result<void>::Success();
    } else {
        return AssignOptionalValue(target.Value()->*member, value, allocator);
    }
}

// 단일 값(String·Soft·참조 제외) 멤버에 FieldValue를 쓴다(변환 규칙은 WriteTargetValue 한 벌).
// enum은 이 필드의 enum ID와 선언된 값(Flags는 선언 값의 OR)만 받는다 — staged 쓰기·복원도 이
// 검사를 지난다(TypeRegistry를 거치지 않는다).
template <class Member>
Result<void> WriteScalarField(Member& target, const FieldValue& value,
                              Allocator* valueAllocator = nullptr) noexcept {
    return WriteTargetValue<FieldValueKind<Member>()>(target, value, valueAllocator);
}

// Generated code가 typed member pointer를 전달한다. raw offset/레이아웃 추정은 사용하지 않는다.
// 별도 serializer/도구도 같은 context/handle 경계를 지나며 pointer/view를 반환받지 않는다.
// 복사는 owner의 encode 고정(PinForEncode) 아래에서 한다: String·Soft 경로 복사(TryAssign)는 새
// 버퍼를 할당한 뒤 원본을 읽으므로, 그 할당 콜백이 같은 필드를 쓰면(WriteGeneratedField —
// ResolveForWrite) 고정 때문에 Busy가 되어 원본 버퍼가 복사 전에 해제되지 않는다. 고정은
// RequestDestroy를 막지 않으므로 out을 게시하기 전에 owner의 논리 수명을 다시 확인하고, out의 옛
// 버퍼는 고정을 푼 뒤 반환한다.
template <class Owner, class MemberOwner, class Member>
Result<void> ReadGeneratedField(ObjectRegistry& registry, const ObjectAccessContext& access,
                                ObjectHandle handle, Member MemberOwner::* member,
                                FieldValue& output) noexcept {
    static_assert(IsBaseOfV<MemberOwner, Owner>);
    auto borrowed = registry.Resolve<Owner>(access, handle);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto target = borrowed.Value().Get();
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    auto pin = registry.PinForEncode(access, handle);
    if (!pin) {
        return Result<void>::Failure(pin.Error());
    }

    FieldValue candidate(output.GetAllocator());
    auto copied = CopyFieldValue(target.Value()->*member, candidate);
    if (!copied) {
        return copied;
    }

    // String 복사 allocator callback이 owner를 논리 파괴했다면 out을 게시하지 않는다.
    auto stillValid = borrowed.Value().Get();
    if (!stillValid) {
        return Result<void>::Failure(stillValid.Error());
    }

    pin.Value().Reset();
    output = Move(candidate);
    return Result<void>::Success();
}

// 단일 값 멤버 쓰기의 공통 본체다(살아 있는 객체 WriteGeneratedField, staged 쓰기, struct 멤버
// 적용). String·Soft는 대상 멤버의 allocator로 새 값을 만든 뒤 get()으로 owner를 다시 확인하고
// 할당·반환 없는 Swap으로 게시한다. pin은 살아 있는 객체 쓰기가 할당 전에 쥔 encode
// 고정이다(staged·struct 적용은 nullptr): 할당 콜백의 재진입 쓰기(ResolveForWrite)를 Busy로 막아
// 갱신이 덮이지 않게 하고, 게시 또는 실패 직후 풀어 옛 버퍼·실패한 새 버퍼는 고정 밖에서 반환한다.
// Text(리플렉션 B9l)는 valueAllocator(대상 객체 allocator — 없으면 Unsupported)로 DeepClone한 새
// 값을 같은 규칙으로 게시한다(옛 노드는 고정 밖에서 반환한다).
template <class Owner, class MemberOwner, class Member, class Getter>
Result<void> WriteGeneratedValue(Member MemberOwner::* member, const FieldValue& value,
                                 Getter&& get, EncodePin* pin = nullptr,
                                 Allocator* valueAllocator = nullptr) noexcept {
    static_assert(IsBaseOfV<MemberOwner, Owner>);
    static_assert(FieldValueKind<Member>() != ValueKind::Opaque);
    auto target = get();
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    if constexpr (IsSameV<Member, String>) {
        String replacement((target.Value()->*member).GetAllocator());
        // replacement보다 뒤에 선언하므로 모든 경로에서 replacement 반환보다 먼저 고정을 푼다.
        auto release = MakeScopeExit([pin]() noexcept {
            if (pin) {
                pin->Reset();
            }
        });
        auto copied = replacement.TryAssign(value.string.View());
        if (!copied) {
            return copied;
        }

        target = get();
        if (!target) {
            return Result<void>::Failure(target.Error());
        }

        // 할당/해제 없는 Swap이 commit이다. 이후 old storage 반환 callback의 RequestDestroy는
        // 이미 완료한 성공을 되돌리지 않는다. 호출 측 Borrow는 이 정리까지 물리 수명을 유지한다.
        (target.Value()->*member).Swap(replacement);
        return Result<void>::Success();
    } else if constexpr (IsSameV<Member, Text>) {
        if (!valueAllocator) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        Text replacement;
        // replacement보다 뒤에 선언하므로 모든 경로에서 replacement(옛 값) 반환보다 먼저 고정을
        // 푼다.
        auto release = MakeScopeExit([pin]() noexcept {
            if (pin) {
                pin->Reset();
            }
        });
        if (value.kind != ValueKind::Text) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        if (!value.present) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto copied = value.text.DeepClone(*valueAllocator);
        if (!copied) {
            return Result<void>::Failure(copied.Error());
        }

        replacement = Move(copied.Value());
        target = get();
        if (!target) {
            return Result<void>::Failure(target.Error());
        }

        // 노드 포인터 교환(할당·반환 없음)이 commit이다. 옛 노드는 replacement가 고정 밖에서
        // 반환한다.
        Text& live = target.Value()->*member;
        Text previous = Move(live);
        live = Move(replacement);
        replacement = Move(previous);
        return Result<void>::Success();
    } else if constexpr (SoftReferenceField<Member>::Supported) {
        // staged 쓰기·복원도 같은 null 규칙·경로 상한을 지킨다(등록된 write의 검증을 거치지
        // 않는다).
        const AssetId asset{value.guid};
        String replacement((target.Value()->*member).objectPath.GetAllocator());
        auto release = MakeScopeExit([pin]() noexcept {
            if (pin) {
                pin->Reset();
            }
        });
        if (auto valid = Member::Validate(asset, value.string.View()); !valid) {
            return valid;
        }

        auto copied = replacement.TryAssign(value.string.View());
        if (!copied) {
            return copied;
        }

        target = get();
        if (!target) {
            return Result<void>::Failure(target.Error());
        }

        // String과 같이 할당 없는 Swap이 commit이다.
        (target.Value()->*member).asset = asset;
        (target.Value()->*member).objectPath.Swap(replacement);
        return Result<void>::Success();
    } else {
        return WriteScalarField(target.Value()->*member, value);
    }
}

// 등록된 생성 write thunk다. 쓰기용 Borrow(ResolveForWrite — owner가 encode 고정 중이면 Busy)를
// 얻고, String·Soft는 새 값 할당부터 Swap까지 owner를 encode 고정해 할당 콜백의 재진입 쓰기를
// Busy로 막는다(고정은 RequestDestroy를 막지 않으므로 게시 직전 owner를 다시 확인한다). 수동
// 함수표의 write callback(FieldWriteOperation이 부른다)에는 이 고정이 없다 — 할당하는 수동
// callback은 같은 규칙을 스스로 지켜야 한다.
template <class Owner, class MemberOwner, class Member>
Result<void> WriteGeneratedField(ObjectRegistry& registry, const ObjectAccessContext& access,
                                 ObjectHandle handle, Member MemberOwner::* member,
                                 const FieldValue& value) noexcept {
    if (value.kind != FieldAccessKind<Member>()) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    // unset(present = false)은 Optional 필드만의 값이다.
    if (!value.present && !OptionalField<Member>::Supported) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 쓰기용 Borrow: owner가 encode 고정 중이면 Busy다(encode가 순회하는 String·Soft 경로 버퍼를
    // 이 쓰기가 해제하지 못하게 — ObjectRegistry::PinForEncode).
    auto borrowed = registry.ResolveForWrite<Owner>(access, handle);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    if constexpr (OptionalField<Member>::Supported) {
        // Optional<T>(리플렉션 B9e): unset 자리의 새 String은 객체 allocator로 만든다. String은
        // 할당부터 게시까지 owner를 encode 고정한다(재진입 쓰기 Busy).
        auto get = [&]() noexcept { return borrowed.Value().Get(); };
        if constexpr (IsSameV<typename OptionalField<Member>::Inner, String>) {
            auto pin = registry.PinForEncode(access, handle);
            if (!pin) {
                return Result<void>::Failure(pin.Error());
            }

            return WriteOptionalValue<Owner>(member, value, get, registry.ObjectAllocator(),
                                             &pin.Value());
        } else {
            return WriteOptionalValue<Owner>(member, value, get, registry.ObjectAllocator());
        }
    } else if constexpr (ObjectReferenceField<Member>::Supported) {
        auto target = borrowed.Value().Get();
        if (!target) {
            return Result<void>::Failure(target.Error());
        }

        return registry.SetReference(access, handle, target.Value()->*member, value.reference);
    } else if constexpr (WeakReferenceField<Member>::Supported) {
        // weak 대상은 이 Registry의 생존 객체이고 정적 대상 타입의 파생이어야 한다(null 허용 —
        // 등록된 write를 거치면 FieldWriteOperation이 스키마 기대 타입으로 먼저 검사했다).
        if (auto valid =
                ValidateWeakTarget(registry, access, value.reference,
                                   NativeTypeKey<typename WeakReferenceField<Member>::Target>());
            !valid) {
            return valid;
        }

        return WriteGeneratedValue<Owner>(member, value,
                                          [&]() noexcept { return borrowed.Value().Get(); });
    } else if constexpr (ClassReferenceField<Member>::Supported) {
        // ClassRef 값은 이 Registry에 등록된 클래스이고 정적 Base의 파생이어야 한다(null 허용 —
        // 등록된 write를 거치면 FieldWriteOperation이 스키마 Base로 먼저 검사했다). 지금 값과 같은
        // 값은 아무것도 하지 않는다(Deprecated 클래스 값의 재기록 — 리플렉션 B9h2).
        if (auto current = borrowed.Value().Get();
            current && (current.Value()->*member).Id() == value.guid) {
            return Result<void>::Success();
        }

        if (auto valid = ValidateClassReference(
                registry, access, value.guid, TypeId{},
                NativeTypeKey<typename ClassReferenceField<Member>::Target>());
            !valid) {
            return valid;
        }

        return WriteGeneratedValue<Owner>(member, value,
                                          [&]() noexcept { return borrowed.Value().Get(); });
    } else if constexpr (IsSameV<Member, String> || SoftReferenceField<Member>::Supported ||
                         IsSameV<Member, Text>) {
        // Text(리플렉션 B9l)는 이 Registry의 객체 allocator로 새 노드를 만든다.
        auto pin = registry.PinForEncode(access, handle);
        if (!pin) {
            return Result<void>::Failure(pin.Error());
        }

        return WriteGeneratedValue<Owner>(
            member, value, [&]() noexcept { return borrowed.Value().Get(); }, &pin.Value(),
            &registry.ObjectAllocator());
    } else {
        return WriteGeneratedValue<Owner>(member, value,
                                          [&]() noexcept { return borrowed.Value().Get(); });
    }
}

template <class Owner, class MemberOwner, class Member>
Result<void> WriteGeneratedStagedField(ConstructionContext& construction, ObjectHandle handle,
                                       Member MemberOwner::* member,
                                       const FieldValue& value) noexcept {
    if (value.kind != FieldAccessKind<Member>()) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    // unset(present = false)은 Optional 필드만의 값이다.
    if (!value.present && !OptionalField<Member>::Supported) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // owner는 쓰기용 GetForWrite로 얻는다 — Required 검사 중 표시 동안 Busy다(검사가 읽는 값을
    // hook이 이 staged 쓰기로 바꾸지 못하게, Docs/Design/ObjectFoundations.ko.md 4절).
    if constexpr (OptionalField<Member>::Supported) {
        // Optional<Name>의 set 쓰기는 Name 필드와 같은 토큰 검사를 owner 확인 뒤에 한다. unset
        // 자리의 새 String은 트랜잭션 Registry의 객체 allocator로 만든다(리플렉션 B9e).
        if (auto target = construction.GetForWrite<Owner>(handle); !target) {
            return Result<void>::Failure(target.Error());
        }

        if constexpr (IsSameV<typename OptionalField<Member>::Inner, Name>) {
            if (value.present) {
                if (auto valid = ValidateNameFieldToken(construction.Registry().Names(), value.name,
                                                        value.GetAllocator());
                    !valid) {
                    return valid;
                }
            }
        }

        return WriteOptionalValue<Owner>(
            member, value, [&]() noexcept { return construction.GetForWrite<Owner>(handle); },
            construction.ObjectAllocator());
    } else if constexpr (ObjectReferenceField<Member>::Supported) {
        auto target = construction.GetForWrite<Owner>(handle);
        if (!target) {
            return Result<void>::Failure(target.Error());
        }

        return construction.SetReference(handle, target.Value()->*member, value.reference);
    } else if constexpr (WeakReferenceField<Member>::Supported) {
        // weak 대상은 이 트랜잭션 Registry의 생존 객체이거나 이 트랜잭션의 객체이고 정적 대상
        // 타입의 파생이어야 한다(null 허용 — 다른 Registry의 handle은 WrongContext, 파괴 요청·
        // 회수된 대상은 ConstructionContext::AcquireType의 StaleHandle). owner 확인(접근·생존·
        // 검사 중 Busy) 뒤에 본다. 게시는 owner를 다시 얻어 할당 없이 대입한다.
        if (auto target = construction.GetForWrite<Owner>(handle); !target) {
            return Result<void>::Failure(target.Error());
        }

        if (!value.reference.IsNull()) {
            if (!construction.Registry().OwnsHandle(value.reference)) {
                return Result<void>::Failure(ErrorCode::WrongContext);
            }

            auto type = construction.AcquireType(value.reference);
            if (!type) {
                return Result<void>::Failure(type.Error());
            }

            if (!type.Value().IsNativeA(
                    NativeTypeKey<typename WeakReferenceField<Member>::Target>())) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }
        }

        return WriteGeneratedValue<Owner>(
            member, value, [&]() noexcept { return construction.GetForWrite<Owner>(handle); });
    } else if constexpr (ClassReferenceField<Member>::Supported) {
        // ClassRef 값은 그 트랜잭션 Registry에 등록된 클래스이고 정적 Base의 파생이어야 한다(null
        // 허용). owner 확인(접근·생존·검사 중 Busy) 뒤에 본다. 복원 2단계는 이 경로를 거치지 않는다
        // (DecodeGeneratedField가 1단계 판정 값을 바로 대입한다).
        auto target = construction.GetForWrite<Owner>(handle);
        if (!target) {
            return Result<void>::Failure(target.Error());
        }

        // 지금 값과 같은 값은 아무것도 하지 않는다(Deprecated 클래스 값의 재기록 — 리플렉션 B9h2).
        if ((target.Value()->*member).Id() == value.guid) {
            return Result<void>::Success();
        }

        if (auto valid = ValidateClassReference(
                construction.Registry(), construction.Access(), value.guid, TypeId{},
                NativeTypeKey<typename ClassReferenceField<Member>::Target>());
            !valid) {
            return valid;
        }

        return WriteGeneratedValue<Owner>(
            member, value, [&]() noexcept { return construction.GetForWrite<Owner>(handle); });
    } else if constexpr (IsSameV<Member, Name>) {
        // Name 토큰은 그 트랜잭션 Registry의 pool 것이어야 한다(FieldWriteOperation과 같은 검사 —
        // pool 없음 Unsupported, 다른 pool WrongContext, 내용 InvalidArgument). 소프트 참조처럼
        // owner 확인(접근·생존·검사 중 Busy) 뒤에 본다 — 잘못된 문맥에서 pool을 만지지 않는다.
        // 게시는 owner를 다시 얻어 할당 없이 대입한다.
        if (auto target = construction.GetForWrite<Owner>(handle); !target) {
            return Result<void>::Failure(target.Error());
        }

        if (auto valid = ValidateNameFieldToken(construction.Registry().Names(), value.name,
                                                value.GetAllocator());
            !valid) {
            return valid;
        }

        return WriteGeneratedValue<Owner>(
            member, value, [&]() noexcept { return construction.GetForWrite<Owner>(handle); });
    } else {
        // Text(리플렉션 B9l)는 트랜잭션 Registry의 객체 allocator로 새 노드를 만든다.
        return WriteGeneratedValue<Owner>(
            member, value, [&]() noexcept { return construction.GetForWrite<Owner>(handle); },
            nullptr, &construction.ObjectAllocator());
    }
}

} // namespace rocks::detail
