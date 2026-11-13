#pragma once

#include <Core/Encoding/Sha256.h>
#include <Core/Lifetime/ModuleLease.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <Meta/TypeSchema.h>
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

// 필드 codec thunk다(Docs/Design/ReflectionFoundations.ko.md 6.4절, 문맥은 Object/FieldCodec.h).
// encode는 필드 값을 payload에 쓰고 wire tag를 돌려준다. decode는 construction이 nullptr이면 1단계
// (검증·참조 기록·진단, 값을 게시하지 않음), 아니면 2단계(staged 객체에 게시)다. self는 이 thunk를
// 가진 함수표다(같은 표의 read·writeStaged를 쓰는 범용 codec용).
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
    uint64 unsignedInteger = 0;
    int64 signedInteger = 0;
    float float32 = 0;
    double float64 = 0;
    // Guid·AssetId 값, SoftObjectReference의 asset이다.
    Guid guid{};
    ObjectHandle reference{};
    // Utf8String 값, SoftObjectReference의 객체 경로다.
    String string;
    // Digest256 값이다.
    Sha256Digest digest{};
    // Enum 값의 enum 스키마 ID다(B3b).
    TypeId type{};

private:
    Allocator* allocator_;
};

class TypeRegistry;
class TypeLease;

namespace detail {

// 필드 쓰기의 바인딩(등록된 원본 스키마 쪽)이다: 값 형식, 읽기 전용 정책.
struct FieldWriteBinding {
    ValueType value{};
    bool readOnly = false;
};

// 필드 쓰기의 실제 대상이다. actual은 대상 객체 스키마의 그 필드, effectiveFlags는 전파된 유효
// 플래그(최상위 필드면 필드 플래그 — B6 경로 쓰기는 바깥 ReadOnly를 전파한다), actualEnum은 그
// 필드의 등록된 enum 스키마, sourceExpectedNative·targetExpectedNative는 원본·대상 스키마의 참조
// 기대 타입 native 키(0이면 Object), referenced는 non-null 참조 값의 대상 타입 lease, allowNull은
// null 참조 허용(ObjectRefArray 원소는 false — B5)이다. lease는 호출자가 얻어 쓰기 완료까지 쥔다.
struct FieldWriteTarget {
    const FieldSchema* actual = nullptr;
    uint32 effectiveFlags = 0;
    const EnumSchema* actualEnum = nullptr;
    const void* sourceExpectedNative = nullptr;
    const void* targetExpectedNative = nullptr;
    const TypeLease* referenced = nullptr;
    bool allowNull = true;
};

// 필드 쓰기 검증의 한 곳이다(Docs/Design/ReflectionFoundations.ko.md 3.6·6.4절).
// FieldWriteOperation과 경로 쓰기(B6)가 같이 쓴다. lease를 얻지 않는 순수 검사이며 오류 순서는 값
// 종류·enum ID·참조 대상 타입(TypeMismatch) → null 금지(InvalidArgument) → 읽기 전용(Unsupported) →
// 값 내용(소프트 참조 규칙·선언되지 않은 enum 값 InvalidArgument; 정수 폭 Overflow는 thunk가
// 검사한다)이다. 그래프 복원의 staged 쓰기는 이 검사를 거치지 않는다(읽기 전용은 복원을 막지
// 않는다).
Result<void> ValidateFieldWrite(const FieldWriteBinding& binding, const FieldWriteTarget& target,
                                const FieldValue& value) noexcept;

} // namespace detail

// 등록된 필드의 실제 대상 schema, 타입과 값 형식을 검사한다. 오류 순서는 context·수명, 타입·필드,
// 전달 값 종류(TypeMismatch — enum은 FieldValue::type이 필드의 enum ID와 다를 때도), 읽기
// 전용(Unsupported), 값 내용(정수 폭 Overflow·소프트 참조의 null 규칙과 경로 상한·선언되지 않은
// enum 값 InvalidArgument)이다. callback 코드 수명은 TypeLease가 보유하는 ModuleLease에 종속되므로
// 이 값을 lease보다 오래 보관하거나 호출하지 않는다.
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

    void Bind(TypeRegistry& registry, const void* nativeType, Guid fieldId, ValueKind valueKind,
              TypeId referencedType, bool readOnly) noexcept {
        registry_ = &registry;
        nativeType_ = nativeType;
        fieldId_ = fieldId;
        referencedType_ = referencedType;
        valueKind_ = valueKind;
        readOnly_ = readOnly;
        bound_ = true;
    }

    Callback callback_ = nullptr;
    TypeRegistry* registry_ = nullptr;
    TypeId referencedType_{};
    const void* nativeType_ = nullptr;
    Guid fieldId_{};
    ValueKind valueKind_ = ValueKind::Opaque;
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
    // Enum 필드가 쓰는 C++ enum의 NativeTypeKey<E>()다(그 밖의 필드는 nullptr). 등록이 필드의
    // value.type으로 찾은 enum 레코드의 native 키와 같은지 확인한다 — 스키마로 해석한 enum과
    // 정적 thunk의 enum이 어긋나지 않게 한다(어긋나면 TypeMismatch).
    const void* valueNativeType = nullptr;
};

// 리플렉션 enum 등록의 실행 정보다(TypeRegistry::RegisterEnum). nativeType은 NativeTypeKey<E>()이며
// Enum 필드의 valueNativeType과 대조한다. 동적 모듈이 등록하면 module로 그 키(정적 객체 주소)의
// 수명을 유지한다.
struct RuntimeEnumOps {
    const void* nativeType = nullptr;
    ModuleLease module;
};

// Trace 호출 동안만 유효한 방문자다. 보관하거나 다른 스레드에 전달하지 않는다.
// 객체가 가진 모든 강한 ObjectRef를 빠짐없이 Visit해야 한다. WeakRef는 방문하지 않는다.
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
    // 보증해야 한다(RegisterNativeClass·메타 생성 코드는 static_assert(__is_base_of)를 거친다).
    // 수동 함수표에서 임의로 설정하지 않는다. TypeRegistry는 기반 레코드 일치·크기·정렬 하한만
    // 검사한다.
    const void* nativeBaseType = nullptr;
    // 기본 수동 Trace는 owner 전용이다. side effect 없는 generated Trace만 명시 opt-in한다.
    bool parallelTraceSafe = false;
    // 모든 영속 필드와 모든 강한 간선이 typed metadata에 포함된 경우만 true다. ROCKS_NATIVE 멤버는
    // 영속 필드가 아니고 강한 참조를 담을 수 없으므로(MetaCompiler RMC2205·생성 static_assert) 생성
    // 타입은 계속 true다. graph serializer는 이 선언 없는 수동/부분 metadata를 완전한 객체로
    // 저장하지 않는다.
    bool completeFieldLayout = false;
};

// noexcept T(Allocator&)가 있으면 registry allocator를 주입하고, 없으면 noexcept T()를 사용한다.
// Abstract 메타데이터는 생성 callback 없이 등록하고 생성 시도는 Unsupported로 거부한다.
// 실패 가능한 초기화는 생성자 밖의 ConstructionContext 단계에서 처리한다.
// Trace가 없는 타입은 강한 간선이 없는 것으로 취급한다. 메타데이터 자동 추적은 없다.
template <class T, bool Abstract = __is_abstract(T)>
RuntimeTypeOps MakeRuntimeTypeOps(ModuleLease module = {}) noexcept {
    static_assert(__is_base_of(Object, T));
    static_assert(
        Abstract || __is_nothrow_constructible(T, Allocator&) || __is_nothrow_constructible(T),
        "Object types need `explicit T(Allocator&) noexcept` or `T() noexcept`; a derived "
        "class whose base takes Allocator& must forward it (e.g. `using Super::Super;`)");
    static_assert(__is_nothrow_destructible(T), "Object types need a noexcept destructor");

    RuntimeTypeOps ops;
    ops.size = sizeof(T);
    ops.alignment = alignof(T);
    ops.nativeType = NativeTypeKey<T>();
    if constexpr (!Abstract) {
        ops.construct = [](void* memory, Allocator& allocator) noexcept -> Object* {
            if constexpr (__is_constructible(T, Allocator&)) {
                static_assert(__is_nothrow_constructible(T, Allocator&));
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

} // namespace rocks
