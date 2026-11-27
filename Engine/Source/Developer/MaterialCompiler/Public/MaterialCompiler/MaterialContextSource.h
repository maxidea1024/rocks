#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <MaterialSchema/MaterialPinType.h>

// 문맥 원문 Engine/Shaders/Private/Material/MaterialContext.rsh를 문맥 값 표(19.3.2절)로 만든다
// (Docs/Design/MaterialSystem 6.9절). 원문은 공급 표지 기본값(MATERIAL_SUPPLY_<기능> 0), 문맥
// 구조체(MaterialVertexInputs·MaterialInputs — 필드형 값마다 칸 하나), 접근자 매크로
// MaterialContext<이름>(공급하면 칸이나 pass 함수 MaterialPass<이름>, 아니면 UE 기본값 — 결정
// MD15), 채울 기능 MATERIAL_CONTEXT_<기능>을 담는다. 저장소의 파일은 이 함수의 출력이고, 시험이
// byte로 견준다(표를 고치면 파일을 함께 고친다).
//
// 사용 예:
//   String text(allocator);
//   if (auto written = WriteMaterialContextSource(allocator, text); !written) {
//       // 할당 실패
//   }
namespace rocks::material {

// 문맥 값 형의 HLSL 원문 이름이다(MaterialContext.rsh·생성 원문과 같은 철자 — 예: Float3 →
// "float3", Double3 → "MaterialDouble3"). 원문 형이 없는 핀 형(자원 등)은 빈 문자열이다.
ROCKS_MATERIALCOMPILER_API StringView MaterialContextTypeName(MaterialPinType type) noexcept;

// out을 표 전체의 원문으로 바꾼다. 같은 표는 같은 byte다(locale·C runtime 무관). 오류: 할당 실패,
// 원문 타입이 없는 표 값(InvalidArgument — 표의 잘못).
ROCKS_MATERIALCOMPILER_API Result<void> WriteMaterialContextSource(Allocator& allocator,
                                                                   String& out) noexcept;

} // namespace rocks::material
