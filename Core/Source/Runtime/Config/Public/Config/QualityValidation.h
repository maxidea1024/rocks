#pragma once

#include <Config/DeviceTraits.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/String.h>

// 품질 조합 검증(Docs/Design/ConfigSystem.ko.md 6.1절 — 설정 CF4, 계획 24.2절). 품질 묶음의 정의와
// 펼침은 CVar의 `CVarGroup`(Docs/Design/CVarSystem.ko.md 11절 — CVR5)이 맡고, 여기는 그 앞의 순수
// 함수 하나다: "묶음 이름 → 요청 등급"과 기기 특성(DeviceTraits)·검증 규칙을 받아 "묶음 이름 → 펼칠
// 등급 + 사유"를 돌려준다. 층에 아무것도 쓰지 않는다. 묶음 변수(사용자가 고른 등급)는 바꾸지 않고,
// 펼칠 때 쓰는 등급만 낮춘다(장치가 바뀌면 원래 선택이 그대로 살아난다 — CVar 11절).
//
// 규칙:
//   1) 묶음마다 요청 등급의 requirements(기기 조건)가 거짓이면 정의된 바로 아래 등급으로 한 칸씩
//      낮춘다. 가장 낮은 등급도 거짓이면 그 등급에 두고 unmet 사유를 남긴다.
//   2) 예산마다 펼칠 등급 비용의 합이 한도(수 특성 × fraction)를 넘으면, 우선순위가 낮은
//   묶음부터(같으면
//      묶음 이름 byte 순서) 이 예산 비용을 줄이는 한 칸 낮춤을 반복한다. 더 낮출 수 없으면 멈추고
//      unmetBudgets에 사유를 남긴다. 기기에 그 특성이 없으면 그 예산은 보지 않는다.
//   traits가 null이면 검증하지 않는다(요청 등급 그대로).
//
// UE 대응: Scalability에는 장치 능력 기반 조합 검증이 없다(Rocks 추가). 표기는 CVar 11절의
// `quality.shadow@1 (requested 3, lowered: <사유>)`다(FormatQualityResolution).
//
// 사용 예(ConfigContext의 묶음 해결이 펼치기 전에 부른다 — CVar 10절 2단계):
//   const int32 shadowLevels[] = {0, 1, 2, 3};
//   const QualityRequest requests[] = {{"quality.shadow", ShadowGroup.Get(), shadowLevels}};
//   QualityResolutionSet resolved(allocator);
//   if (ResolveQualityLevels(rules, budgets, requests, &traits, resolved)) {
//       // resolved.groups[i]->applied 등급의 항목을 Quality 층에 펼친다
//   }
namespace rocks::config {

// 등급 하나가 예산 하나에 쓰는 양이다.
struct QualityCost {
    StringView budget;
    double amount = 0;
};

// 등급 하나의 검증 규칙이다. 규칙이 없는 등급은 요구·비용이 없다.
struct QualityLevelRule {
    int32 level = 0;
    // 이 등급을 쓸 수 있는 기기 조건(모두 참이어야 한다).
    Span<const DeviceCondition> requirements{};
    Span<const QualityCost> costs{};
};

// 묶음 하나의 검증 규칙이다. 정의된 등급은 규칙이 아니라 요청(QualityRequest::levels — 묶음 정의)에
// 있다.
struct QualityGroupRule {
    // 묶음 이름(묶음 변수의 콘솔 이름 — `quality.shadow`).
    StringView group;
    // 우선순위다. 예산을 맞출 때 작은 묶음부터 낮춘다.
    int32 priority = 0;
    Span<const QualityLevelRule> rules{};
};

// 예산 하나다(여러 묶음의 등급 비용 합의 상한).
struct QualityBudgetRule {
    StringView name;
    // 한도를 주는 수 특성 이름(`gpu.memory_mib`).
    StringView trait;
    // 한도 = 특성 값 × fraction.
    double fraction = 1.0;
};

// 묶음 하나의 요청이다(묶음 변수의 지금 값 — 사용자가 고른 등급 — 과 그 묶음 정의의 등급).
struct QualityRequest {
    StringView group;
    int32 level = 0;
    // 정의된 등급(오름차순, 겹치지 않는다 — CVarGroup 정의). 낮춤은 이 목록의 바로 아래 등급이다.
    // 규칙이 있는 묶음이면 level이 이 안에 있어야 한다.
    Span<const int32> levels{};
};

// 묶음 하나의 결과다.
struct QualityResolution {
    explicit QualityResolution(Allocator& allocator) noexcept
        : group(allocator), reason(allocator) {
    }

    String group;
    int32 requested = 0;
    // 펼칠 등급(낮췄으면 requested보다 작다).
    int32 applied = 0;
    // 낮춘 사유(여러 번이면 `; `로 잇는다)나 가장 낮은 등급도 맞지 않은 사유다(영어 —
    // 진단·Explain용).
    String reason;
    // 가장 낮은 등급의 requirements도 거짓이다.
    bool unmet = false;
};

// ResolveQualityLevels의 결과다.
struct QualityResolutionSet {
    explicit QualityResolutionSet(Allocator& allocator) noexcept
        : groups(allocator), unmetBudgets(allocator) {
    }

    // 요청 순서의 결과.
    Array<UniquePtr<QualityResolution>> groups;
    // 더 낮출 수 없어 넘친 예산의 사유(영어).
    Array<String> unmetBudgets;
};

// 요청마다 펼칠 등급을 정한다(위 규칙). 규칙이 없는 묶음은 요청 등급 그대로다. out은 먼저 비운다.
// 오류: 규칙이 있는 묶음의 요청 등급이 그 요청의 levels에 없거나 같은 묶음 요청이 둘이면
// InvalidArgument, 할당 실패. 실패하면 out은 비어 있다.
Result<void> ResolveQualityLevels(Span<const QualityGroupRule> groups,
                                  Span<const QualityBudgetRule> budgets,
                                  Span<const QualityRequest> requests, const DeviceTraits* traits,
                                  QualityResolutionSet& out) noexcept;

// 결과 하나의 표기를 out에 쓴다: 낮추지 않았으면 `quality.shadow@3`, 낮췄으면
// `quality.shadow@1 (requested 3, lowered: <사유>)`, 맞지 않았으면 `quality.shadow@0 (requested 0,
// unmet: <사유>)`(CVar 11절 표기). 오류: 할당 실패.
Result<void> FormatQualityResolution(const QualityResolution& resolution, String& out) noexcept;

} // namespace rocks::config
