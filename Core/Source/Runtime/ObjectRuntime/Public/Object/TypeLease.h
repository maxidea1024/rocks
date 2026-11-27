#pragma once

#include <Object/ClassDefaults.h>
#include <Base/NumericLimits.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Meta/TypeSchema.h>

namespace rocks {

class TypeRegistry;
class TypeLease;
struct RuntimeTypeOps;
struct RuntimeFieldOps;
struct RuntimeEventOps;
struct RuntimeStructOps;
struct RuntimeStructFieldOps;

// 등록된 리플렉션 struct 하나의 비소유 view다(Docs/Design/ReflectionFoundations.ko.md 4.6절).
// 그것을 얻어 온 TypeLease(클래스·struct)가 사는 동안 유효하다 — 등록이 필드의 struct를 의존으로
// 붙잡으므로(Unregister는 Busy) 중첩 view도 바깥 lease 수명 동안 유효하다. 보관하지 않는다. 빈
// view에서 IsEmpty 외의 접근은 Fatal이다.
//
// 사용 예(codec이 struct 멤버를 스키마 순서로 돈다):
//   StructView view = lease.FieldStruct(fieldIndex);
//   for (usize index = 0; index < view.Schema().fields.Size(); ++index) {
//       const RuntimeStructFieldOps& member = view.FieldOps(index);
//   }
class ROCKS_OBJECTRUNTIME_API StructView {
public:
    static constexpr usize NoField = IndexNone;

    constexpr StructView() noexcept = default;

    bool IsEmpty() const noexcept {
        return record_ == nullptr;
    }

    // struct 스키마다(base 없음, id·version·name·fields·flags — flags는 TypeFlags::Atomic만).
    const TypeSchema& Schema() const noexcept;
    const RuntimeStructOps& StructOps() const noexcept;

    // 스키마 순서 schemaIndex 멤버의 함수표다. 범위 밖은 Fatal이다.
    const RuntimeStructFieldOps& FieldOps(usize schemaIndex) const noexcept;

    // 필드 ID의 스키마 위치다(정렬 색인 이진 탐색, 없으면 NoField).
    usize FieldIndex(Guid fieldId) const noexcept;

    // schemaIndex 멤버의 등록 enum·struct다(대상 형식 — 배열 멤버면 원소의 enum·struct, 그 종류가
    // 아니면 nullptr·빈 view).
    const EnumSchema* FieldEnum(usize schemaIndex) const noexcept;
    StructView FieldStruct(usize schemaIndex) const noexcept;

    // schemaIndex Map 멤버의 키 enum이다(리플렉션 B9f2 — 키가 Enum이 아니거나 Map이 아니면 nullptr,
    // TypeLease::FieldKeyEnum과 같은 규칙).
    const EnumSchema* FieldKeyEnum(usize schemaIndex) const noexcept;

    // 등록이 계산한 값: 중첩 깊이(1 이상, MaxStructDepth 이하 — struct 원소 배열 멤버는 원소 깊이 +
    // 1), 강한 참조 멤버가 있는지(Transient 포함·중첩 포함·ObjectRefArray·StructRefArray 포함),
    // Required 멤버가 있는지(중첩 struct·배열 멤버·원소 struct 포함, Transient 멤버 아래 제외 —
    // 그래프 로드의 Required 실효 값 검사가 이것으로 검사할 최상위 필드를 고른다), 소유
    // 객체가 WorldOnly여야 하는지(원소 struct 포함), Registry 컨테이너(ObjectRefArray·
    // StructRefArray) 멤버를 (중첩 struct까지) 담는지. 마지막 값이 참인 참조 struct는
    // StructRefArray의 원소가 될 수 없다.
    uint32 Depth() const noexcept;
    bool ContainsStrongReference() const noexcept;
    bool ContainsRequired() const noexcept;
    bool RequiresWorldOnlyOwner() const noexcept;
    bool ContainsReferenceArray() const noexcept;

    // Name 값(ValueKind::Name — 단일 멤버·Array<Name> 멤버)을 담는지다. 등록이 계산한 형질이며 중첩
    // struct 멤버와 struct 원소 배열 멤버의 원소 struct에서 전파되고 Transient 멤버도 센다. 이것이
    // 참인 struct는 그래프 복원 1단계의 관찰 대상(FieldCodecHooks::ObservedStructs)이 될 수 없다 —
    // 1단계는 pool에 닿지 않아 Name 토큰을 만들 수 없기 때문이다(직렬화기가 InvalidArgument로
    // 거부한다, Docs/Design/ReflectionExtensions.ko.md 4.1절). 빈 view는 Fatal이다.
    //
    // 사용 예:
    //   if (lease.Struct().ContainsStrongReference() || lease.Struct().ContainsName()) {
    //       return Result<void>::Failure(ErrorCode::InvalidArgument);   // 관찰할 수 없다
    //   }
    bool ContainsName() const noexcept;

    // weak 참조 값(ValueKind::WeakObjectReference — 단일 멤버·Array<WeakObjectRef> 멤버)을
    // 담는지다(리플렉션 B9d1). 등록이 계산한 형질이며 중첩 struct 멤버와 struct 원소 배열 멤버의
    // 원소 struct에서 전파되고 Transient 멤버도 센다. weak는 강한 참조가 아니므로
    // ContainsStrongReference와 무관하다(Array<S>·SubstituteStruct 대상 그대로다). 이것이 참인
    // struct는 그래프 복원 1단계의 관찰 대상(FieldCodecHooks::ObservedStructs)이 될 수 없다 —
    // 1단계는 대상 노드 handle을 만들 수 없어 weak 값이 늘 null로 보이기 때문이다(직렬화기가
    // InvalidArgument로 거부한다, Docs/Design/ReflectionExtensions.ko.md 5.1절). 빈 view는
    // Fatal이다.
    //
    // 사용 예:
    //   if (lease.Struct().ContainsWeakReference()) {
    //       return Result<void>::Failure(ErrorCode::InvalidArgument);   // 관찰할 수 없다
    //   }
    bool ContainsWeakReference() const noexcept;

    // InstancedStruct 값(ValueKind::InstancedStruct — 단일 멤버·원소 배열·정적 배열·Map 값 멤버)을
    // 담는지다(리플렉션 B9j). 등록이 계산한 형질이며 중첩 struct 멤버와 struct 원소 배열·Optional·
    // Map 멤버의 대상 struct에서 전파되고 Transient 멤버도 센다. 이것이 참인 struct는
    // InstancedStruct의 내용이 될 수 없고(중첩 금지 — detail::CheckInstancedStructContent,
    // Docs/Design/ReflectionExtensions.ko.md 11절) 그래프 복원 1단계의 관찰 대상도 될 수 없다(담긴
    // 타입의 lease를 1단계 임시 값에 둘 곳이 없다 — 직렬화기가 InvalidArgument로 거부한다). 빈
    // view는 Fatal이다.
    //
    // 사용 예:
    //   if (lease.Struct().ContainsInstancedStruct()) { /* InstancedStruct에 담을 수 없다 */ }
    bool ContainsInstancedStruct() const noexcept;

    // 선언된 struct 플래그 TypeFlags::Atomic(ROCKS_STRUCT(Atomic))이 있는지다 — 값이 통째
    // 단위다(Docs/Design/ReflectionExtensions.ko.md 3.1절: 차이 저장은 멤버 하나라도 기준과
    // 다르면 모든 멤버를 쓰고, override 집합·되돌리기 단위가 그 struct 필드 경로다). 차이 쓰기·
    // override 기록(A5)과 텍스트 출력이 멤버로 내려갈지 정할 때 이것을 본다. 로드·경로 접근은
    // Atomic과 무관하게 멤버 단위다. 등록이 계산한 형질이 아니라 스키마 flags의 비트이며, 멤버
    // struct의 Atomic을 바깥으로 전파하지 않는다(비Atomic struct 안의 Atomic 멤버는 그 멤버
    // 경로에서 멈춘다). 빈 view는 Fatal이다.
    //
    // 사용 예(차이 저장이 멤버로 내려갈지 고른다):
    //   const StructView view = lease.FieldStruct(fieldIndex);
    //   if (view.IsAtomic()) { /* 기준과 다르면 struct Record 전체를 쓴다 */ }
    bool IsAtomic() const noexcept;

private:
    friend class TypeRegistry;
    friend class TypeLease;

    explicit StructView(const void* record) noexcept : record_(record) {
    }

    const void* record_ = nullptr;
};

// TypeRegistry 레코드의 종류다. Class는 Object 파생 타입(Register·Acquire), Enum은 리플렉션
// enum(RegisterEnum·AcquireValueType), Struct는 리플렉션 struct(B4)다.
enum class TypeKind : uint8 {
    Class,
    Struct,
    Enum
};

// 불변 스키마와 콜백 코드의 수명을 유지한다. lease가 있으면 Unregister는 Busy다.
// Registry와 그 Allocator가 모든 lease보다 오래 살아야 한다.
// 서로 다른 lease의 Clone/Reset은 동시 호출 가능하나 같은 래퍼의 동시 변경은 금지한다.
class ROCKS_OBJECTRUNTIME_API TypeLease {
public:
    // FieldIndex가 필드를 찾지 못했을 때의 값이다.
    static constexpr usize NoField = IndexNone;

    TypeLease() noexcept = default;
    TypeLease(TypeLease&& other) noexcept;
    TypeLease& operator=(TypeLease&& other) noexcept;
    TypeLease(const TypeLease&) = delete;
    TypeLease& operator=(const TypeLease&) = delete;
    ~TypeLease();

    Result<TypeLease> Clone() const noexcept;
    // 반환 참조는 이 lease 또는 같은 레코드를 지키는 다른 lease가 살아 있는 동안 유효하다.
    // 임시 lease에서 빌린 참조를 반환하는 호출은 금지한다. 빈 lease 접근은 즉시 종료한다.
    const TypeSchema& Schema() const& noexcept;
    const TypeSchema& Schema() const&& = delete;
    const RuntimeTypeOps& Ops() const& noexcept;
    const RuntimeTypeOps& Ops() const&& = delete;

    // 아직 이 타입의 객체를 만든 적이 없을 때의 ObjectOffset이다.
    static constexpr usize NoObjectOffset = NumericLimits<usize>::Max();

    // 이 타입 객체의 저장소 시작에서 Object 기반까지의 byte 거리다. C++ 타입마다 고정이지만 0이라는
    // 보장은 없다(Object는 가상 함수가 없어 가상 함수를 가진 파생은 MSVC ABI에서 vfptr 뒤에 Object
    // 기반을 둔다). ObjectRegistry가 첫 생성 때 기록하므로 그 전에는 NoObjectOffset이다. Registry는
    // 소유자 저장소 범위 검사에서 `object - ObjectOffset()`으로 저장소 시작을 얻는다(Docs/Design/
    // GarbageCollectionScale.ko.md 6.4절 hot·cold). 빈 lease는 Fatal이다.
    usize ObjectOffset() const noexcept;

    // ObjectRegistry 생성 경로가 생성마다 부른다. 기록이 없으면 offset을 기록하고 true, 있으면
    // 같은지를 돌려준다(다르면 construct가 같은 타입에서 다른 기반 위치를 돌려준 함수표 오류다). 빈
    // lease는 Fatal이다.
    bool RecordObjectOffset(usize offset) const noexcept;

    // 클래스 기본값 레코드다(Docs/Design/RuntimeClassMetadata.ko.md 'A5a 레코드' — 만드는 쪽은
    // ObjectSerialization AcquireClassDefaults가 builder를 준다). 게시된 레코드가 있으면 그것을,
    // 실패를 기억했으면 그 오류를 돌려준다(diagnostic에 기억한 진단). 아직 없으면 TypeRegistry 생성
    // 잠금 안에서 상태를 다시 보고 builder를 한 번 불러 결과를 게시한다: 성공은 초안 byte를
    // TypeRegistry allocator의 한 블록으로 복사하고, 실패는 IsRememberedClassDefaultsFailure면
    // 진단을 기억한다(일시 오류는 기억하지 않는다). 어느 스레드에서나 부를 수 있다. 같은 스레드가
    // builder 안에서 다시 부르면 Fatal이다(생성자 순수성 계약 위반). 반환 레코드는 이 lease(또는
    // 같은 레코드를 지키는 다른 lease)가 살아 있는 동안 유효하다.
    //
    // 오류: 클래스가 아닌 lease·null builder InvalidArgument, builder의 오류, 게시 할당 실패
    // OutOfMemory(기억하지 않는다). 빈 lease는 Fatal이다.
    Result<const ClassDefaults*>
    ResolveClassDefaults(ClassDefaultsBuilder builder, void* user,
                         ClassDefaultsDiagnostic* diagnostic = nullptr) const noexcept;

    // base 사슬의 모든 클래스 레코드에 생성 코덱 표지(RuntimeTypeOps::generatedFieldCodecs)가
    // 있는지다(클래스 기본값 레코드의 원본 모드 인코딩 조건). 빈 lease는 Fatal이다.
    bool HasGeneratedFieldCodecs() const noexcept;

    // ID는 값으로 반환하므로 lease 종료 후에도 보관할 수 있다. 빈 lease는 Fatal이다.
    TypeId Id() const noexcept;

    // 레코드 종류다. Acquire는 Class만, AcquireValueType은 Enum·Struct만 돌려준다. 빈 lease는
    // Fatal이다. Enum lease의 Schema()는 id·name만 있고 필드가 없으며 Ops()는 nativeType·module만
    // 있다. Struct lease의 Schema()는 id·version·name·fields·flags이고 Ops()는 size·alignment·
    // nativeType·module이다(멤버 함수표 등은 StructOps()).
    TypeKind Kind() const noexcept;

    // Enum lease의 enum 스키마다. Enum이 아니거나 빈 lease면 Fatal이다. 수명은 Schema()와 같다.
    const EnumSchema& Enum() const& noexcept;
    const EnumSchema& Enum() const&& = delete;

    // Class·Struct lease에서 fieldIndex(Schema().fields의 위치) 필드가 가리키는 enum의 스키마다
    // (대상 형식 — enum 원소 배열이면 원소 enum). 그 필드가 Enum 값·enum 원소 배열이 아니면
    // nullptr이다. 등록이 그 enum을 의존으로 붙잡으므로(Unregister는 Busy)
    // 반환 참조는 이 lease 수명 동안 유효하다. 범위 밖 index나 빈 lease는 Fatal이다.
    //
    // 사용 예(저장된 값 검증):
    //   const EnumSchema* values = lease.FieldEnum(index);
    //   if (values && !IsDeclaredEnumValue(*values, stored)) { ... }
    const EnumSchema* FieldEnum(usize fieldIndex) const& noexcept;
    const EnumSchema* FieldEnum(usize fieldIndex) const&& = delete;

    // Class·Struct lease에서 fieldIndex 필드가 가리키는 등록 struct의 view다(대상 형식 — struct
    // 원소 배열이면 원소 struct). 그 필드가 Struct 값·struct 원소 배열이 아니면 빈 view다. 반환
    // view는 이 lease 수명 동안 유효하다. 범위 밖 index나 빈 lease는 Fatal이다.
    StructView FieldStruct(usize fieldIndex) const& noexcept;
    StructView FieldStruct(usize fieldIndex) const&& = delete;

    // Class·Struct lease에서 fieldIndex Map 필드의 키가 가리키는 enum의 스키마다(리플렉션 B9f2 —
    // FieldKeyType이 Enum인 Map, Docs/Design/ReflectionExtensions.ko.md 1.1절 키 대상). 키가 Enum이
    // 아니거나 Map이 아니면 nullptr이다. 등록이 그 enum을 의존으로 붙잡으므로 반환 참조는 이 lease
    // 수명 동안 유효하다. 범위 밖 index나 빈 lease는 Fatal이다. codec은 위치 프레임의
    // FieldCodecFrame::keyEnumeration으로 받는다(FieldScope).
    //
    // 사용 예:
    //   FieldScope scope(context, field, schema.id, lease.FieldEnum(index),
    //                    lease.FieldStruct(index), FieldCodecFrame::NoIndex,
    //                    lease.FieldKeyEnum(index));
    const EnumSchema* FieldKeyEnum(usize fieldIndex) const& noexcept;
    const EnumSchema* FieldKeyEnum(usize fieldIndex) const&& = delete;

    // Class lease에서 schemaIndex(Schema().fields의 위치) 필드의 등록 함수표다(read·write·codec·
    // 경로 접근 thunk). 함수표가 없는 필드(Opaque)는 nullptr이다. 반환 포인터는 이 lease 수명 동안
    // 유효하다. Class가 아닌 lease·빈 lease·범위 밖 index는 Fatal이다(struct 멤버 함수표는
    // StructView::FieldOps).
    //
    // 사용 예(경로 접근이 최상위 필드의 thunk를 찾는다):
    //   const usize index = lease.FieldIndex(fieldId);
    //   const RuntimeFieldOps* ops = index != TypeLease::NoField ? lease.FieldOps(index) : nullptr;
    const RuntimeFieldOps* FieldOps(usize schemaIndex) const& noexcept;
    const RuntimeFieldOps* FieldOps(usize schemaIndex) const&& = delete;

    // Class lease에서 schemaIndex 필드의 이벤트 연산표다(FieldKind::Event 필드 — Docs/Design/
    // EventReflection.ko.md ED16). 이벤트 필드가 아니면 nullptr이다. 반환 포인터는 이 lease 수명
    // 동안 유효하다. Class가 아닌 lease·빈 lease·범위 밖 index는 Fatal이다.
    //
    // 사용 예(도구가 이벤트에 바인딩한다):
    //   const usize index = lease.FieldIndex(diedId);
    //   const RuntimeEventOps* died = nullptr;
    //   if (index != TypeLease::NoField) {
    //       died = lease.EventOps(index);
    //   }
    //
    //   if (died) {
    //       auto bound = died->bind(registry, access, actor, listener, onDiedId);
    //   }
    const RuntimeEventOps* EventOps(usize schemaIndex) const& noexcept;
    const RuntimeEventOps* EventOps(usize schemaIndex) const&& = delete;

    // Class lease의 변경 통지 대상(Notify 필드·함수 — Docs/Design/ChangeNotification.ko.md CN-D2)
    // 수다. 통지 번호는 0부터 이 값 미만이다(필드 → 기반부터의 함수, 각각 선언 순서 — 등록 때 정해
    // 지는 실행 중 값이라 저장하지 않는다). Class가 아닌 lease는 0이다. 빈 lease는 Fatal이다.
    uint32 NotifyCount() const noexcept;

    // 대상 ID(필드 FieldId·함수 FunctionId)의 통지 번호다(정렬 표의 이진 탐색). 통지 대상이 아니면
    // NotifyIndexNone이다. 빈 lease는 Fatal이다.
    //
    // 사용 예:
    //   const uint32 index = lease.NotifyIndex(healthId);
    //   if (index != NotifyIndexNone) { /* 구독할 수 있다 */ }
    uint32 NotifyIndex(Guid targetId) const noexcept;

    // 통지 번호의 대상 ID다. index가 NotifyCount() 이상이면 Fatal이다.
    Guid NotifyTarget(uint32 index) const noexcept;

    // Class·Struct lease에서 필드 ID의 스키마 위치다(필드 ID 정렬 색인의 이진 탐색 — 상속 필드도
    // 파생 스키마의 같은 ID로 찾는다). 없으면 NoField다(Enum lease는 필드가 없어 항상 NoField). 빈
    // lease는 Fatal이다.
    usize FieldIndex(Guid fieldId) const noexcept;

    // Struct lease(AcquireValueType(id, TypeKind::Struct))의 view·실행 정보다. Struct가 아니거나 빈
    // lease면 Fatal이다.
    StructView Struct() const& noexcept;
    StructView Struct() const&& = delete;
    const RuntimeStructOps& StructOps() const& noexcept;
    const RuntimeStructOps& StructOps() const&& = delete;

    // 이름은 이 lease 또는 같은 레코드를 지키는 다른 lease에 종속된 UTF-8 byte view다.
    // 임시 lease에 호출하면 view가 곧바로 무효화될 수 있어 금지한다. 빈 lease는 Fatal이다.
    StringView Name() const& noexcept;
    StringView Name() const&& = delete;

    // immutable schema ancestry 조회다. 빈 lease는 Fatal이다. 스키마 ID 기준이므로 같은 native
    // 타입을 다른 TypeId로 등록한 별칭은 구분된다.
    bool IsA(TypeId expected) const noexcept;

    // 이 타입이 nativeType(NativeTypeKey<T>())의 C++ 타입이거나 그 파생인지다. nullptr(Object)는
    // 항상 참이다. Resolve<T>·SetReference·NewObject<T>가 이 규칙으로 기반 타입 접근을 허용한다.
    // 단일 비가상 상속만 등록되므로 참이면 Object*에서 T*로의 static_cast가 안전하다. 빈 lease는
    // Fatal.
    bool IsNativeA(const void* nativeType) const noexcept;

    bool IsValid() const noexcept {
        return record_ != nullptr;
    }

    void Reset() noexcept;

private:
    friend class TypeRegistry;

    TypeLease(TypeRegistry* owner, void* record) noexcept : owner_(owner), record_(record) {
    }

    TypeRegistry* owner_ = nullptr;
    void* record_ = nullptr;
};

} // namespace rocks
