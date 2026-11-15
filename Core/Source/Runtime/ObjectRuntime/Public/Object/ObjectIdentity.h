#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Object/ObjectHandle.h>
#include <Object/TypeLease.h>

// 새 객체 정체성 규칙의 입력과 순수 판정이다(Docs/Design/ObjectFoundations.ko.md 1절). 객체 경로
// 상한(MaxObjectPathBytes), Registry 객체 정책(RegistryPolicy), Outer 요약(OuterSummary),
// CollectInners 순서(InnerOrder), 정체성 규칙 6–9 판정(detail::CheckNewObjectIdentity)과 새 객체
// 경로 길이(detail::ChildPathBytes)를 둔다. Registry 생성과 그래프 1단계가 같이 쓴다.
// Object/ObjectRegistry.h가 include한다.
namespace rocks {

// 객체 경로(Outer 사슬 이름을 '.'로 이은 것 — Docs/Design/ObjectFoundations.ko.md 1.1절)의 byte
// 상한이다(AS 5.1 문자열 상한). 경로가 이것을 넘는 객체는 만들 수 없다(CapacityExceeded). 이름이 1
// byte 이상이므로 Outer 깊이도 이것으로 묶인다(최대 2048).
inline constexpr usize MaxObjectPathBytes = 4096;

// Registry 생성 때 고정하는 객체 정책이다(Docs/Design/ObjectFoundations.ko.md 1.3절). 기본값은 정책
// 없음이다(World registry·시험 Registry). 에셋 객체 Registry(A1 AssetManager)는
// {packageNativeType = NativeTypeKey<Package>(), rejectWorldOnly = true}로 만든다.
//
// 판정은 생성(ConstructionContext::Create·NewObject)의 오류 순서 6·8단계다: WorldOnly는
// Unsupported, 패키지 규칙은 InvalidArgument다.
struct RegistryPolicy {
    // null이 아니면 패키지 정책이다: 이 native 타입(과 파생 — TypeLease::IsNativeA)의 객체는
    // Outer와 이름이 없어야 하고, 그 밖의 객체는 Outer가 있어야 하며 outermost가 이 타입이어야
    // 한다(AS 6.1 "패키지 없는 객체 생성은 거부한다"). TypeId가 아니라 native 키로 정하므로 TypeId
    // 별명도 같은 판정이다. 패키지 정책 Registry에서는 outer → inner도 GC 강한 간선이고(자식 고리가
    // export 표다 — 소속 객체는 Discard로만 죽는다), RequestDestroy·Object::Destroy는 Unsupported다
    // (Docs/Design/ObjectFoundations.ko.md 1.3·1.7절). 또 완전한 필드 배치(RuntimeTypeOps::
    // completeFieldLayout)가 아닌 타입의 생성은 Unsupported다(생성 오류 순서 4 — 관찰자 계수와
    // 패키지 그래프 저장이 그 선언에 기댄다).
    const void* packageNativeType = nullptr;
    // 참이면 WorldOnly 타입(TypeFlags::WorldOnly — 상속됨) 생성을 Unsupported로 거부한다(에셋 객체
    // Registry — RF 2.2·결정 21, AS 11.4의 AssetLease 누수 차단).
    bool rejectWorldOnly = false;

    bool operator==(const RegistryPolicy&) const noexcept = default;
};

// Outer로 쓸 객체의 요약이다(Docs/Design/ObjectFoundations.ko.md 1.2절). 생성 규칙(이름·대표 자식·
// 패키지·경로 길이)을 판정하는 입력이며, 그래프 1단계가 아직 없는 Outer를 기술할 때도 쓴다(A0-Od2).
// 조회 시점의 값이며 수명이나 접근권을 보존하지 않는다.
struct OuterSummary {
    ObjectHandle outermost{};        // Outer 사슬 끝(자기 자신이 outermost면 자기)
    bool isOutermost = false;        // 이 객체에 Outer가 없는지
    bool outermostIsPackage = false; // outermost가 정책의 패키지 타입인지(정책이 없으면 false)
    // Inner 유무는 이름 점유와 같은 기준이다(다른 트랜잭션의 Reserved는 세고, GC cutoff white는
    // 세지 않는다).
    bool hasRepresentative = false; // 대표 자식(빈 이름 Inner)이 있는지
    bool hasNamedInners = false;    // 이름 있는 Inner가 있는지
    uint32 pathBytes = 0;           // 이 객체의 객체 경로 byte 수

    // 아직 만들지 않은 outermost(예: 로더가 곧 만들 Package)의 요약이다. isPackage는 그 객체가
    // 정책의 패키지 타입인지다.
    static constexpr OuterSummary ForNewOutermost(bool isPackage) noexcept {
        OuterSummary summary;
        summary.isOutermost = true;
        summary.outermostIsPackage = isPackage;
        return summary;
    }
};

// CollectInners가 Inner를 모으는 순서다.
enum class InnerOrder : uint8 {
    Direct,    // 직계 Inner만(이름 순, 빈 이름이 먼저)
    DepthFirst // 전위 깊이 우선(각 Inner 바로 뒤에 그 부분 트리) — AS 5.1 export 순서
};

namespace detail {

// 새 객체 정체성 규칙 6–9(Docs/Design/ObjectFoundations.ko.md 1.5절)를 판정하는 순수 함수다.
// Registry 생성과 그래프 1단계·CreateObjects 사전 확인(A0-Od2)이 같이 써서 규칙이 갈라지지 않게
// 한다. 규칙 5(Outer 상태)는 호출자가 outer 요약을 만들 때 판정한다.
//
// 인자: policy는 Registry 정책(또는 그 사본), type은 만들 타입, outer는 Outer 요약(Outer가 없으면
// null), name은 이름(빈 문자열은 이름 없음 또는 대표 자식), scratch는 NFC 판정 임시 저장소다.
//
// 순서와 오류: 6. policy.rejectWorldOnly이고 WorldOnly 타입이면 Unsupported → 7. 이름 형식
// (ValidateNameComponent — InvalidArgument, scratch 할당 실패는 OutOfMemory) → 8. 패키지
// 정책(패키지 타입에 Outer·이름, 비패키지에 Outer 없음·outermost가 패키지 아님), 빈 이름(Outer가
// 있으면 대표 자식뿐 — Outer가 outermost이고 이름 있는 Inner가 없어야 한다), 대표 자식이 있는
// Outer의 이름 있는 자식은 InvalidArgument → 9. 새 객체 경로가 MaxObjectPathBytes를 넘으면
// CapacityExceeded. 같은 대표 자식 둘은 여기서 거부하지 않는다(이름 충돌 — 생성이 AlreadyExists로
// 낸다).
Result<void> CheckNewObjectIdentity(const RegistryPolicy& policy, const TypeLease& type,
                                    const OuterSummary* outer, StringView name,
                                    Allocator& scratch) noexcept;

// outer 아래 이름이 nameBytes byte인 새 객체의 객체 경로 길이다(outer가 null이면 outermost라 0).
// 경로는 Outer 경로 + (둘 다 비어 있지 않으면 '.') + 이름이다(outermost·대표 자식은 빈 경로).
// Registry 생성과 그래프 1단계(아직 없는 노드의 경로 길이 — Docs/Design/ObjectFoundations.ko.md
// 5.4절)가 같이 쓴다. 덧셈은 usize로 한다 — 호출자가 준 요약의 pathBytes가 uint32 최대값이어도
// 넘쳐 작은 값이 되지 않는다(호출자는 결과를 MaxObjectPathBytes와 비교한다).
inline usize ChildPathBytes(const OuterSummary* outer, usize nameBytes) noexcept {
    if (outer == nullptr) {
        return 0;
    }

    const usize separator = outer->pathBytes != 0 && nameBytes != 0 ? 1 : 0;
    return static_cast<usize>(outer->pathBytes) + separator + nameBytes;
}

} // namespace detail

} // namespace rocks
