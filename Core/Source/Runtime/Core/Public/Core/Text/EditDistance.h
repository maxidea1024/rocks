#pragma once

#include <Core/Text/StringView.h>

// 두 텍스트의 편집 거리(Levenshtein — 삽입·삭제·바꾸기 하나가 1)다. 이름 오타에 가까운 이름을
// 제안하는 데 쓴다(CVar `ReportUnresolved`의 `did you mean` — Docs/Design/CVarSystem.ko.md 12절).
// UE 대응: Algo::LevenshteinDistance(Core/Public/Algo/LevenshteinDistance.h). UE는 두 길이에
// 비례하는 행을 할당하지만, 여기서는 한도 안의 거리만 묻는 쓰임에 맞춰 할당 없이 한도 폭의 띠만
// 계산한다.
namespace rocks {

// EditDistanceWithin이 받는 한도의 최댓값이다(띠 폭 2 × 한도 + 1 칸을 스택에 둔다).
inline constexpr usize MaxEditDistanceLimit = 32;

// left와 right의 byte 단위 편집 거리가 limit 이하면 그 거리를, 넘으면 limit + 1을
// 돌려준다(대소문자를 구분한다 — UTF-8 다중 byte 글자는 byte마다 센다). 할당하지 않고 O(min(n, m) ×
// limit)이다.
//
// 사용 예:
//   EditDistanceWithin("render.shadow.distanse", "render.shadow.distance", 3);   // 1
//   EditDistanceWithin("kitten", "sitting", 2);                                    // 3(= 한도 + 1)
//
// 전제: limit ≤ MaxEditDistanceLimit(아니면 Fatal).
usize EditDistanceWithin(StringView left, StringView right, usize limit) noexcept;

} // namespace rocks
