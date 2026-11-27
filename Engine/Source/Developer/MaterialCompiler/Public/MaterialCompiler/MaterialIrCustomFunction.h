#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <MaterialCompiler/MaterialIrText.h>
#include <MaterialCompiler/MaterialIrType.h>

// Custom HLSL 함수 하나다(Docs/Design/MaterialSystem 6.13절 MS2f5 — UE
// FMaterialCustomExpressionEntry). CustomCall 값의 즉시값 [0]이 표 번호다. (node, context, 인자
// 형)이 같으면 한 항목이다. 목록 칸은 표의 칸 배열에 인자 → 추가 출력 → 정의 → include 순으로
// 이어 있다(firstItem부터).
//
// 사용 예:
//   for (const IrCustomFunction& function : module.CustomFunctions().Functions()) {
//       const StringView code = module.CustomFunctions().Text(function.code);
//   }
namespace rocks::material {

// 번역 문맥이다(생성 함수의 첫 인자 — 픽셀 MaterialInputs, 정점 MaterialVertexInputs).
enum class IrCustomContext : uint8 {
    Pixel = 0,
    Vertex = 1
};

struct IrCustomFunction {
    // Custom 노드의 NodeId다(함수 펼침 사본이면 사본 id — 6.12절 MS2e2).
    Guid node;
    IrCustomContext context = IrCustomContext::Pixel;
    // 반환 형이다(Float1–4).
    IrType returnType = IrFloat(1);
    // 함수 본문이다(return 감싸기·줄 끝 정리를 마친 원문).
    IrText code;
    // 코드가 clip·discard로 픽셀을 버릴 수 있는가(UE HasPixelDiscard — 선언 Yes 또는 Search가
    // 찾음).
    bool pixelDiscard = false;
    uint32 firstItem = 0;
    uint32 argumentCount = 0;
    uint32 outputCount = 0;
    uint32 defineCount = 0;
    uint32 includeCount = 0;
};

} // namespace rocks::material
