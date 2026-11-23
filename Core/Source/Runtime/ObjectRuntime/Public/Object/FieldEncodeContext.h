#pragma once

#include <Object/FieldCodecContext.h>

// 필드 codec 문맥 사슬의 셋째 조각으로 저장 문맥(FieldEncodeContext)이다(Docs/Design/
// ReflectionFoundations.ko.md 6.4절·Docs/Design/ObjectFoundations.ko.md 4절). 저장 정책 판정,
// Required 검사와 Required 실효 값 검사 모드, 강한·weak 참조 쓰기, struct 치환 hook, 외부 참조
// Admit 질의가 있다. Object/FieldCodec.h로 받는다.
namespace rocks {

class Object;

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

    // 지금 위치(필드·struct 멤버)를 쓰지 않는지다: 유효 플래그의 제외(IsExcluded)이거나 포함 필터
    // 밖(IsOutsideInclude — policy.includeFlags, 리플렉션 B9h1)이다. 직렬화기와 struct codec이
    // 위치에 들어간 뒤 부른다.
    bool ExcludesCurrent() const noexcept {
        return IsExcluded(EffectiveFlags()) || IsOutsideInclude(policy_.includeFlags);
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

    // Registry 밖 원본 객체를 읽는 문맥이면 그 객체다(아니면 nullptr — 클래스 기본값 레코드의 임시
    // 인스턴스, Docs/Design/RuntimeClassMetadata.ko.md 'A5a 레코드'). 생성 encode thunk
    // (detail::EncodeGeneratedField·EncodeBitfieldField)는 이것이 있으면 Registry·staged 대신 이
    // 객체에서 멤버를 읽고(같은 EncodeMemberValue — 같은 byte), 수동 함수표의 범용 encode는
    // Unsupported다. 이때 thunk가 받는 registry는 null이다.
    virtual const Object* RawSource() const noexcept {
        return nullptr;
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

    // 지금 위치의 null이 아닌 Soft 참조 값의 asset 대신 저장할 asset이다(hook이 없거나 검사
    // 모드면 asset — FieldCodecHooks::SubstituteSoftReference). hook이 null을 돌려주면
    // InvalidArgument다.
    Result<AssetId> SubstituteSoftReference(AssetId asset, StringView objectPath,
                                            TypeId expectedType) noexcept {
        FieldCodecHooks* hooks = Hooks();
        if (!hooks || !Current() || IsRequiredCheck()) {
            return Result<AssetId>::Success(asset);
        }

        PathStep steps[MaxPropertyPathSteps];
        SoftReferenceInfo info;
        info.asset = asset;
        info.objectPath = objectPath;
        info.expectedType = expectedType;
        info.effectiveFlags = EffectiveFlags();
        info.objectIndex = ObjectIndex();
        info.path = BuildPath(steps);
        auto substituted = hooks->SubstituteSoftReference(info);
        if (substituted && substituted.Value().IsNull()) {
            return Result<AssetId>::Failure(ErrorCode::InvalidArgument);
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

} // namespace rocks
