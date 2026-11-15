#pragma once

#include <Object/AssetId.h>
#include <Object/ConstructionContext.h>
#include <Object/FieldCodecHooks.h>
#include <Object/RuntimeTypeOps.h>
#include <Object/StructRefArray.h>
#include <Object/TypeLease.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <Serialization/RecordList.h>
#include <Serialization/ValueArray.h>

// 필드 codec의 문맥·정책이다(Docs/Design/ReflectionFoundations.ko.md 6.1–6.4절 — hook 묶음과 진단·
// 외부 참조 정보는 Object/FieldCodecHooks.h). 생성 codec
// thunk(RuntimeFieldOps::encode·decode)는 이 문맥으로만 참조 wire·진단·Soft 수집·예산에 닿으므로,
// 그래프 형식(graph v2의 U32 local ID·외부 참조 비트)이나 저장 집합이 바뀌어도 생성 코드를 다시
// 만들지 않는다. 직렬화기(ObjectSerialization)가 문맥을 구현하고 필드마다 FieldScope로 위치를
// 정한다. 이 헤더는 생성 헤더와 직렬화기만 include한다.
namespace rocks {

class NamePool;

// 미선언 enum 값을 읽었을 때: 거부(InvalidArgument) 또는 진단 후 대체 값(EnumReplacementValue).
enum class UndeclaredEnumPolicy : uint8 {
    Reject,
    ReportAndDefault
};

// 저장 버전이 현재 스키마보다 높을 때: 거부 또는 아는 필드만 읽고 진단(읽기 전용 열기 — A1).
enum class NewerVersionPolicy : uint8 {
    Reject,
    LoadNewerSkippingUnknown
};

// 등록되지 않은 클래스를 가리키는 ClassRef 값(ValueKind::ClassReference)을 읽었을 때: 거부(기본,
// NotFound) 또는 진단(UnknownClassReference) 후 null(Docs/Design/ReflectionExtensions.ko.md 5.2절 —
// 클래스를 지운 패키지를 열어 고칠 때 쓴다). 등록은 됐지만 클래스가 아니거나 Base의 파생이 아닌
// 값은 정책과 무관하게 InvalidArgument다. Map 키·Set 원소 자리에는 값을 바꾸는 정책을 쓰지
// 않는다(늘 거부 — 같은 문서 1.7절, 그 자리는 리플렉션 B9f2가 연다).
enum class UnknownClassPolicy : uint8 {
    Reject,
    ReportAndNull
};

// 스키마에 없는 필드: 진단 후 건너뛰기(기본) 또는 거부(쿠킹 데이터는 정확히 일치해야 한다).
enum class UnknownFieldPolicy : uint8 {
    SkipAndReport,
    Reject
};

// 저장 정책이다. rejectMissingRequired는 패키지 저장·쿠킹이 켜며 Required 참조가 null이면
// InvalidArgument다. excludedFlags의 비트(예: 쿠킹의 EditorOnly)를 유효 플래그에 가진 필드는
// 저장하지 않는다(Transient는 항상 제외).
struct FieldEncodePolicy {
    bool rejectMissingRequired = false;
    uint32 excludedFlags = 0;
};

// 복원 정책이다. 기본은 미선언 enum·높은 버전 거부, 모르는 필드는 진단 후 건너뛰기다. Record에 없는
// 영속 필드·struct 멤버는 정책이 아니라 형식 규칙으로 기준 값(생성 직후 값)을 유지한다(누락 = 기준
// — Docs/Design/ObjectFoundations.ko.md 4절). excludedFlags는 이 데이터에 없어야 하는 유효 플래그다
// (저장의 FieldEncodePolicy::excludedFlags와 짝 — 쿠킹 로드 = EditorOnly): 그런 필드·멤버(배열이면
// 그 원소 포함)가 있으면 InvalidArgument이고, 없으면 기준 값이며 Required 실효 값 검사에서도 뺀다.
//
// 사용 예(쿠킹 데이터 로드):
//   ObjectGraphLoadOptions options;
//   options.policy.unknownField = UnknownFieldPolicy::Reject;
//   options.policy.excludedFlags = static_cast<uint32>(FieldFlags::EditorOnly);
struct FieldDecodePolicy {
    UndeclaredEnumPolicy undeclaredEnum = UndeclaredEnumPolicy::Reject;
    NewerVersionPolicy newerVersion = NewerVersionPolicy::Reject;
    UnknownFieldPolicy unknownField = UnknownFieldPolicy::SkipAndReport;
    uint32 excludedFlags = 0;
    UnknownClassPolicy unknownClass = UnknownClassPolicy::Reject;
};

class RecordScope;

// 지금 처리하는 필드 위치 하나다. field는 그 필드(struct 멤버면 멤버), declaringType은 그 필드를
// 선언한 클래스·struct, effectiveFlags는 PropagateFieldFlags로 전파된 플래그(배열 원소와 원소
// struct 멤버는 배열 필드의 것을 공유·전파한다), enumeration은 Enum 필드·enum 원소 배열의 등록된
// enum 스키마(그 밖은 nullptr), structure는 Struct 필드·struct 원소 배열의 등록 struct(그 밖은 빈
// view), index는 배열 필드에서 지금 처리하는 원소 index(아니면 NoIndex — 원소마다 새 프레임을
// 만들지 않고 ElementIndexScope가 이 칸을 쓴다, 진단 경로는 [배열[i], 멤버])다.
struct FieldCodecFrame {
    static constexpr uint32 NoIndex = NumericLimits<uint32>::Max();

    const FieldSchema* field = nullptr;
    TypeId declaringType{};
    uint32 effectiveFlags = 0;
    const EnumSchema* enumeration = nullptr;
    StructView structure{};
    uint32 index = NoIndex;
    const FieldCodecFrame* parent = nullptr;
};

// 참조(강한·weak)의 기대 형식이다. schemaType은 스키마의 기대 타입(0 = Object), staticNative는
// 생성 thunk의 정적 대상 타입 NativeTypeKey<T>()(Object거나 모르면 nullptr)다. 대상은 둘 다를 상속
// 규칙으로 만족해야 한다.
struct ReferenceExpectation {
    TypeId schemaType{};
    const void* staticNative = nullptr;
};

// 읽은 강한 참조다. 1단계는 isNull만 정확하고 handle은 비어 있다. 2단계는 만든 객체 handle이다.
struct DecodedReference {
    bool isNull = true;
    ObjectHandle handle{};
};

class FieldScope;
class ElementIndexScope;

// 두 문맥의 공통 기반이다: 위치(프레임) 스택, 진단·Soft 보고, 예산, 참조 wire. 직렬화기가
// 파생해 구현한다. 같은 문맥을 여러 스레드에서 쓰지 않는다.
class FieldCodecContext {
public:
    FieldCodecContext(const FieldCodecContext&) = delete;
    FieldCodecContext& operator=(const FieldCodecContext&) = delete;

    // 지금 위치다. FieldScope 밖에서 Field()를 부르면 Fatal이다.
    const FieldCodecFrame* Current() const noexcept {
        return current_;
    }

    const FieldSchema& Field() const noexcept {
        if (!current_) {
            platform::Fatal("FieldCodecContext::Field called outside a field scope");
        }

        return *current_->field;
    }

    uint32 EffectiveFlags() const noexcept {
        return current_ ? current_->effectiveFlags : 0;
    }

    const EnumSchema* FieldEnum() const noexcept {
        return current_ ? current_->enumeration : nullptr;
    }

    TypeId ObjectType() const noexcept {
        return objectType_;
    }

    uint32 ObjectIndex() const noexcept {
        return objectIndex_;
    }

    // 참조 필드(강한·weak)의 wire tag다(graph v2 = U32 — local ID와 외부 참조 비트, weak는 외부
    // 참조 비트를 쓰지 않는다). 참조 필드의 encode는 이 값을 돌려주고 decode는 저장 tag를 이것과
    // 비교한다.
    uint32 ReferenceWireType() const noexcept {
        return referenceWire_;
    }

    // 진단·수집을 기록하는 단계인지다(복원 2단계는 거짓 — 1단계가 이미 판정했다).
    bool IsRecording() const noexcept {
        return recording_;
    }

    // Name 필드 값(ValueKind::Name)의 토큰 pool이다(저장 encode가 토큰의 표시 문자열을 읽는다 —
    // 직렬화기가 대상 Registry의 RegistryOptions::names로 정한다, 없으면 nullptr). 복원 문맥은
    // pool에 닿지 않으므로(1단계는 표시 문자열을 기록하고 2a가 intern한다) 정하지 않는다.
    //
    // 사용 예(Name 값 encode):
    //   auto display = AcquireNameFieldText(context.Names(), value.name, context.Scratch());
    NamePool* Names() const noexcept {
        return names_;
    }

    // 진단을 받을 곳이 있는지다(기록하지 않는 단계는 참이다 — 1단계가 이미 판정했다). 값을 바꾸는
    // 정책(ReportAndDefault)은 이것이 거짓이면 InvalidArgument다(진단 없이 값을 바꾸지 않는다).
    bool CanReport() const noexcept {
        return !recording_ || hooks_ != nullptr;
    }

    // 지금 위치로 진단을 만든다. 기록하지 않는 단계(복원 2단계)에서는 아무것도 하지 않는다.
    Result<void> Report(FieldDiagnosticKind kind, int64 value = 0, TypeId valueType = {}) noexcept;

    // 스키마에 없는 필드를 보고한다. 최상위면 경로가 비어 있고, struct 안이면 경로는 감싼 struct
    // 필드까지다(같은 ID의 최상위 필드와 구별된다).
    Result<void> ReportUnknownField(Guid field) noexcept {
        if (!recording_ || !hooks_) {
            return Result<void>::Success();
        }

        PathStep steps[MaxPropertyPathSteps];
        FieldDiagnostic diagnostic;
        diagnostic.kind = FieldDiagnosticKind::UnknownField;
        diagnostic.objectType = objectType_;
        diagnostic.objectIndex = objectIndex_;
        diagnostic.path = BuildPath(steps);
        diagnostic.field = field;
        return hooks_->Report(diagnostic);
    }

    // 지금 위치의 non-null 소프트 참조를 보고한다(플래그·위치는 문맥이 채운다).
    Result<void> CollectSoftReference(AssetId asset, StringView objectPath,
                                      TypeId expectedType) noexcept;

    // 이 필드 처리의 작업량이다. 복원 1단계는 2단계 몫까지 두 배를 청구하고, 2단계는 청구하지
    // 않는다.
    virtual Result<void> Spend(uint64 work) noexcept = 0;

    // 복원 2단계에만 있는 작업량(staged 쓰기·문자열 복사·참조 연결)이다. 1단계가 미리 청구하고
    // 2단계와 저장은 청구하지 않는다.
    virtual Result<void> ReserveApply(uint64 work) noexcept = 0;

    // ClassRef 값(ValueKind::ClassReference — Docs/Design/ReflectionExtensions.ko.md 5.2절)의
    // 클래스 type을 대상 TypeRegistry에서 찾아 lease를 준다. base는 스키마의 Base 클래스
    // ID(value.type — 배열이면 element.type)이고, type이 base 자신이거나 그 파생(스키마 사슬
    // TypeLease::IsA)이어야 한다(base 0이면 Base ID를 보지 않는다 — 등록이 스키마의 0을 거부하므로
    // codec이 주는 base는 0이 아니다). 직렬화기가 구현한다: 복원 1단계 문맥은 TypeRegistry만
    // 조회하고(ObjectRegistry에 닿지 않는다 — 잠금으로 보호되는 TypeRegistry::Acquire라
    // worker에서도 부를 수 있다), 저장 문맥은 ObjectRegistry::AcquireRegisteredType(소유
    // 스레드)이다. 복원 2단계는 조회하지 않는다(1단계 판정 기록 — TakeClassReference). 판정 규칙은
    // detail::CheckClassReferenceLease(Object/ClassRef.h) 하나다.
    //
    // 반환: 그 클래스의 lease. 실패 — 미등록 NotFound, 클래스가 아닌 레코드(struct·enum)·base의
    // 파생이 아님 InvalidArgument, 종료 ShuttingDown, 문맥·스레드 오류는 그 오류. 기본 문맥은
    // Unsupported다(ClassRef를 지원하지 않는 문맥 — 시험 문맥 등).
    //
    // 사용 예(codec이 부르는 형태):
    //   auto found = context.AcquireClassReference(value.guid, type.type);
    //   if (!found) { return Result<void>::Failure(found.Error()); }
    virtual Result<TypeLease> AcquireClassReference(TypeId type, TypeId base) noexcept {
        (void)type;
        (void)base;
        return Result<TypeLease>::Failure(ErrorCode::Unsupported);
    }

    // 값 임시 저장에 쓰는 할당기다(저장·1단계는 예산을 청구한다).
    virtual Allocator& Scratch() noexcept = 0;

    // 관찰 대상 struct 임시 값과 그 안 배열의 원소 버퍼를 할당하는 할당기다(복원 1단계의 struct
    // hook 관찰 — Docs/Design/ReflectionFoundations.ko.md 5.6절). 바이트를 작업량으로 청구하되
    // Scratch와 달리 단일 할당 한도(maxValueBytes)가 없다: 관찰 struct 안 배열의 원소 버퍼는 wire
    // 값 크기보다 클 수 있다(예: 빈 String 원소 20만 개 — wire 8 byte, native 수십 byte). work
    // 예산이 상한이다. 복원 2단계와 저장은 관찰하지 않으므로 쓰지 않는다.
    virtual Allocator& ObservationScratch() noexcept = 0;

    // 중첩 Record(struct 값)의 TLV 수를 그래프 필드 누계(maxTotalFields)에 더한다. 복원 2단계는
    // 세지 않는다.
    virtual Result<void> AddFields(usize count) noexcept = 0;

    // 배열 원소 수를 그래프 원소 누계(maxTotalElements)에 더한다(넘으면 CapacityExceeded). 배열
    // codec이 header를 해석한 직후·원소를 읽거나 할당하기 전에 부른다(저장은 원소를 쓰기 전).
    // 복원 2단계는 세지 않는다.
    virtual Result<void> AddElements(usize count) noexcept = 0;

protected:
    FieldCodecContext(FieldCodecHooks* hooks, uint32 referenceWire) noexcept
        : hooks_(hooks), referenceWire_(referenceWire) {
    }

    ~FieldCodecContext() = default;

    // 직렬화기가 객체마다 정한다.
    void SetObject(TypeId type, uint32 index) noexcept {
        objectType_ = type;
        objectIndex_ = index;
    }

    // 복원 2단계는 false다(진단·수집을 버린다).
    void SetRecording(bool recording) noexcept {
        recording_ = recording;
    }

    // 저장 문맥이 Names()를 정한다(그 Registry의 RegistryOptions::names).
    void SetNames(NamePool* names) noexcept {
        names_ = names;
    }

    bool Recording() const noexcept {
        return recording_;
    }

    FieldCodecHooks* Hooks() const noexcept {
        return hooks_;
    }

    // 프레임 사슬로 경로를 만든다(바깥 → 안 순서). 반환 span은 steps를 가리킨다.
    Span<const PathStep> BuildPath(PathStep (&steps)[MaxPropertyPathSteps]) const noexcept;

private:
    friend class FieldScope;
    friend class ElementIndexScope;

    FieldCodecHooks* hooks_;
    NamePool* names_ = nullptr;
    // 맨 위 프레임이다. ElementIndexScope가 그 index 칸을 쓰므로 non-const다(Current()는 const를
    // 돌려준다).
    FieldCodecFrame* current_ = nullptr;
    TypeId objectType_{};
    uint32 objectIndex_ = 0;
    uint32 referenceWire_;
    bool recording_ = true;
};

// 위치 하나에 들어가고 나온다(RAII). 바깥 프레임의 유효 플래그를 PropagateFieldFlags로 전파한다.
// scope는 문맥보다 먼저 끝나야 하고 들어간 순서의 역순으로 끝나야 한다.
//
// 사용 예(직렬화기):
//   FieldScope scope(context, schema.fields[index], schema.id, lease.FieldEnum(index));
//   auto wire = ops->encode(*ops, registry, access, handle, payload, context);
class FieldScope {
public:
    [[nodiscard]] FieldScope(FieldCodecContext& context, const FieldSchema& field,
                             TypeId declaringType, const EnumSchema* enumeration = nullptr,
                             StructView structure = {},
                             uint32 index = FieldCodecFrame::NoIndex) noexcept
        : context_(context), previous_(context.current_) {
        frame_.field = &field;
        frame_.declaringType = declaringType;
        frame_.enumeration = enumeration;
        frame_.structure = structure;
        frame_.index = index;
        frame_.parent = context.current_;
        frame_.effectiveFlags =
            context.current_ ? PropagateFieldFlags(context.current_->effectiveFlags, field.flags)
                             : field.flags;
        context.current_ = &frame_;
    }

    ~FieldScope() {
        // scope는 들어간 순서의 역순으로 끝나야 한다.
        if (context_.current_ != &frame_) {
            platform::Fatal("FieldScope ended out of order");
        }

        // parent는 const 사슬이므로 바깥 프레임(non-const)은 따로 보관한 것으로 되돌린다.
        context_.current_ = previous_;
    }

    FieldScope(const FieldScope&) = delete;
    FieldScope& operator=(const FieldScope&) = delete;

private:
    FieldCodecContext& context_;
    FieldCodecFrame* previous_;
    FieldCodecFrame frame_;
};

// 배열 필드 프레임의 원소 index 칸을 쓴다(RAII, Docs/Design/ReflectionFoundations.ko.md 5.6절).
// 원소마다 새 프레임을 만들지 않으므로 프레임은 최대 MaxPropertyPathSteps(17)개(BuildPath 버퍼
// 크기)다. 만들 때 현재 프레임이 배열 필드(value.kind == Array)이고 그 index가 NoIndex여야
// 한다(아니면 Fatal — 배열 codec 계약 위반). Set은 현재 프레임이 그 배열 프레임일 때만 쓸 수
// 있고(원소 struct 멤버 scope 안에서 부르면 Fatal), 소멸 때 index를 NoIndex로 되돌린다. 배열
// 프레임을 연 FieldScope보다 먼저 끝나야 한다.
//
// 사용 예(배열 codec):
//   ElementIndexScope element(context);
//   for (usize index = 0; index < count; ++index) {
//       element.Set(static_cast<uint32>(index));   // 진단·hook 경로가 [f[index]]가 된다
//       ...
//   }
class ElementIndexScope {
public:
    [[nodiscard]] explicit ElementIndexScope(FieldCodecContext& context) noexcept
        : context_(context), frame_(context.current_) {
        if (!frame_ || frame_->field->value.kind != ValueKind::Array ||
            frame_->index != FieldCodecFrame::NoIndex) {
            platform::Fatal("ElementIndexScope requires an array field frame without an index");
        }
    }

    ~ElementIndexScope() {
        frame_->index = FieldCodecFrame::NoIndex;
    }

    ElementIndexScope(const ElementIndexScope&) = delete;
    ElementIndexScope& operator=(const ElementIndexScope&) = delete;

    // 지금 처리하는 원소 index를 프레임에 쓴다.
    void Set(uint32 index) noexcept {
        if (context_.current_ != frame_) {
            platform::Fatal("ElementIndexScope::Set called outside its array frame");
        }

        frame_->index = index;
    }

private:
    FieldCodecContext& context_;
    FieldCodecFrame* frame_;
};

// 저장 문맥이다(그래프 저장이 구현한다). 그래프 로드의 Required 실효 값 검사(Docs/Design/
// ObjectFoundations.ko.md 4절)도 이 문맥의 검사 모드로 같은 encode 경로를 쓴다.
class FieldEncodeContext : public FieldCodecContext {
public:
    const FieldEncodePolicy& Policy() const noexcept {
        return policy_;
    }

    // 이 유효 플래그의 필드를 저장하지 않는지다(Transient는 항상).
    bool IsExcluded(uint32 effectiveFlags) const noexcept {
        return HasFlag(effectiveFlags, FieldFlags::Transient) ||
               (effectiveFlags & policy_.excludedFlags) != 0;
    }

    // Required 실효 값 검사 문맥인지다. 이 모드에서는 Required null을 거부하지 않고(정책과 무관)
    // RequiredMissing으로 보고하며, 필드별 MaxElements(ArrayTooLong)·SubstituteStruct hook·Soft
    // 수집을 하지 않는다.
    bool IsRequiredCheck() const noexcept {
        return staged_ != nullptr;
    }

    // staged 객체를 읽는 문맥이면 그 트랜잭션이다(아니면 nullptr). 생성 encode thunk
    // (detail::EncodeGeneratedField)는 이것이 있으면 Registry Resolve 대신
    // ConstructionContext::Get으로 소유자를 얻고, 수동 함수표의 범용 encode는
    // RuntimeFieldOps::readStaged로 읽는다(encode 고정을 쥐지 않는다 — 검사 중 표시가 대신한다).
    ConstructionContext* StagedSource() const noexcept {
        return staged_;
    }

    // 지금 위치가 Required이고 값이 null일 때: 검사 모드면 RequiredMissing을 보고한 뒤 성공(hook
    // 실패는 그 오류), 아니면 rejectMissingRequired일 때 보고 뒤 InvalidArgument다. 그 밖은
    // 성공이다.
    Result<void> CheckRequired(bool isNull) noexcept {
        if (!isNull || !HasFlag(EffectiveFlags(), FieldFlags::Required)) {
            return Result<void>::Success();
        }

        if (IsRequiredCheck()) {
            return Report(FieldDiagnosticKind::RequiredMissing);
        }

        if (!policy_.rejectMissingRequired) {
            return Result<void>::Success();
        }

        if (auto reported = Report(FieldDiagnosticKind::RequiredMissing); !reported) {
            return reported;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 강한 참조 하나를 쓴다: Required null 검사 뒤 대상을 expectation으로 검사하고(TypeMismatch)
    // ReferenceWireType() 형식으로 쓴다. out은 직렬화기가 이 필드의 encode에 준 writer여야 한다 —
    // 그래프 저장은 외부 참조 값의 위치를 그 writer 안 offset으로 기록하므로(번호 재배정 —
    // Docs/Design/ObjectFoundations.ko.md 5.3절) 다른 writer에 쓴 외부 참조는 InvalidArgument다.
    Result<void> WriteReference(ArchiveWriter& out, ObjectHandle target,
                                const ReferenceExpectation& expectation) noexcept {
        if (auto required = CheckRequired(target == ObjectHandle{}); !required) {
            return required;
        }

        return WriteReferenceWire(out, target, expectation);
    }

    // weak 참조 하나를 쓴다(리플렉션 B9d1 — Docs/Design/ReflectionExtensions.ko.md 5.1절).
    // ReferenceWireType() 형식이고 weak는 Required가 될 수 없으므로 Required 검사를 하지 않는다.
    // 같은 저장 단위(그래프) 안 대상만 보존한다: 대상이 null이거나 같은 Registry에서 죽었으면
    // (StaleHandle·Destroyed) null(진단 없음 — weak의 정상 상태), 살아 있으면 기대 형식을 검사한
    // 뒤(TypeMismatch) 저장 집합 안이면 local ID, 밖이면 저장 정책(Reject·Admit·Include)대로 null과
    // ExternalReference 진단(값 2)을 남긴다(그래프 저장의 규칙은 GraphExternalPolicy 주석). out은
    // WriteReference와 같이 직렬화기가 이 필드의 encode에 준 writer여야 한다(Include가 값 위치를 그
    // writer 안 offset으로 기록해 끝에 제자리로 덮어쓴다).
    //
    // 반환: 성공(값을 썼다). 실패 — 다른 Registry의 handle WrongContext(정책·hook 질의 전), 기대
    // 형식 위반 TypeMismatch, Admit hook의 Reject InvalidArgument(진단 값 0)·hook 실패는 그 오류,
    // 다른 writer InvalidArgument, 접근·종료·재진입 오류와 바이트·작업량 한도 CapacityExceeded,
    // 할당 OutOfMemory. 기본 문맥은 Unsupported다.
    //
    // 사용 예(생성 codec이 부르는 형태):
    //   return context.WriteWeakReference(out, value.reference, {type.type, staticNative});
    Result<void> WriteWeakReference(ArchiveWriter& out, ObjectHandle target,
                                    const ReferenceExpectation& expectation) noexcept {
        return WriteWeakReferenceWire(out, target, expectation);
    }

    // 중첩 Record(struct 값)의 한도다(그래프 옵션에서 — 전체 크기는 필드 값 한도, 값 하나도 같은
    // 값 한도).
    virtual RecordLimits NestedRecordLimits() const noexcept = 0;

    // 지금 위치의 struct 값 대신 저장할 값이다(hook이 없거나 검사 모드면 value). null은
    // InvalidArgument다.
    Result<const void*> SubstituteStruct(TypeId type, const void* nativeKey,
                                         const void* value) noexcept {
        FieldCodecHooks* hooks = Hooks();
        if (!hooks || !Current() || IsRequiredCheck()) {
            return Result<const void*>::Success(value);
        }

        auto substituted = hooks->SubstituteStruct(*Current(), type, nativeKey, value);
        if (substituted && substituted.Value() == nullptr) {
            return Result<const void*>::Failure(ErrorCode::InvalidArgument);
        }

        return substituted;
    }

protected:
    FieldEncodeContext(const FieldEncodePolicy& policy, FieldCodecHooks* hooks,
                       uint32 referenceWire) noexcept
        : FieldCodecContext(hooks, referenceWire), policy_(policy) {
    }

    // 검사 모드였으면 staged 트랜잭션의 검사 중 표시를 내린다.
    ~FieldEncodeContext() {
        if (staged_) {
            staged_->requiredCheck_ = false;
        }
    }

    // 이 문맥을 staged 트랜잭션의 Required 실효 값 검사 모드로 바꾸고 그 트랜잭션에 검사 중 표시를
    // 세운다(문맥이 끝날 때 내린다). 표시 동안 그 트랜잭션의 Create·SetReference·AssignReferences·
    // AssignElements·SetElementReference·GetForWrite(등록된 writeStaged·적용 쓰기)·Commit은 Busy다
    // (encode 고정의 staged판 — hook·allocator 콜백이 검사 중인 값을 바꾸지 못한다). 문맥 하나에 한
    // 번만 부르고, 이미 표시가 선 트랜잭션에 부르면 Fatal이다(검사는 겹치지 않는다). 트랜잭션은
    // 문맥보다 오래 살아야 한다.
    //
    // 사용 예(직렬화기의 검사 문맥 생성자):
    //   RequiredCheckContext(..., ConstructionContext& transaction) noexcept
    //       : FieldEncodeContext(policy, hooks, wire) {
    //       BeginRequiredCheck(transaction);
    //   }
    void BeginRequiredCheck(ConstructionContext& staged) noexcept {
        if (staged_ || staged.requiredCheck_) {
            platform::Fatal("Required check started twice");
        }

        staged.requiredCheck_ = true;
        staged_ = &staged;
    }

    virtual Result<void> WriteReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                            const ReferenceExpectation& expectation) noexcept = 0;

    // weak 참조 쓰기의 직렬화기 구현이다(WriteWeakReference). 기본은 Unsupported다(weak 참조를 쓰지
    // 않는 문맥).
    virtual Result<void> WriteWeakReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                                const ReferenceExpectation& expectation) noexcept {
        (void)out;
        (void)target;
        (void)expectation;
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 지금 위치(Current())의 저장 집합 밖 대상 target을 외부 참조로 쓸지 hooks에 묻는다(그래프
    // 저장의 Admit 정책 — FieldCodecHooks::AdmitExternalReference). 유효 플래그·Required·노드
    // 위치·경로는 문맥이 채운다. weak면 info.weak가 참이고 Required는 늘 거짓이다(slot은 호출자가
    // 0으로 준다 — weak import는 없다). hooks가 없으면 Reject다(hook 기본과 같다). 결과 값의
    // 검사와 진단은 호출자(직렬화기)가 한다.
    Result<ExternalAdmission> AdmitExternalReference(ObjectHandle target, TypeId targetType,
                                                     TypeId expectedType, uint32 slot,
                                                     bool weak = false) noexcept {
        FieldCodecHooks* hooks = Hooks();
        if (!hooks) {
            return Result<ExternalAdmission>::Success(ExternalAdmission::Reject);
        }

        PathStep steps[MaxPropertyPathSteps];
        ExternalReferenceInfo info;
        info.target = target;
        info.targetType = targetType;
        info.expectedType = expectedType;
        info.effectiveFlags = EffectiveFlags();
        info.required = !weak && HasFlag(info.effectiveFlags, FieldFlags::Required);
        info.objectIndex = ObjectIndex();
        info.path = BuildPath(steps);
        info.slot = slot;
        info.weak = weak;
        return hooks->AdmitExternalReference(info);
    }

private:
    FieldEncodePolicy policy_;
    ConstructionContext* staged_ = nullptr;
};

// 복원 문맥이다(그래프 복원이 1단계·2단계로 구현한다).
class FieldDecodeContext : public FieldCodecContext {
public:
    const FieldDecodePolicy& Policy() const noexcept {
        return policy_;
    }

    // 이 유효 플래그의 필드·멤버가 이 데이터에 없어야 하는지다(policy.excludedFlags — 쿠킹 로드의
    // EditorOnly). 있으면 직렬화기·struct codec이 InvalidArgument로 거부한다.
    bool IsExcluded(uint32 effectiveFlags) const noexcept {
        return (effectiveFlags & policy_.excludedFlags) != 0;
    }

    // 지금 위치(Current())의 필드·멤버가 저장 데이터에 있음을 알린다. 복원 1단계가 필드·멤버마다 그
    // 값을 검증한 뒤 한 번 부른다(객체 Record는 직렬화기, struct 멤버는 DecodeStructRecord).
    // 기록하지 않는 단계(복원 2단계)나 위치 밖에서는 아무것도 하지 않는다. A5 override 집합("파일에
    // 있는 값 경로 = 집합" — Docs/Design/RuntimeClassMetadata.ko.md '클래스 기본값 결정')의
    // 자리이며 지금은 NotePresent의 기본 구현이 아무것도 하지 않는다. 실패를 돌려주면 그 복원이
    // 실패한다.
    Result<void> MarkPresent() noexcept {
        if (!Recording() || !Current()) {
            return Result<void>::Success();
        }

        return NotePresent(*Current());
    }

    // 필드 payload를 읽는 reader를 연다. Record 스택(struct 값)이 있으면 맨 위 Record로, 없으면
    // 직렬화기의 최상위 경로(OpenTopLevelValue)로 연다. 1단계는 그래프 공유 예산, 2단계는 같은 크기
    // 한도와 무제한 operation·work다.
    Result<ArchiveReader> OpenValue(const FieldView& field) noexcept {
        if (depth_ != 0) {
            return Top().OpenValue(field);
        }

        return OpenTopLevelValue(field);
    }

    // 맨 위 Record reader다(struct 값을 해석하는 동안 — 없으면 nullptr). Next()로 순회한다.
    RecordReader* CurrentRecord() noexcept {
        return depth_ != 0 ? &Top() : nullptr;
    }

    // struct 값의 중첩 Record를 열어 Record 스택에 넣는다(wire가 Record가 아니면 TypeMismatch).
    // 1단계는 맨 위(없으면 객체) Record의 OpenRecordField(공유 예산 — 헤더·색인만 청구, 필드 수
    // 한도는 남은 그래프 누계)와 AddFields, 2단계는 같은 버전 범위·크기 한도·무제한
    // operation·work다.
    virtual Result<RecordScope> OpenRecord(const FieldView& field, TypeId expected,
                                           uint32 minVersion, uint32 maxVersion) noexcept = 0;

    // 마이그레이션 결과 Record를 정확히 version으로 열어 Record 스택에 넣는다(아니면 Unsupported).
    virtual Result<RecordScope> OpenMigratedRecord(Span<const uint8> record, TypeId expected,
                                                   uint32 version) noexcept = 0;

    // 단일 값 원소 배열 필드(wire ValueArray)를 연다(Docs/Design/ReflectionFoundations.ko.md
    // 5.6절): wire가 ValueArray가 아니면 TypeMismatch, 그 뒤 OpenValue로 연 payload를
    // ValueArrayReader::Open(elementWire, maxCount, shape)에 넘긴다(그 검사 순서·청구 — 1단계는
    // 그래프 공유 예산, 2단계는 무제한). codec은 maxCount에 MaxArrayElements를 넘긴다(로드는 필드별
    // maxElements를 걸지 않는다).
    //
    // 사용 예:
    //   auto array = context.OpenValueArray(field, static_cast<uint32>(WireType::U32),
    //                                       MaxArrayElements);
    //   if (!array) { return Result<void>::Failure(array.Error()); }
    Result<ValueArrayReader> OpenValueArray(const FieldView& field, uint32 elementWire,
                                            uint32 maxCount, WireValueShape shape = {}) noexcept {
        if (field.wireType != static_cast<uint32>(WireType::ValueArray)) {
            return Result<ValueArrayReader>::Failure(ErrorCode::TypeMismatch);
        }

        auto opened = OpenValue(field);
        if (!opened) {
            return Result<ValueArrayReader>::Failure(opened.Error());
        }

        return ValueArrayReader::Open(Move(opened.Value()), elementWire, maxCount, shape);
    }

    // struct 원소 배열 필드(wire RecordList)를 연다. wire가 RecordList가 아니면 TypeMismatch다.
    // 항목 schema는 itemSchema, 항목 버전 범위는 (1, UINT32_MAX)이며(버전 판정은 codec) 목록 한도는
    // 직렬화기가 정한다(그래프는 {MaxArrayElements, maxValueBytes, maxValueBytes − 12} — 저장
    // 한도와 같다). 1단계는 맨 위(없으면 객체) Record의 RecordListReader::OpenField(공유 예산 —
    // 색인만 청구)이고 맨 위 Record가 없으면 2단계 단독 Open 몫(payload byte)을 미리 청구한다.
    // 2단계는 스택이 비면 같은 한도의 단독 Open(무제한 operation·work), 있으면 맨 위의 OpenField다.
    // 반환 reader는 이 필드 payload를 빌리며 호출자가 OpenListItem을 다 쓸 때까지 쥔다.
    virtual Result<RecordListReader> OpenRecordList(const FieldView& field,
                                                    TypeId itemSchema) noexcept = 0;

    // list의 index번째 항목 Record를 Record 스택에 넣는다. 1단계는 OpenItem(index, 남은 그래프 필드
    // 누계)과 AddFields(항목 TLV 수), 2단계는 OpenItem(index, maxTotalFields)다. 오류는
    // OpenRecord와 같다(범위 밖 index·버전 0 InvalidArgument, schema 불일치 TypeMismatch, 필드 수
    // 초과 CapacityExceeded).
    virtual Result<RecordScope> OpenListItem(RecordListReader& list, usize index) noexcept = 0;

    // 2단계 codec이 지금 적용하는 StructRefArray 원소의 위치다(StructElementScope 안 — 없으면
    // nullptr). 원소 안 참조 멤버 쓰기(detail::SetStructMemberReference)가 이것이 있으면
    // ConstructionContext::SetElementReference(컨테이너 기준 범위 검사)로 보낸다.
    const detail::StructElementLocation* StructElement() const noexcept {
        return element_;
    }

    // struct의 옛 버전 Record를 현재 버전으로 올린다. 1단계는 로드 옵션 override(Unsupported면
    // 다음)·타입이 등록한 마이그레이션 순으로 부르고(둘 다 없으면 Unsupported) 결과를 발생 순서대로
    // 보관해 view를 돌려준다. 2단계는 같은 순서의 보관본을 대조해 돌려준다(어긋나면 InvalidArgument
    // — 계약 위반·rollback).
    virtual Result<Span<const uint8>> MigrateStruct(TypeId type, uint32 fromVersion,
                                                    uint32 toVersion,
                                                    Span<const uint8> record) noexcept = 0;

    // 이 struct ID를 1단계에서 관찰하는지다(로드 시작 때 직렬화기가 정한 목록).
    bool IsObservedStruct(TypeId type) const noexcept {
        if (!Recording()) {
            return false;
        }

        usize low = 0;
        usize high = observed_.Size();
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            const TypeId id = observed_[middle];
            if (id == type) {
                return true;
            }

            if (id.high < type.high || (id.high == type.high && id.low < type.low)) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }

        return false;
    }

    // 지금 위치의 관찰 대상 struct 값을 hook에 넘긴다.
    Result<void> ObserveStruct(TypeId type, const void* nativeKey, const void* value) noexcept {
        FieldCodecHooks* hooks = Hooks();
        if (!Recording() || !hooks || !Current()) {
            return Result<void>::Success();
        }

        return hooks->ObserveStruct(*Current(), type, nativeKey, value);
    }

    // 강한 참조 하나를 읽는다(ReferenceWireType() 형식). 1단계는 token 범위를 검사하고 기대 형식과
    // 함께 기록한다.
    virtual Result<DecodedReference>
    ReadReference(ArchiveReader& reader, const ReferenceExpectation& expectation) noexcept = 0;

    // weak 참조 하나를 읽는다(리플렉션 B9d1 — Docs/Design/ReflectionExtensions.ko.md 5.1절,
    // ReferenceWireType() 형식). 같은 그래프 안 노드 번호만 받는다: 외부 참조 번호(graph v2의
    // 0x80000000 | i)는 writer가 쓰지 않으므로 InvalidArgument이고, 노드 번호의 대상은 강한 참조와
    // 같은 기대 형식 규칙으로 판정한다(1단계가 기록해 모든 노드를 해석한 뒤 TypeMismatch). 1단계는
    // isNull만 정확하고, 2단계는 만든 노드 객체 handle이다. weak 간선은 도달 검사·GC·관찰자의
    // 대상이 아니다. 기본은 Unsupported다(weak 복원을 지원하지 않는 문맥).
    //
    // 사용 예(생성 codec이 부르는 형태 — ReadValueWire의 WeakObjectReference 갈래):
    //   auto decoded = context.ReadWeakReference(reader, {type.type, staticNative});
    //   if (!decoded) {
    //       return Result<void>::Failure(decoded.Error());
    //   }
    //
    //   out.reference = decoded.Value();   // 2단계면 handle로 WeakRef<T>를 만든다
    virtual Result<DecodedReference>
    ReadWeakReference(ArchiveReader& reader, const ReferenceExpectation& expectation) noexcept {
        (void)reader;
        (void)expectation;
        return Result<DecodedReference>::Failure(ErrorCode::Unsupported);
    }

    // 복원 1단계(기록하는 단계)가 내용 규칙(ValidateNameFieldText)을 지난 Name 표시 문자열 하나를
    // 방문 순서대로 기록한다(Docs/Design/ReflectionExtensions.ko.md 4.1절). 1단계는 ObjectRegistry·
    // pool에 닿지 않으므로 intern은 2a(CreateObjects 사전 확인)가 대상 Registry의 pool로 일괄 하고,
    // 1단계가 그 몫(byte + 상수)을 여기서 미리 청구한다. text는 복원 입력(또는 1단계가 보관한
    // 마이그레이션 결과)을 빌린 view이며 Stage 수명 동안 유효하다. 값 하나에 한 번이다(None
    // 문자열도 기록한다 — 2a가 기본 None으로 둔다). 기본은 Unsupported다(Name 복원을 지원하지 않는
    // 문맥).
    virtual Result<void> CollectName(StringView text) noexcept {
        (void)text;
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 복원 2단계가 1단계와 같은 방문 순서로 Name 토큰 하나를 받는다(2a가 intern한 토큰 — 대상
    // Registry pool의 것이고 내용 검사를 지났다). text는 2단계가 다시 읽은 표시 문자열이며 1단계
    // 기록과 byte가 다르거나 기록이 남지 않았으면 두 단계의 방문 순서가 어긋난 계약 위반이라
    // InvalidArgument다. 기본은 Unsupported다.
    virtual Result<Name> TakeName(StringView text) noexcept {
        (void)text;
        return Result<Name>::Failure(ErrorCode::Unsupported);
    }

    // 복원 1단계가 null이 아닌 저장 ClassRef 값 하나(stored)의 판정을 방문 순서대로 기록한다(nulled
    // = 미등록 클래스를 decode 정책 ReportAndNull로 null로 바꿨다 — Docs/Design/
    // ReflectionExtensions.ko.md 5.2절). 2단계는 TypeRegistry를 다시 조회하지 않고 이 기록을 쓰므로
    // 두 단계 사이에 등록이 바뀌어도 결과는 1단계 판정·진단과 같다. 기록 몫은 구현이 1단계에
    // 청구한다. 기본은 Unsupported다(ClassRef 복원을 지원하지 않는 문맥).
    virtual Result<void> CollectClassReference(TypeId stored, bool nulled) noexcept {
        (void)stored;
        (void)nulled;
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 복원 2단계가 1단계와 같은 방문 순서로 ClassRef 판정 하나를 받는다: 1단계 판정 뒤의 값(stored
    // 그대로이거나 null). stored가 기록과 다르거나 기록이 남지 않았으면 두 단계의 방문 순서가
    // 어긋난 계약 위반이라 InvalidArgument다. 기본은 Unsupported다.
    virtual Result<TypeId> TakeClassReference(TypeId stored) noexcept {
        (void)stored;
        return Result<TypeId>::Failure(ErrorCode::Unsupported);
    }

protected:
    FieldDecodeContext(const FieldDecodePolicy& policy, FieldCodecHooks* hooks,
                       uint32 referenceWire) noexcept
        : FieldCodecContext(hooks, referenceWire), policy_(policy) {
    }

    ~FieldDecodeContext() {
        if (depth_ != 0) {
            platform::Fatal("FieldDecodeContext destroyed with open record scopes");
        }
    }

    // Record 스택이 비었을 때의 값 reader다(1단계는 객체 Record, 2단계는 보관한 FieldView).
    virtual Result<ArchiveReader> OpenTopLevelValue(const FieldView& field) noexcept = 0;

    // 직렬화기가 연 reader를 Record 스택에 넣는다(B5 RecordList 항목 reader도 같은 경로).
    Result<RecordScope> PushRecord(RecordReader&& reader) noexcept;

    // 관찰할 struct ID다(ID 순으로 정렬된 span — 로드 동안 유효해야 한다).
    void SetObservedStructs(Span<const TypeId> sorted) noexcept {
        observed_ = sorted;
    }

    // MarkPresent가 부르는 present 판정 hook이다(frame = 파일에 있는 필드·멤버의 위치, 호출 동안만
    // 유효). 기본은 아무것도 하지 않는다 — A5가 override 집합을 채울 때 직렬화기 문맥이 구현한다
    // (Docs/Design/ObjectFoundations.ko.md 4절, 공개 hook 여부는 A5가 정한다).
    virtual Result<void> NotePresent(const FieldCodecFrame& frame) noexcept {
        (void)frame;
        return Result<void>::Success();
    }

private:
    friend class RecordScope;
    friend class StructElementScope;

    static constexpr uint32 MaxRecordDepth = MaxStructDepth + 1;

    RecordReader& Top() noexcept {
        return *slots_[depth_ - 1];
    }

    FieldDecodePolicy policy_;
    Span<const TypeId> observed_{};
    const detail::StructElementLocation* element_ = nullptr;
    alignas(RecordReader) uint8 storage_[MaxRecordDepth * sizeof(RecordReader)];
    // placement new가 돌려준 포인터다(저장소 주소를 다시 해석하지 않는다).
    RecordReader* slots_[MaxRecordDepth] = {};
    uint32 depth_ = 0;
};

// FieldDecodeContext의 Record 스택 한 칸을 쥔다(RAII). 이동할 수 있고(이동된 원본은 아무것도 하지
// 않는다) 들어간 순서의 역순으로 끝나야 한다(아니면 Fatal). Close로 일찍 닫을 수 있다(마이그레이션
// 전에 원본 Record를 닫는다).
//
// 사용 예(struct codec):
//   auto scope = context.OpenRecord(field, structId, 1, NumericLimits<uint32>::Max());
//   if (!scope) { return Result<void>::Failure(scope.Error()); }
//   RecordReader& record = *context.CurrentRecord();
class [[nodiscard]] RecordScope {
public:
    RecordScope(RecordScope&& other) noexcept : context_(other.context_), depth_(other.depth_) {
        other.context_ = nullptr;
    }

    RecordScope& operator=(RecordScope&&) = delete;
    RecordScope(const RecordScope&) = delete;
    RecordScope& operator=(const RecordScope&) = delete;

    ~RecordScope() {
        Close();
    }

    void Close() noexcept {
        if (!context_) {
            return;
        }

        if (context_->depth_ != depth_) {
            platform::Fatal("RecordScope ended out of order");
        }

        context_->Top().~RecordReader();
        --context_->depth_;
        context_ = nullptr;
    }

private:
    friend class FieldDecodeContext;

    RecordScope(FieldDecodeContext* context, uint32 depth) noexcept
        : context_(context), depth_(depth) {
    }

    FieldDecodeContext* context_;
    uint32 depth_;
};

// 2단계 codec이 StructRefArray 원소 하나를 적용하는 동안 그 위치를 문맥에 세운다(RAII). 그 안의
// 참조 멤버 쓰기는 원소 위치로 범위 검사된다(FieldDecodeContext::StructElement). 이미 원소 위치가
// 있으면 Fatal이다(StructRefArray 원소는 Registry 컨테이너를 담지 않으므로 겹치지 않는다). 위치는
// 이 scope가 복사해 쥐며 scope는 문맥보다 먼저 끝나야 한다.
//
// 사용 예(배열 codec 2단계):
//   StructElementScope element(context, detail::StructRefArrayAccess::Location(array, index));
//   // 원소 struct 해석 — 참조 멤버는 SetElementReference로 간다
class StructElementScope {
public:
    [[nodiscard]] StructElementScope(FieldDecodeContext& context,
                                     const detail::StructElementLocation& location) noexcept
        : context_(context), location_(location) {
        if (context.element_ != nullptr) {
            platform::Fatal("StructElementScope nested inside another struct element");
        }

        context.element_ = &location_;
    }

    ~StructElementScope() {
        if (context_.element_ != &location_) {
            platform::Fatal("StructElementScope ended out of order");
        }

        context_.element_ = nullptr;
    }

    StructElementScope(const StructElementScope&) = delete;
    StructElementScope& operator=(const StructElementScope&) = delete;

private:
    FieldDecodeContext& context_;
    detail::StructElementLocation location_;
};

inline Result<RecordScope> FieldDecodeContext::PushRecord(RecordReader&& reader) noexcept {
    if (depth_ == MaxRecordDepth) {
        return Result<RecordScope>::Failure(ErrorCode::CapacityExceeded);
    }

    slots_[depth_] = ::new (storage_ + depth_ * sizeof(RecordReader)) RecordReader(Move(reader));
    ++depth_;
    return Result<RecordScope>::Success(RecordScope(this, depth_));
}

inline Span<const PathStep>
FieldCodecContext::BuildPath(PathStep (&steps)[MaxPropertyPathSteps]) const noexcept {
    usize depth = 0;
    for (const FieldCodecFrame* frame = current_; frame; frame = frame->parent) {
        ++depth;
    }

    if (depth > MaxPropertyPathSteps) {
        platform::Fatal("FieldCodecContext path deeper than MaxPropertyPathSteps");
    }

    usize position = depth;
    for (const FieldCodecFrame* frame = current_; frame; frame = frame->parent) {
        --position;
        steps[position].fieldId = frame->field->id;
        steps[position].indexed = frame->index != FieldCodecFrame::NoIndex;
        steps[position].index = steps[position].indexed ? frame->index : 0;
    }

    return Span<const PathStep>(steps, depth);
}

inline Result<void> FieldCodecContext::Report(FieldDiagnosticKind kind, int64 value,
                                              TypeId valueType) noexcept {
    if (!recording_ || !hooks_) {
        return Result<void>::Success();
    }

    PathStep steps[MaxPropertyPathSteps];
    FieldDiagnostic diagnostic;
    diagnostic.kind = kind;
    diagnostic.objectType = objectType_;
    diagnostic.objectIndex = objectIndex_;
    diagnostic.path = BuildPath(steps);
    diagnostic.field = current_ ? current_->field->id : Guid{};
    diagnostic.valueType = valueType;
    diagnostic.value = value;
    return hooks_->Report(diagnostic);
}

inline Result<void> FieldCodecContext::CollectSoftReference(AssetId asset, StringView objectPath,
                                                            TypeId expectedType) noexcept {
    if (!recording_ || !hooks_ || (asset.IsNull() && objectPath.ByteCount() == 0)) {
        return Result<void>::Success();
    }

    PathStep steps[MaxPropertyPathSteps];
    SoftReferenceInfo info;
    info.asset = asset;
    info.objectPath = objectPath;
    info.expectedType = expectedType;
    info.effectiveFlags = EffectiveFlags();
    info.objectIndex = objectIndex_;
    info.path = BuildPath(steps);
    return hooks_->CollectSoftReference(info);
}

} // namespace rocks
