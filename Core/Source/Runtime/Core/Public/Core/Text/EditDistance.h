#pragma once

#include <Core/Text/StringView.h>

// 두 텍스트의 편집 거리(OSA — 제한된 Damerau-Levenshtein: 삽입·삭제·바꾸기·인접 두 글자 맞바꾸기
// 하나가 1, 한 번 맞바꾼 글자는 다시 고치지 않는다)다. 이름 오타에 가까운 이름을 제안하는 데 쓰는
// 공용 함수 하나다(CVar `ReportUnresolved`의 `did you mean` — Docs/Design/CVarSystem.ko.md 12절,
// 명령줄 이름 제안 — Docs/Design/AppInfoCommandLine.ko.md). 문턱은 호출자가 이름 붙은 상수로 준다.
// UE 대응: Algo::LevenshteinDistance(Core/Public/Algo/LevenshteinDistance.h — 맞바꾸기 없음). UE는
// 두 길이에 비례하는 행을 할당하지만, 여기서는 한도 안의 거리만 묻는 쓰임에 맞춰 할당 없이 한도
// 폭의 띠만 계산한다.
namespace rocks {

// EditDistance가 정확히 세는 거리의 최댓값이다(띠 폭 2 × 한도 + 1 칸 세 행을 스택에 둔다). 이보다
// 큰 한도는 이 값으로 줄여 계산한다.
inline constexpr usize MaxEditDistanceLimit = 32;

// 글자 비교 방식이다.
enum class EditDistanceCase : uint8 {
    // byte가 같아야 같은 글자다.
    Sensitive,
    // ASCII 영문자의 대소문자를 같게 본다(그 밖의 byte — UTF-8 다중 byte 글자 포함 — 는 그대로
    // 비교한다).
    AsciiInsensitive,
};

// left와 right의 byte 단위 OSA 편집 거리가 limit 이하면 그 거리를, 넘으면 limit + 1을 돌려준다.
// limit이 MaxEditDistanceLimit보다 크면 MaxEditDistanceLimit으로 줄여 계산한다 — 그보다 먼 거리는
// "멀다"(MaxEditDistanceLimit + 1)로만 답한다. UTF-8 다중 byte 글자는 byte마다 센다. 할당하지
// 않고 O(min(n, m) × (2 × limit + 1))이다(n, m은 두 byte 수 — 길이 차이가 한도를 넘으면 바로
// 답한다).
//
// 사용 예:
//   EditDistance("render.shadow.distanse", "render.shadow.distance", 3,
//                EditDistanceCase::Sensitive);                                  // 1
//   EditDistance("ab", "ba", 3, EditDistanceCase::Sensitive);                  // 1(맞바꾸기)
//   EditDistance("--Verbose", "--verbose", 2, EditDistanceCase::AsciiInsensitive); // 0
//   EditDistance("kitten", "sitting", 2, EditDistanceCase::Sensitive);         // 3(= 한도 + 1)
usize EditDistance(StringView left, StringView right, usize limit,
                   EditDistanceCase textCase) noexcept;

} // namespace rocks
