#pragma once

#include <Object/AssetId.h>
#include <Object/RuntimeTypeOps.h>
#include <Object/TypeLease.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>

// 필드 codec의 문맥·정책·hook이다(Docs/Design/ReflectionFoundations.ko.md 6.1–6.4절). 생성 codec
// thunk(RuntimeFieldOps::encode·decode)는 이 문맥으로만 참조 wire·진단·Soft 수집·예산에 닿으므로,
// 그래프 형식(graph v1 local ID, graph v2 import 비트)이나 저장 집합이 바뀌어도 생성 코드를 다시
// 만들지 않는다. 직렬화기(ObjectSerialization)가 문맥을 구현하고 필드마다 FieldScope로 위치를
// 정한다. 이 헤더는 생성 헤더와 직렬화기만 include한다.
namespace rocks {

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

// 복원 정책이다. 기본은 미선언 enum·높은 버전·누락 필드 거부, 모르는 필드는 진단 후 건너뛰기다.
// allowMissingFields면 Record에 없는 필드는 native 기본값으로
// 두고, allowMissingFlags의 비트(쿠킹 로드의 EditorOnly)를 가진 필드만 누락을 허용할 수도 있다.
struct FieldDecodePolicy {
    UndeclaredEnumPolicy undeclaredEnum = UndeclaredEnumPolicy::Reject;
    NewerVersionPolicy newerVersion = NewerVersionPolicy::Reject;
    UnknownFieldPolicy unknownField = UnknownFieldPolicy::SkipAndReport;
    bool allowMissingFields = false;
    uint32 allowMissingFlags = 0;
};

enum class FieldDiagnosticKind : uint8 {
    // field는 모르는 필드 ID다.
    UnknownField,
    // Required 필드가 null이거나 저장 데이터에 없다.
    RequiredMissing,
    // value는 저장된 값, valueType은 enum ID다.
    UndeclaredEnumValue,
    // value는 저장 버전이다(객체 Record면 path가 비어 있다).
    NewerVersion
};

// 진단 하나다. objectIndex는 그래프 안 객체 위치(graph v1 local ID − 1, v2 export index), path는
// 필드 경로(호출 동안만 유효)다.
struct FieldDiagnostic {
    FieldDiagnosticKind kind = FieldDiagnosticKind::UnknownField;
    TypeId objectType{};
    uint32 objectIndex = 0;
    Span<const PathStep> path{};
    Guid field{};
    TypeId valueType{};
    int64 value = 0;
};

// non-null 소프트 참조 하나다(A1 Dependencies 수집). view와 path는 호출 동안만 유효하다.
struct SoftReferenceInfo {
    AssetId asset{};
    StringView objectPath{};
    TypeId expectedType{};
    uint32 effectiveFlags = 0;
    uint32 objectIndex = 0;
    Span<const PathStep> path{};
};

struct FieldCodecFrame;
class RecordScope;

// 직렬화 관찰 hook 묶음이다. 기본 구현은 아무것도 하지 않는다. 실패를 돌려주면 그 저장·복원이
// 실패한다(진단 유실로 읽기 전용 판정을 놓치지 않게). 저장·복원이 실패하면 그 호출에서 받은
// 진단·수집은 무효이고 호출자가 버린다. hook은 소유 객체를 Borrow한 상태에서 불리므로 Registry와
// 입력 byte를 바꾸지 않는다. 복원의 진단·수집은 1단계에서 한 번만 온다. struct 관찰·치환(B4,
// Docs/Design/ReflectionFoundations.ko.md 4.6·6.4절)도 같은 묶음이고, A0-O가 외부 참조 보고를
// 더한다.
//
// 사용 예:
//   struct Collect final : FieldCodecHooks {
//       Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override { ... }
//   } hooks;
//   ObjectGraphLoadOptions options;
//   options.policy.undeclaredEnum = UndeclaredEnumPolicy::ReportAndDefault;
//   options.hooks = &hooks;
class FieldCodecHooks {
public:
    virtual Result<void> Report(const FieldDiagnostic&) noexcept {
        return Result<void>::Success();
    }

    virtual Result<void> CollectSoftReference(const SoftReferenceInfo&) noexcept {
        return Result<void>::Success();
    }

    // 복원 1단계에서 관찰할 struct ID다(로드 시작 때 한 번 읽는다 — 미등록 ID는 NotFound, 참조를
    // 담는 struct는 InvalidArgument). 반환 span은 그 로드 동안 유효해야 한다.
    virtual Span<const TypeId> ObservedStructs() noexcept {
        return {};
    }

    // 관찰 대상 struct 값 하나를 검증과 같은 한 번의 해석으로 채운 임시 값으로 받는다(value는 호출
    // 동안만 유효한 nativeKey 타입의 값). 실패하면 객체를 만들기 전에 로드가 실패한다.
    virtual Result<void> ObserveStruct(const FieldCodecFrame&, TypeId, const void* nativeKey,
                                       const void* value) noexcept {
        (void)nativeKey;
        (void)value;
        return Result<void>::Success();
    }

    // 저장할 struct 값 대신 쓸 값을 돌려준다(기본은 in — 저장은 객체를 바꾸지 않는다). 반환값은 그
    // struct 값의 encode가 끝날 때까지 유효한 같은 native 타입의 값이어야 하고 null은
    // InvalidArgument다. 참조를 담는 struct에는 부르지 않는다.
    virtual Result<const void*> SubstituteStruct(const FieldCodecFrame&, TypeId,
                                                 const void* nativeKey, const void* in) noexcept {
        (void)nativeKey;
        return Result<const void*>::Success(in);
    }

protected:
    FieldCodecHooks() noexcept = default;
    FieldCodecHooks(const FieldCodecHooks&) noexcept = default;
    FieldCodecHooks& operator=(const FieldCodecHooks&) noexcept = default;
    ~FieldCodecHooks() = default;
};

// 지금 처리하는 필드 위치 하나다. field는 그 필드(struct 멤버면 멤버), declaringType은 그 필드를
// 선언한 클래스·struct, effectiveFlags는 PropagateFieldFlags로 전파된 플래그, enumeration은 Enum
// 필드의 등록된 enum 스키마(그 밖은 nullptr), structure는 Struct 필드의 등록 struct(그 밖은 빈
// view), index는 배열 원소 index(아니면 NoIndex — B5 원소도 새 프레임이 아니라 이 칸을 쓴다)다.
struct FieldCodecFrame {
    static constexpr uint32 NoIndex = ~0u;

    const FieldSchema* field = nullptr;
    TypeId declaringType{};
    uint32 effectiveFlags = 0;
    const EnumSchema* enumeration = nullptr;
    StructView structure{};
    uint32 index = NoIndex;
    const FieldCodecFrame* parent = nullptr;
};

// 강한 참조의 기대 형식이다. schemaType은 스키마의 기대 타입(0 = Object), staticNative는 생성
// thunk의 정적 대상 타입 NativeTypeKey<T>()(Object거나 모르면 nullptr)다. 대상은 둘 다를 상속
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

    // 강한 참조 필드의 wire tag다(graph v1 = U64 local ID, v2 = U32 + import 비트). 참조 필드의
    // encode는 이 값을 돌려주고 decode는 저장 tag를 이것과 비교한다.
    uint32 ReferenceWireType() const noexcept {
        return referenceWire_;
    }

    // 진단·수집을 기록하는 단계인지다(복원 2단계는 거짓 — 1단계가 이미 판정했다).
    bool IsRecording() const noexcept {
        return recording_;
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

        PathStep steps[33];
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

    // 값 임시 저장에 쓰는 할당기다(저장·1단계는 예산을 청구한다).
    virtual Allocator& Scratch() noexcept = 0;

    // 중첩 Record(struct 값)의 TLV 수를 그래프 필드 누계(maxTotalFields)에 더한다. 복원 2단계는
    // 세지 않는다.
    virtual Result<void> AddFields(usize count) noexcept = 0;

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

    bool Recording() const noexcept {
        return recording_;
    }

    FieldCodecHooks* Hooks() const noexcept {
        return hooks_;
    }

    // 프레임 사슬로 경로를 만든다(바깥 → 안 순서). 반환 span은 steps를 가리킨다.
    Span<const PathStep> BuildPath(PathStep (&steps)[33]) const noexcept;

private:
    friend class FieldScope;

    FieldCodecHooks* hooks_;
    const FieldCodecFrame* current_ = nullptr;
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
        : context_(context) {
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

        context_.current_ = frame_.parent;
    }

    FieldScope(const FieldScope&) = delete;
    FieldScope& operator=(const FieldScope&) = delete;

private:
    FieldCodecContext& context_;
    FieldCodecFrame frame_;
};

// 저장 문맥이다(그래프 저장이 구현한다).
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

    // 지금 위치가 Required이고 값이 null이며 rejectMissingRequired면 RequiredMissing을 보고하고
    // InvalidArgument다. 그 밖은 성공이다.
    Result<void> CheckRequired(bool isNull) noexcept {
        if (!isNull || !policy_.rejectMissingRequired ||
            !HasFlag(EffectiveFlags(), FieldFlags::Required)) {
            return Result<void>::Success();
        }

        if (auto reported = Report(FieldDiagnosticKind::RequiredMissing); !reported) {
            return reported;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 강한 참조 하나를 쓴다: Required null 검사 뒤 대상을 expectation으로 검사하고(TypeMismatch)
    // ReferenceWireType() 형식으로 쓴다.
    Result<void> WriteReference(ArchiveWriter& out, ObjectHandle target,
                                const ReferenceExpectation& expectation) noexcept {
        if (auto required = CheckRequired(target == ObjectHandle{}); !required) {
            return required;
        }

        return WriteReferenceWire(out, target, expectation);
    }

    // 중첩 Record(struct 값)의 한도다(그래프 옵션에서 — 전체 크기는 필드 값 한도, 값 하나도 같은
    // 값 한도).
    virtual RecordLimits NestedRecordLimits() const noexcept = 0;

    // 지금 위치의 struct 값 대신 저장할 값이다(hook이 없으면 value). null은 InvalidArgument다.
    Result<const void*> SubstituteStruct(TypeId type, const void* nativeKey,
                                         const void* value) noexcept {
        FieldCodecHooks* hooks = Hooks();
        if (!hooks || !Current()) {
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

    ~FieldEncodeContext() = default;

    virtual Result<void> WriteReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                            const ReferenceExpectation& expectation) noexcept = 0;

private:
    FieldEncodePolicy policy_;
};

// 복원 문맥이다(그래프 복원이 1단계·2단계로 구현한다).
class FieldDecodeContext : public FieldCodecContext {
public:
    const FieldDecodePolicy& Policy() const noexcept {
        return policy_;
    }

    // Record에 없는 이 필드를 native 기본값으로 둘 수 있는지다.
    bool IsMissingAllowed(uint32 effectiveFlags) const noexcept {
        return policy_.allowMissingFields || (effectiveFlags & policy_.allowMissingFlags) != 0;
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

    // 지금 위치가 Required이고 값이 null이면 RequiredMissing을 보고한다(거부하지 않는다 — 기존
    // 필드에 Required를 붙여도 null을 가진 데이터를 열어 고칠 수 있게). 필드 검증이 끝난 뒤 부른다.
    Result<void> ReportIfRequiredNull(bool isNull) noexcept {
        if (!isNull || !HasFlag(EffectiveFlags(), FieldFlags::Required)) {
            return Result<void>::Success();
        }

        return Report(FieldDiagnosticKind::RequiredMissing);
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

private:
    friend class RecordScope;

    static constexpr uint32 MaxRecordDepth = MaxStructDepth + 1;

    RecordReader& Top() noexcept {
        return *slots_[depth_ - 1];
    }

    FieldDecodePolicy policy_;
    Span<const TypeId> observed_{};
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
//   auto scope = context.OpenRecord(field, structId, 1, ~0u);
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

inline Result<RecordScope> FieldDecodeContext::PushRecord(RecordReader&& reader) noexcept {
    if (depth_ == MaxRecordDepth) {
        return Result<RecordScope>::Failure(ErrorCode::CapacityExceeded);
    }

    slots_[depth_] = ::new (storage_ + depth_ * sizeof(RecordReader)) RecordReader(Move(reader));
    ++depth_;
    return Result<RecordScope>::Success(RecordScope(this, depth_));
}

inline Span<const PathStep> FieldCodecContext::BuildPath(PathStep (&steps)[33]) const noexcept {
    usize depth = 0;
    for (const FieldCodecFrame* frame = current_; frame; frame = frame->parent) {
        ++depth;
    }

    if (depth > 33) {
        platform::Fatal("FieldCodecContext path deeper than 33 steps");
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

    PathStep steps[33];
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

    PathStep steps[33];
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
