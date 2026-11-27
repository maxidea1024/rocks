#pragma once

#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>

#include "CustomOutputKind.h"

// custom 출력 노드 표다(Docs/Design/MaterialSystem 6.11절 MS2d3 "출력 표"). 번역기가 결과를 만들고
// 생성 원문이 존재 정의와 함수를 낸다. 이름은 UE GetFunctionName에서 Get을 뗀 것이다.
namespace rocks::material::detail {

// 표 전부다(MS2d3 표 순서).
Span<const CustomOutputKind> CustomOutputKinds() noexcept;

// 종류 이름의 표 칸이다(없으면 nullptr).
const CustomOutputKind* FindCustomOutputKind(StringView type) noexcept;

// 이름의 표 칸이다(없으면 nullptr).
const CustomOutputKind* FindCustomOutputByName(StringView name) noexcept;

} // namespace rocks::material::detail
