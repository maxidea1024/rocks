#pragma once

#include <Core/Containers/HashMap.h>
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
class ROCKS_OBJECTRUNTIME_API TypeRegistry {
public:
    explicit TypeRegistry(Allocator& allocator) noexcept;
    ~TypeRegistry();
    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;

    // 입력 버퍼는 호출 동안 유효해야 한다. 실패하면 기존 등록 상태를 유지한다.
    // ops는 값으로 소유권을 넘기므로 등록 실패 시에도 그 ModuleLease는 정리된다.
    // 주요 실패: 클래스 플래그(Abstract·WorldOnly)가 아닌 비트(struct 전용 Atomic 포함)·참조
    // (ObjectReference·SoftObjectReference — 배열이면 원소)가 아닌 필드의 Required·참조·Enum·
    // Struct가 아닌 대상 형식의 type·type이 없는 Enum·Struct 필드·범위 밖 값 종류·배열이 아닌
    // 필드의 비어 있지 않은 element·maxElements·FieldKind와 대상 형식의
    // 불일치(소프트 참조·Enum·Struct는 FieldKind::Value)·Enum·Struct 대상에 없거나 그 밖의 필드에
    // 있는 valueNativeType·ASCII 식별자가 아닌 도구 이름·WorldOnly base의 파생에 WorldOnly 없음·
    // WorldOnly 소유자를 요구하는 struct(원소 포함)를 WorldOnly가 아닌 클래스에 둠·Struct·Array
    // 필드의 read·write·writeStaged·Transient가 아닌 Struct·Array 필드의 codec 없음은
    // InvalidArgument다. Array 필드(B5, Docs/Design/ReflectionFoundations.ko.md 5.6절)는 value =
    // {Array, 0}, element는 성질 표의 Array 원소 종류(스칼라·Int8·Int16·Guid·Utf8String·Name·
    // ObjectReference·WeakObjectReference·ClassReference·Enum·Struct·AssetId·SoftObjectReference·
    // Digest256 — Opaque·Array 거부, ClassReference는 element.type 필수), maxElements ∈ [1,
    // MaxArrayElements]이고, 참조 struct 원소(StructRefArray)는 Registry 컨테이너를 담지 않아야
    // 한다(위반은 InvalidArgument). 같은 ID(다른 종류 포함)·같은 필드 ID·대소문자 무시로 같은 실효
    // 도구 이름(toolName 또는 name)은 AlreadyExists, base나 대상 enum·struct 미등록은 NotFound,
    // base 필드와 다른 상속 필드 서술(값·원소 형식·maxElements·도구 이름 포함)·종류가 다른 레코드를
    // 가리키는 대상·native 키가 다른 대상은 TypeMismatch다. 대상 enum·struct(원소 포함)는 이
    // 타입이 등록되어 있는 동안 의존으로 붙잡힌다(그 레코드의 Unregister는 Busy). 타입 도구
    // 이름(TypeSchema::toolName — 리플렉션 B9d2)은 비어 있거나 IsTypeToolName을 만족해야 하고
    // (InvalidArgument), 비어 있지 않으면 등록된 클래스·struct의 도구 이름과 대소문자 무시로 달라야
    // 한다(AlreadyExists). ClassReference 필드(배열이면 원소)의 Base 클래스 ID는 의존으로 붙잡지
    // 않는다 — 강한 참조의 기대 타입처럼 쓰기·저장·복원 때 조회하므로 자기 자신·아직 등록되지 않은
    // 파생 클래스를 Base로 둘 수 있다(Docs/Design/ReflectionExtensions.ko.md 5.2절).
    Result<TypeRegistrationToken> Register(const TypeSchema&, RuntimeTypeOps) noexcept;

    // 리플렉션 enum을 등록한다(생성 코드는 GeneratedType<E>::Register). 입력 버퍼는 호출 동안
    // 유효해야 하며 이름·열거자 바이트를 깊게 복사한다. 실패하면 기존 등록 상태를 유지하고 ops의
    // ModuleLease는 정리된다. 주요 실패: id 0·빈 이름·UTF-8이 아닌 이름·지원하지 않는
    // underlying(정수 8종 UInt8·UInt16·UInt32·UInt64·Int8·Int16·Int32·Int64만 —
    // EnumUnderlyingRange)·알 수 없는 플래그·열거자 없음 또는 4096개 초과·ASCII 식별자가 아닌
    // 열거자 이름·[A-Za-z0-9_]+가 아닌 도구 이름이나 접두·UTF-8이 아닌 표시 이름·underlying 범위 밖
    // 값(UInt64는 int64 최댓값까지)·Flags enum의 음수 값·nativeType null은 InvalidArgument, 같은
    // ID(다른 종류 포함)·같은 nativeType의 enum(다른 ID의 별칭)·같은 값(별칭)·접두 유무를 합쳐
    // 대소문자 무시로 같은 도구 이름은 AlreadyExists다.
    //
    // 사용 예:
    //   auto token = GeneratedType<TextureGroup>::Register(types);   // enum 먼저
    //   auto owner = GeneratedType<TextureSettings>::Register(types); // 그 enum을 쓰는 클래스
    Result<TypeRegistrationToken> RegisterEnum(const EnumSchema&, RuntimeEnumOps) noexcept;

    // 리플렉션 struct를 등록한다(Docs/Design/ReflectionFoundations.ko.md 4.6절, 생성 코드는
    // GeneratedType<S>::Register). 스키마는 클래스와 같은 TypeSchema이고 flags는 struct 플래그
    // TypeFlags::Atomic만 받는다(ROCKS_STRUCT(Atomic) — 뜻은 Meta/TypeSchema.h TypeFlags, 조회는
    // StructView::IsAtomic). baseType은 리플렉션 base struct다(리플렉션 B9k — 없으면 0): 먼저
    // 등록되어 있어야 하고, fields가 base의 모든 필드를 같은 안정 ID·표현으로 펼쳐 담아야 하며,
    // base가 Atomic이면 파생도 Atomic이어야 한다(TypeLease::IsA가 이 사슬을 본다). 입력 버퍼는 호출
    // 동안 유효해야 하며 깊게 복사한다. 멤버 struct·enum·base struct·InstancedStruct<Base>의 Base가
    // 먼저 등록되어 있어야 하고 이 struct가 등록된 동안 의존으로 붙잡힌다(그들의 Unregister는
    // Busy). 주요 실패:
    // - InvalidArgument: id 0, version 0, 자기 자신이 base, Atomic base 아래의 Atomic이 아닌 파생,
    //   담을 수 없는 InstancedStruct Base, Atomic이 아닌 플래그 비트(클래스 플래그 Abstract·
    //   WorldOnly 포함), 형식에 맞지 않는 타입 도구 이름(IsTypeToolName), 필드 서술 오류(Register와
    //   같은 규칙 — 배열 멤버 포함), Opaque 필드, 스키마와 1:1이 아닌 함수표, Transient가 아닌
    //   필드의 codec 없음, encode·decode 한쪽만, size·alignment·nativeType·construct·destroy 없음,
    //   trace와 강한 참조 멤버 (ObjectRefArray·StructRefArray 포함)의 유무 불일치, 깊이
    //   MaxStructDepth 초과(struct 원소 배열 멤버는 원소 깊이 + 1), Registry 컨테이너를 담는 참조
    //   struct 원소.
    // - AlreadyExists: 같은 ID(다른 종류 포함), 같은 nativeType의 struct, 같은 필드 ID, 대소문자
    //   무시로 같은 실효 도구 이름, 대소문자 무시로 같은 타입 도구 이름(Register와 같은 규칙 —
    //   클래스와 struct가 한 이름 공간이다).
    // - NotFound: 멤버 struct·enum·base struct·InstancedStruct Base 미등록. TypeMismatch: 종류·
    //   native 키가 다른 멤버 대상, struct가 아닌 base, base 필드를 같은 ID·표현으로 담지 않은
    //   파생.
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

    // 리플렉션 struct 레코드를 native 키(NativeTypeKey<S>() — StructRefArray 원소 표의
    // StructElementOps::nativeType과 같은 주소)로 연다. 등록 규칙이 struct native 키를 유일하게
    // 하므로(같은 nativeType의 두 번째 struct는 AlreadyExists) 결과는 하나다. 관찰자 Registry가
    // StructRefArray 원소를 떼어 낼 때 그 원소만 trace하려고 RuntimeStructOps::trace를 얻는 데
    // 쓴다(Docs/Design/ObjectFoundations.ko.md 3절). lease가 있는 동안 그 레코드의 Unregister는
    // Busy다.
    //
    // 오류: nativeType이 null이면 InvalidArgument, 종료 뒤 ShuttingDown, 그 키의 struct가
    // 없으면(클래스· enum의 키 포함) NotFound, lease 계수 포화는 Overflow다.
    //
    // 사용 예:
    //   auto lease = types.AcquireStructByNative(storage.element->nativeType);
    //   if (lease && lease.Value().StructOps().trace != nullptr) {
    //       lease.Value().StructOps().trace(block, visitor);
    //   }
    Result<TypeLease> AcquireStructByNative(const void* nativeType) noexcept;

    // 타입 도구 이름(TypeSchema::toolName — 리플렉션 B9d2, Docs/Design/ReflectionExtensions.ko.md
    // 5.2절)으로 클래스·struct 레코드를 연다. ASCII 대소문자를 무시한다(등록이 그 규칙으로 유일하게
    // 한다). 에셋 명령의 `<Type>`·ClassRef 텍스트·InstancedStruct `_structType`·Sparks가 이 색인을
    // 쓴다. 조회는 잠금 안의 해시 색인이라 어느 스레드에서나 부를 수 있다. lease의 종류는
    // TypeLease::Kind로 본다. lease가 있는 동안 그 레코드의 Unregister는 Busy다.
    //
    // 오류: 빈 이름 InvalidArgument, 종료 뒤 ShuttingDown, 그 이름의 타입이 없으면(도구 이름 없이
    // 등록한 타입 포함) NotFound, lease 계수 포화 Overflow다.
    //
    // 사용 예:
    //   auto lease = types.AcquireByToolName(Span<const char>("Texture2D", 9));
    //   if (lease && lease.Value().Kind() == TypeKind::Class) { ... }
    Result<TypeLease> AcquireByToolName(Span<const char> toolName) noexcept;

    // 파생 클래스 조회다(ClassRef 값 판정 — 5.2절): type이 등록된 클래스이고 base 자신이거나 그
    // 파생(스키마 사슬 TypeLease::IsA)이면 그 클래스의 lease를 준다. 추상 클래스도 받는다. 판정
    // 규칙은 detail::CheckClassReferenceLease(Object/ClassRef.h)와 같다.
    //
    // 오류: base 0 InvalidArgument, 종료 뒤 ShuttingDown, type 미등록 NotFound, type이 enum·struct
    // 레코드이거나 base의 파생이 아님 InvalidArgument, lease 계수 포화 Overflow다.
    //
    // 사용 예:
    //   auto lease = types.AcquireDerivedClass(stored, TypeIdOf<Pawn>());
    //   if (!lease) { return Result<void>::Failure(lease.Error()); }
    Result<TypeLease> AcquireDerivedClass(TypeId type, TypeId base) noexcept;

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
    struct IndexGrowth;

    // Enum·Struct 레코드의 native 키 색인 키다(종류마다 native 키가 유일하다 — 클래스는 같은 native
    // 타입의 별칭 ID를 허용하므로 넣지 않는다).
    struct NativeIndexKey {
        const void* nativeType = nullptr;
        TypeKind kind = TypeKind::Struct;

        bool operator==(const NativeIndexKey&) const noexcept = default;
    };

    struct NativeIndexKeyHash {
        uint64 operator()(const NativeIndexKey& key) const noexcept {
            return HashCombine(MixHash(reinterpret_cast<uptr>(key.nativeType)),
                               static_cast<uint64>(key.kind));
        }
    };

    // 타입 도구 이름 색인의 해시·같음이다(ASCII 대소문자 무시 — 키는 레코드가 소유한 이름의
    // view다).
    struct ToolNameHash {
        uint64 operator()(StringView name) const noexcept;
    };

    struct ToolNameEqual {
        bool operator()(StringView left, StringView right) const noexcept;
    };

    using IdIndex = HashMap<TypeId, Record*>;
    using NativeIndex = HashMap<NativeIndexKey, Record*, NativeIndexKeyHash>;
    using ToolNameIndex = HashMap<StringView, Record*, ToolNameHash, ToolNameEqual>;

    // 잠금 보유: 레코드 하나에 lease를 더해 돌려준다(계수 포화는 Overflow).
    Result<TypeLease> LeaseLocked(Record*) noexcept;
    // 잠금 보유: ID·native 키 색인 조회다(없으면 nullptr).
    Record* FindLocked(TypeId) const noexcept;
    Record* FindNativeLocked(const void* nativeType, TypeKind kind) const noexcept;
    // 잠금 보유: 타입 도구 이름 색인 조회다(빈 이름·없으면 nullptr).
    Record* FindToolNameLocked(Span<const char> toolName) const noexcept;
    // mutating_ 아래 잠금 밖: 레코드 하나를 색인에 무할당으로 넣을 수 있게 준비한다(공간이
    // 모자라거나 지운 칸이 있는 색인은 새 색인을 할당해 둔다). 할당 실패는 그 오류이고 색인은
    // 그대로다. native가 참이면 native 키 색인도, toolName이 참이면 도구 이름 색인도 준비한다.
    Result<void> PrepareIndexes(IndexGrowth&, bool native, bool toolName = false) noexcept;
    // 잠금 보유: record를 목록 머리와 색인에 게시한다(무할당 — 새 색인이 있으면 내용을 옮겨 바꾸고
    // 옛 색인은 growth로 넘겨 잠금 밖에서 반환한다). 도구 이름이 있는 레코드는 도구 이름 색인에도
    // 넣는다(PrepareIndexes의 toolName으로 준비해 둔다).
    void PublishLocked(Record* record, IndexGrowth&) noexcept;
    Result<TypeLease> Clone(void*) noexcept;
    void Release(void*) noexcept;
    void DestroyRecord(Record*) noexcept;
    // gate_를 쥔 채 부른다. fields의 대상 형식(FieldTargetType — 배열이면 원소)이 Enum·Struct인
    // 필드가 가리키는 레코드를 찾아 종류·native 키(thunkType)·의존 계수 여유를 확인하고, targets가
    // 있으면 필드별 대상(그 밖은 nullptr)을 쓴다. ownerWorldOnly가 거짓이면 WorldOnly 소유자를
    // 요구하는 struct(원소 포함)를 거부하고, 참조 struct 원소 배열(StructRefArray)의 원소가
    // Registry 컨테이너를 담으면 거부한다(InvalidArgument). Optional 내부·Map 값·Set 원소의
    // struct는 값 struct여야 한다(InvalidArgument). Map 키가 Enum이면 그 레코드도 같은
    // 규칙(종류·native 키 — thunkType(..., key = true))으로 찾아 keyTargets(있으면)에 쓴다(리플렉션
    // B9f2).
    Result<void> ResolveValueTypes(Span<const FieldSchema> fields,
                                   const void* (*thunkType)(const void* context, Guid fieldId,
                                                            bool key),
                                   const void* context, bool ownerWorldOnly, Record** targets,
                                   Record** keyTargets) noexcept;

    Allocator& allocator_;
    mutable platform::Mutex gate_;
    // 등록 레코드의 이중 연결 목록이다(머리가 가장 최근 등록 — Shutdown이 이 순서로 의존자부터
    // 파괴한다).
    Record* records_ = nullptr;
    // TypeId → 레코드(모든 종류), (native 키, 종류) → 레코드(Enum·Struct), 타입 도구 이름 →
    // 레코드(도구 이름이 있는 클래스·struct) 색인이다. 조회는 잠금 안이고, 할당은 mutating_ 아래
    // 잠금 밖에서 새 색인으로 한다(잠금 안 삽입·삭제는 무할당 — 할당기 콜백을 잠금 안에서 부르지
    // 않는다). 삭제가 남긴 칸이 있으면 다음 등록이 새 색인으로 정리한다.
    IdIndex byId_;
    NativeIndex byNative_;
    ToolNameIndex byToolName_;
    bool idErased_ = false;
    bool nativeErased_ = false;
    bool toolNameErased_ = false;
    uint64 domain_ = 0;
    uint64 ownerThread_ = 0;
    uint32 nextGeneration_ = 1;
    bool closed_ = false;
    bool mutating_ = false;
};

} // namespace rocks
