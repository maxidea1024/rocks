#pragma once

#include <Meta/PropertyPathSchema.h>
#include <Object/AssetId.h>
#include <Object/ObjectHandle.h>

// 직렬화 관찰 hook 묶음(FieldCodecHooks)과 hook이 받는 진단·소프트 참조·외부 참조 정보다
// (설계: Docs/Design/ReflectionFoundations.ko.md 6.4절, ObjectFoundations.ko.md 5.3절).
// 필드 codec 문맥(Object/FieldCodec.h)이 이 헤더를 include하며, 저장·복원 옵션에 hook을 주는
// 호출자는 FieldCodec.h 또는 ObjectSerialization/ObjectGraph.h로 받는다.
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
    // 저장할 배열·Map·Set의 원소 수(value)가 필드 maxElements를 넘는다(path = 그 필드 — Map·Set은
    // 리플렉션 B9f2가 같은 진단을 쓴다). 저장은 이 진단 뒤 CapacityExceeded다(로드는 필드별
    // maxElements를 걸지 않으므로 내지 않는다).
    ArrayTooLong,
    // 저장 집합 밖 참조를 외부 참조로 쓰지 않았다(path = 참조 위치, value 0 = 거부, 1 = null로 씀,
    // 2 = weak 집합 밖 null — Docs/Design/ObjectFoundations.ko.md 5.2·5.3절,
    // ReflectionExtensions.ko.md 5.1·15절). 그래프 저장의 Reject 정책, Include 거부 조건(기준
    // Outer·노드 1의 조상·다른 패키지), Admit 정책에서 AdmitExternalReference가 Reject를 고른
    // 위치(weak 포함)가 value 0 뒤 InvalidArgument이고, 강한 참조에 Null을 고른 위치는 value 1을
    // 남기고 그 위치를 null로 쓴다. weak 참조(WeakObjectRef PROPERTY)의 대상이 저장 집합 밖이면
    // value 2를 남기고 null로 쓴다(저장은 실패하지 않는다): Reject 정책, Admit에서 Null·Import(weak
    // import는 없다), Include에서 모든 노드를 encode한 뒤의 최종 집합 밖(그때 기록 순서대로 —
    // objectIndex·path는 기록할 때의 위치다). 저장 정책 UnsaveablePolicy::Exclude에서 저장 불가
    // 객체(클래스 Transient·Deprecated — 리플렉션 B9h2)를 가리키는 강한·weak 참조는 value 3
    // (Transient 클래스)·4(Deprecated 클래스)를 남기고 null로 쓴다.
    ExternalReference,
    // 복원 1단계가 읽은 ClassRef 값(ValueKind::ClassReference)의 클래스가 대상 TypeRegistry에
    // 등록되어 있지 않아 decode 정책 UnknownClassPolicy::ReportAndNull대로 null로 읽었다(valueType
    // = 저장된 클래스 TypeId, path = 그 값의 위치 — 배열이면 [f[i]], Docs/Design/
    // ReflectionExtensions.ko.md 5.2·15절). Reject 정책(기본)은 진단 없이 NotFound다.
    UnknownClassReference,
    // 복원 1단계가 클래스 Transient(상속 포함 — 리플렉션 B9h2)인 노드를 읽었다(objectType = 그 노드
    // 타입, path는 비어 있다 — 노드는 정상으로 만든다). 패키지 저장(Exclude)으로 다시 저장하면 그
    // 객체와 그 참조는 빠진다.
    TransientType,
    // 복원 1단계가 클래스 Deprecated인 노드(path 빈 것 — objectType = 노드 타입) 또는 Deprecated
    // 클래스를 가리키는 ClassRef 값(valueType = 그 클래스, path = 값 위치)을 읽었다(리플렉션 B9h2 —
    // 값·노드는 정상으로 읽는다).
    DeprecatedType,
    // 복원 1단계가 읽은 InstancedStruct 값(ValueKind::InstancedStruct — 리플렉션 B9j)의 담긴
    // struct가 대상 TypeRegistry에 등록되어 있지 않아 decode 정책
    // UnknownStructTypePolicy::ReportAndClear대로 비웠다(valueType = 저장된 struct ID, path = 그
    // 값의 위치 — 배열·Map 값이면 [f[i]], Docs/Design/ReflectionExtensions.ko.md 11절). Reject
    // 정책(기본)은 진단 없이 NotFound다.
    UnknownStructType
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

// 그래프 저장(Admit 정책)이 저장 집합 밖 참조(강한·weak) 대상을 만났을 때 hook에 주는 정보다
// (Docs/Design/ObjectFoundations.ko.md 5.3절 — FieldCodecHooks::AdmitExternalReference). path는
// 호출 동안만 유효하다.
struct ExternalReferenceInfo {
    // 대상 객체와 그 실제 등록 타입이다(대상 생존·기대 형식 검사를 지난 뒤다).
    ObjectHandle target{};
    TypeId targetType{};
    // 그 위치의 스키마 기대 타입이다(0 = Object).
    TypeId expectedType{};
    // 그 위치의 유효 플래그(PropagateFieldFlags — 배열 원소는 배열 필드의 것)와 Required 여부다.
    uint32 effectiveFlags = 0;
    bool required = false;
    // 참조를 가진 노드 위치(0부터 — local ID − 1)와 필드 경로다.
    uint32 objectIndex = 0;
    Span<const PathStep> path{};
    // Import를 고르면 이 대상이 받을 외부 번호(slot — 1부터, 첫 출현 순)다. Null·Reject면 쓰이지
    // 않는다(다음 대상이 같은 번호를 받는다). weak면 0이다(weak import는 없다).
    uint32 slot = 0;
    // weak 참조(WeakObjectRef PROPERTY — 리플렉션 B9d1) 위치인지다. weak는 같은 그래프 안만
    // 보존하므로 Null·Import 모두 그 위치를 null로 쓰고 ExternalReference 진단(value 2)을 남기며
    // 저장은 실패하지 않는다. Reject만 진단(value 0) 뒤 InvalidArgument다. weak 질의는 slot 캐시를
    // 쓰지 않는다 — 같은 대상의 출현마다(강한 참조가 먼저 Import한 대상이어도) 다시 묻는다.
    // required는 늘 거짓이다.
    bool weak = false;
};

// 외부 참조 허락 hook의 결과다(Docs/Design/ObjectFoundations.ko.md 5.3절·11절 결정 14).
enum class ExternalAdmission : uint8 {
    // 외부 참조로 쓴다(값 = 0x80000000 | slot — 같은 대상의 이후 출현은 hook 없이 같은 slot).
    Import,
    // 이 위치를 null(0)로 쓰고 ExternalReference 진단(value 1)을 남긴다(slot을 쓰지 않는다). 그
    // 위치가 Required이면 null 값에 Required 규칙을 적용한다(rejectMissingRequired면
    // RequiredMissing 진단 뒤 InvalidArgument).
    Null,
    // ExternalReference 진단(value 0) 뒤 저장이 InvalidArgument로 실패한다.
    Reject
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
// 관찰·치환(B4, Docs/Design/ReflectionFoundations.ko.md 4.6·6.4절)과 그래프 저장의 외부 참조
// 허락(AdmitExternalReference — Docs/Design/ObjectFoundations.ko.md 5.3절)도 같은 묶음이다. 배열
// 원소 struct의 Observe·Substitute에 오는 frame은 배열 필드의
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

    // 저장할 Soft 참조 값의 asset 대신 쓸 asset을 돌려준다(기본은 info.asset — 저장은 객체를
    // 바꾸지 않는다). 저장 문맥이 null이 아닌 값에만, Required 검사 모드가 아닐 때 부르고(struct
    // 치환과 같은 조건) 바뀐 asset을 쓴 뒤 수집(CollectSoftReference)도 바뀐 asset으로 한다. 객체
    // 경로는 그대로다. null asset을 돌려주면 InvalidArgument다(경로만 남은 값은 SoftObjectRef
    // 규칙이 거부한다). Map 키·Set 원소의 값도 부른다(순서는 삽입 순서라 그대로다 — 두 키를 같게
    // 만들지 않는 것은 hook의 책임이다).
    //
    // 사용 예(패키지 복제 — 원본 id를 가리키는 값을 사본 id로, AssetSystem.ko.md 7.1절 17 A2e3b):
    //   Result<AssetId> SubstituteSoftReference(const SoftReferenceInfo& info) noexcept override {
    //       return Result<AssetId>::Success(info.asset == source_ ? copy_ : info.asset);
    //   }
    virtual Result<AssetId> SubstituteSoftReference(const SoftReferenceInfo& info) noexcept {
        return Result<AssetId>::Success(info.asset);
    }

    // 복원 1단계에서 관찰할 struct ID다(로드 시작 때 한 번 읽는다 — 미등록 ID는 NotFound, 참조
    // (강한·weak)나 Name을 담는 struct는 InvalidArgument — 1단계 임시 값에는 참조 대상도 Name
    // 토큰도 만들 수 없다, StructView::ContainsStrongReference·ContainsWeakReference·ContainsName —
    // 리플렉션 B9j부터 InstancedStruct를 담은 struct도 거부한다, ContainsInstancedStruct). 반환
    // span은 그 로드 동안 유효해야 한다.
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

    // 그래프 저장(GraphExternalPolicy::Admit)이 저장 집합 밖 참조 대상을 만날 때마다 그 대상을
    // 외부 참조로 쓸지 묻는다(Docs/Design/ObjectFoundations.ko.md 5.3절). 대상 생존·기대 형식
    // 검사를 지난 뒤에 소유 스레드에서 불린다. 강한 참조는 같은 대상의 두 번째 출현부터 첫 결정이
    // Import였으면 부르지 않고(같은 slot으로 집계만 한다) Null이었으면 다시 부른다. weak 참조
    // (info.weak)는 출현마다 부르고 Import·Null 모두 null + 진단(value 2)이다 — weak를 보지 않는
    // hook이 Import를 골라도 저장은 실패하지 않는다(ReflectionExtensions.ko.md 5.1절). 실패를
    // 돌려주면 저장이 그 오류로 실패하고, 정의되지 않은 결과 값은 InvalidArgument다. 기본은
    // Reject다(외부 참조를 쓰려면 hook을 준다). 저장 중이므로 Registry·저장 중인 객체를 바꾸지
    // 않는다.
    //
    // 인자: info(대상·타입·위치·Import일 때 받을 slot — 호출 동안만 유효).
    // 반환: Import(외부 참조로 쓴다 — weak면 null + 진단 value 2), Null(그 위치를 null로 쓰고 진단
    // value 1 — weak면 value 2), Reject(진단 value 0 뒤 InvalidArgument — weak도 같다). hook이
    // 없거나 이 함수를 재정의하지 않으면 Reject라 집합 밖 weak 하나로도 저장이 실패한다(weak를
    // null로 받으려면 아래 예처럼 info.weak에 Null을 준다).
    //
    // 사용 예(A1 writer — 같은 패키지 밖 객체만 import로 쓴다, weak는 같은 그래프 밖이면 null):
    //   Result<ExternalAdmission> AdmitExternalReference(
    //       const ExternalReferenceInfo& info) noexcept override {
    //       if (info.weak) {
    //           return Result<ExternalAdmission>::Success(ExternalAdmission::Null);
    //       }
    //
    //       auto outermost = registry_.GetOutermost(access_, info.target);
    //       if (!outermost || outermost.Value() == package_) {
    //           return Result<ExternalAdmission>::Success(ExternalAdmission::Reject);
    //       }
    //
    //       return Result<ExternalAdmission>::Success(ExternalAdmission::Import);
    //   }
    virtual Result<ExternalAdmission>
    AdmitExternalReference(const ExternalReferenceInfo& info) noexcept {
        (void)info;
        return Result<ExternalAdmission>::Success(ExternalAdmission::Reject);
    }

protected:
    FieldCodecHooks() noexcept = default;
    FieldCodecHooks(const FieldCodecHooks&) noexcept = default;
    FieldCodecHooks& operator=(const FieldCodecHooks&) noexcept = default;
    ~FieldCodecHooks() = default;
};

} // namespace rocks
