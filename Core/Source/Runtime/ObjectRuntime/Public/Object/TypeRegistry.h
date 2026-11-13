#pragma once

#include <Core/Memory/Allocator.h>
#include <Meta/TypeSchema.h>
#include <Object/RuntimeTypeOps.h>
#include <Object/TypeLease.h>
#include <Platform/Sync.h>

namespace rocks {

class TypeRegistry;

struct TypeRegistrationToken {
    uint64 registry = 0;
    uint32 generation = 0;
    TypeId type = {};
};

struct TypeStats {
    uint32 registeredTypes = 0;
    uint32 leases = 0;
};

// 등록·해제·Shutdown은 생성 스레드 전용이다. Acquire/Stats/lease 계수는 잠금으로 보호한다.
// Allocator는 Registry보다 오래 살아야 하며 소멸 전 모든 lease를 반환해야 한다.
// 콜백 코드는 입력 계약을 신뢰한다. 임의 플러그인의 메모리 안전성을 검증하지 않는다.
class TypeRegistry {
public:
    explicit TypeRegistry(Allocator& allocator) noexcept;
    ~TypeRegistry();
    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;

    // 입력 버퍼는 호출 동안 유효해야 한다. 실패하면 기존 등록 상태를 유지한다.
    // ops는 값으로 소유권을 넘기므로 등록 실패 시에도 그 ModuleLease는 정리된다.
    // 주요 실패: 알 수 없는 플래그 비트·참조(ObjectReference·SoftObjectReference)가 아닌 필드의
    // Required·참조·Enum이 아닌 값 종류의 value.type·value.type이 없는 Enum 필드·아직 받지 않는 값
    // 종류(Struct·Array, 범위 밖)·비어 있지 않은 element·maxElements(배열은 B5)·FieldKind와 값
    // 종류의 불일치(소프트 참조·Enum은 FieldKind::Value)·Enum 필드에 없거나 그 밖의 필드에 있는
    // valueNativeType·ASCII 식별자가 아닌 도구 이름·WorldOnly base의 파생에 WorldOnly 없음은
    // InvalidArgument, 같은 ID(다른 종류 포함)·같은 필드 ID·대소문자 무시로 같은 실효 도구
    // 이름(toolName 또는 name)은 AlreadyExists, base나 Enum 필드의 enum 미등록은 NotFound, base
    // 필드와 다른 상속 필드 서술(값 형식·도구 이름 포함)·enum이 아닌 레코드를 가리키는 Enum 필드·
    // native 키가 다른 enum은 TypeMismatch다. Enum 필드가 가리키는 enum은 이 타입이 등록되어 있는
    // 동안 의존으로 붙잡힌다(그 enum의 Unregister는 Busy).
    Result<TypeRegistrationToken> Register(const TypeSchema&, RuntimeTypeOps) noexcept;

    // 리플렉션 enum을 등록한다(생성 코드는 GeneratedType<E>::Register). 입력 버퍼는 호출 동안
    // 유효해야 하며 이름·열거자 바이트를 깊게 복사한다. 실패하면 기존 등록 상태를 유지하고 ops의
    // ModuleLease는 정리된다. 주요 실패: id 0·빈 이름·UTF-8이 아닌 이름·지원하지 않는
    // underlying(UInt8·UInt16·UInt32·Int32만)·알 수 없는 플래그·열거자 없음 또는 4096개 초과·ASCII
    // 식별자가 아닌 열거자 이름·[A-Za-z0-9_]+가 아닌 도구 이름이나 접두·UTF-8이 아닌 표시 이름·
    // underlying 범위 밖 값·Flags enum의 음수 값·nativeType null은 InvalidArgument, 같은 ID(다른
    // 종류 포함)·같은 nativeType의 enum(다른 ID의 별칭)·같은 값(별칭)·접두 유무를 합쳐 대소문자
    // 무시로 같은 도구 이름은 AlreadyExists다.
    //
    // 사용 예:
    //   auto token = GeneratedType<TextureGroup>::Register(types);   // enum 먼저
    //   auto owner = GeneratedType<TextureSettings>::Register(types); // 그 enum을 쓰는 클래스
    Result<TypeRegistrationToken> RegisterEnum(const EnumSchema&, RuntimeEnumOps) noexcept;

    // 리플렉션 struct를 등록한다(Docs/Design/ReflectionFoundations.ko.md 4.6절, 생성 코드는
    // GeneratedType<S>::Register). 스키마는 클래스와 같은 TypeSchema(base 없음, 클래스 플래그
    // 없음)이고 입력 버퍼는 호출 동안 유효해야 하며 깊게 복사한다. 멤버 struct·enum이 먼저 등록되어
    // 있어야 하고 이 struct가 등록된 동안 의존으로 붙잡힌다. 주요 실패:
    // - InvalidArgument: id 0, version 0, base, 클래스 플래그, 필드 서술 오류(Register와 같은
    // 규칙),
    //   Opaque 필드, 스키마와 1:1이 아닌 함수표, Transient가 아닌 필드의 codec 없음, encode·decode
    //   한쪽만, size·alignment·nativeType·construct·destroy 없음, trace와 강한 참조 멤버의 유무
    //   불일치, 깊이 MaxStructDepth 초과.
    // - AlreadyExists: 같은 ID(다른 종류 포함), 같은 nativeType의 struct, 같은 필드 ID, 대소문자
    // 무시로
    //   같은 실효 도구 이름.
    // - NotFound: 멤버 struct·enum 미등록. TypeMismatch: 종류·native 키가 다른 멤버 대상.
    //
    // 사용 예:
    //   auto inner = GeneratedType<BulkDataRef>::Register(types);   // 멤버 struct 먼저
    //   auto owner = GeneratedType<TextureSource>::Register(types);  // 그것을 쓰는 타입
    Result<TypeRegistrationToken> RegisterStruct(const TypeSchema&, RuntimeStructOps) noexcept;

    // 클래스(TypeKind::Class) 레코드만 연다. Enum·Struct 레코드의 ID면 TypeMismatch, 없으면
    // NotFound다.
    Result<TypeLease> Acquire(TypeId) noexcept;

    // Enum·Struct 레코드를 연다. kind가 Class면 InvalidArgument(Acquire를 쓴다), 같은 ID의 다른
    // 종류 레코드면 TypeMismatch, 없으면 NotFound다. lease가 있는 동안 그 레코드의 Unregister는
    // Busy다.
    //
    // 사용 예:
    //   auto lease = types.AcquireValueType(TypeIdOf<TextureGroup>(), TypeKind::Enum);
    //   if (lease) {
    //       const EnumSchema& schema = lease.Value().Enum();
    //       const EnumeratorSchema* value = FindEnumerator(schema, toolName);   // 도구 이름 조회
    //   }
    Result<TypeLease> AcquireValueType(TypeId, TypeKind kind) noexcept;
    Result<bool> IsA(TypeId actual, TypeId expected) noexcept;
    Result<void> Unregister(TypeRegistrationToken) noexcept;
    // 신규 Acquire/Clone을 닫는다. lease가 남으면 Busy이며, 반환 후 다시 호출한다.
    // Busy를 무시한 Registry 파괴는 강제 해제 대신 즉시 종료한다.
    Result<void> Shutdown() noexcept;
    TypeStats Stats() const noexcept;

private:
    friend class TypeLease;
    friend class StructView;
    struct Record;
    Result<TypeLease> Clone(void*) noexcept;
    void Release(void*) noexcept;
    void DestroyRecord(Record*) noexcept;
    // gate_를 쥔 채 부른다. fields의 Enum·Struct 필드가 가리키는 레코드를 찾아 종류·native
    // 키(thunkType)·의존 계수 여유를 확인하고, targets가 있으면 필드별 대상(그 밖은 nullptr)을
    // 쓴다. ownerWorldOnly가 거짓이면 WorldOnly 소유자를 요구하는 struct 필드를 거부한다.
    Result<void> ResolveValueTypes(Span<const FieldSchema> fields,
                                   const void* (*thunkType)(const void* context, Guid fieldId),
                                   const void* context, bool ownerWorldOnly,
                                   Record** targets) noexcept;

    Allocator& allocator_;
    mutable platform::Mutex gate_;
    Record* records_ = nullptr;
    uint64 domain_ = 0;
    uint64 ownerThread_ = 0;
    uint32 nextGeneration_ = 1;
    bool closed_ = false;
    bool mutating_ = false;
};

} // namespace rocks
