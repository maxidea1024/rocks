#pragma once

#include <Meta/TypeSchema.h>
#include <Object/AssetId.h>
#include <Object/ObjectHandle.h>

// 직렬화 관찰 hook 묶음(FieldCodecHooks)과 hook이 받는 진단·소프트 참조 정보다
// (설계: Docs/Design/ReflectionFoundations.ko.md 6.4절). 필드 codec 문맥(Object/FieldCodec.h)이 이
// 헤더를 include하며, 저장·복원 옵션에 hook을 주는 호출자는 FieldCodec.h 또는
// ObjectSerialization/ObjectGraph.h로 받는다.
namespace rocks {

enum class FieldDiagnosticKind : uint8 {
    // field는 모르는 필드 ID다.
    UnknownField,
    // Required 위치의 값이 null이다(배열이면 원소 — path는 [f[i]]). 저장은 rejectMissingRequired일
    // 때 이 진단 뒤 InvalidArgument이고, 복원은 거부하지 않고 적용 뒤·Commit 전에 staged 객체의
    // 실효 값(파일 값이든 기준 값이든)으로 한 번 보고한다.
    RequiredMissing,
    // value는 저장된 값, valueType은 enum ID다.
    UndeclaredEnumValue,
    // value는 저장 버전이다(객체 Record면 path가 비어 있다).
    NewerVersion,
    // 저장할 배열의 원소 수(value)가 필드 maxElements를 넘는다(path = 배열 필드). 저장은 이 진단
    // 뒤 CapacityExceeded다(로드는 필드별 maxElements를 걸지 않으므로 내지 않는다).
    ArrayTooLong,
    // 저장 집합 밖 강한 참조를 쓰지 못했다(path = 참조 위치, value 0 = 거부). 그래프 저장의 Reject
    // 정책과 Include 거부 조건(기준 Outer·노드 1의 조상·다른 패키지 — Docs/Design/
    // ObjectFoundations.ko.md 5.2절)이 이 진단 뒤 InvalidArgument다. 외부 참조 허락 hook의
    // 결과(거부 0·null 1)는 A0-Oe가 더한다.
    ExternalReference
};

// 진단 하나다. objectIndex는 그래프 안 노드 위치(0부터 — local ID − 1 = export index − 1), path는
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

// 직렬화 관찰 hook 묶음이다. 기본 구현은 아무것도 하지 않는다. 실패를 돌려주면 그 저장·복원이
// 실패한다(진단 유실로 읽기 전용 판정을 놓치지 않게). 저장·복원이 실패하면 그 호출에서 받은
// 진단·수집은 무효이고 호출자가 버린다. hook은 소유 객체를 Borrow한 상태에서 불리므로(복원의
// Required 실효 값 진단은 staged 객체라 Borrow 없이 불리고 ConstructionContext의 검사 중 표시가
// 값을 지킨다) Registry와 입력 byte를 바꾸지 않는다. 복원의 RequiredMissing은 적용 뒤 Required
// 검사에서 한 번, 그 밖의 진단·Soft 수집은 1단계에서 한 번 온다(Soft 수집은 파일 값만). 단계형
// 복원(ObjectGraphStage)의 1단계 hook(진단·Soft 수집·struct 관찰·ObservedStructs)은 Validate를 부른
// 스레드(worker일 수 있다)에서, RequiredMissing은 소유 스레드에서 불린다 — worker 검증에 주는
// hook은 그 스레드에서 안전해야 하고 다른 스레드와 동시에 쓰이면 스레드 안전해야 한다. struct
// 관찰·치환(B4, Docs/Design/ReflectionFoundations.ko.md 4.6·6.4절)도 같은 묶음이고, A0-O가 외부
// 참조 보고를 더한다. 배열 원소 struct의 Observe·Substitute에 오는 frame은 배열 필드의
// frame이다(field의 value.kind == Array, structure = 원소 struct, index = 원소 index — 원소마다 새
// frame이 아니다).
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

} // namespace rocks
