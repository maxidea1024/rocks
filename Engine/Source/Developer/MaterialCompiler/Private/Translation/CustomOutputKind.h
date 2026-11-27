#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>

#include "CustomOutputInput.h"

// custom 출력 종류 하나다(Docs/Design/MaterialSystem 6.11절 MS2d3 "출력 표").
namespace rocks::material::detail {

// 빈도다.
enum class CustomOutputFrequency : uint8 {
    Pixel = 0,
    // 정점 빈도와 이전 프레임(UE SF_Vertex + NeedsPreviousFrameEvaluation).
    Vertex = 1
};

// 출력 종류 하나다.
struct CustomOutputKind {
    // 노드 종류 이름이다.
    StringView type;
    // Rocks 이름이다(생성 함수 EvaluateMaterialCustomOutput_<이름><번호>).
    StringView name;
    // 정의 이름 조각(이름의 대문자 — MATERIAL_CUSTOM_OUTPUT_<NAME>)이다.
    StringView defineName;
    CustomOutputFrequency frequency = CustomOutputFrequency::Pixel;
    // 대표 노멀 순환 검사 뿌리인가(19.3.3절).
    bool normalRoot = false;
    Span<const CustomOutputInput> inputs;
};

} // namespace rocks::material::detail
