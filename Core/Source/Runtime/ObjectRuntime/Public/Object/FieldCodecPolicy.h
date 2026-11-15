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

// 필드 codec 문맥 사슬의 첫 조각으로 저장·복원 정책과 위치·참조 형식이다(Docs/Design/
// ReflectionFoundations.ko.md 6.1–6.4절). 정책 enum(UndeclaredEnumPolicy·NewerVersionPolicy·
// UnknownClassPolicy·UnknownFieldPolicy), 저장·복원 정책(FieldEncodePolicy·FieldDecodePolicy),
// 위치 프레임(FieldCodecFrame), 참조 기대 형식(ReferenceExpectation)과 읽은 강한 참조
// (DecodedReference)가 있다. 사슬(FieldCodecPolicy → FieldCodecContext → FieldEncodeContext →
// FieldCodec)은 각 조각이 앞 조각을 include하므로 직접 include하지 말고 Object/FieldCodec.h로
// 받는다.
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

} // namespace rocks
