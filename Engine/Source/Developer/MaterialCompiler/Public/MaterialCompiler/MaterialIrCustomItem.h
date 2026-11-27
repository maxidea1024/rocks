#pragma once

#include <MaterialCompiler/MaterialIrText.h>
#include <MaterialCompiler/MaterialIrType.h>

// Custom 함수 표 항목의 목록 칸 하나다(Docs/Design/MaterialSystem 6.13절 MS2f5). 쓰는 칸은 목록마다
// 다르다:
//   인자 — name(HLSL 인자 이름)·type(값 형, 텍스처면 텍스처 형 — 샘플러 인자가 하나 더 붙는다)
//   추가 출력 — name·type(Float1–4)
//   정의 — name·value(#define 원문)
//   include — value(가상 경로)
namespace rocks::material {

struct IrCustomItem {
    IrText name;
    IrText value;
    IrType type = IrFloat(1);
};

} // namespace rocks::material
