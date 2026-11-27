#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// custom 출력 번호 하나의 입력이다(Docs/Design/MaterialSystem 6.11절 MS2d3 "출력 표").
namespace rocks::material::detail {

// 출력 하나(번호 하나)의 입력이다.
struct CustomOutputInput {
    // 노드 입력 키다.
    StringView key;
    // 결과 이전의 폭(1–4)이다. 결과는 float4로 맞춘다.
    uint8 width = 1;
    // 연결이 없을 때의 기본값이다(hasDefault가 거짓이면 함수를 내지 않는다).
    bool hasDefault = true;
    float fallback[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // 연결이 없으면 "Input missing"이다(UE BentNormal·ClearCoatNormal·Tangent).
    bool required = false;
    // 결과 slot 원문("<이름><번호>")이다.
    StringView slot;
};

} // namespace rocks::material::detail
