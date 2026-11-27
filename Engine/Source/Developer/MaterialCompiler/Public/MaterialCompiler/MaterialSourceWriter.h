#pragma once

#include <Base/Guid.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <MaterialCompiler/MaterialGeneratedSource.h>
#include <MaterialCompiler/MaterialIrModule.h>
#include <MaterialSchema/MaterialTraitsDesc.h>

// IR 모듈 → 생성 원문 /Engine/Generated/Material.rsh다(Docs/Design/MaterialSystem 7.3·7.7절, 단계
// MS2a3). 7.3절 정의와 진입점 함수를 늘 모두 내고, 함수마다 그 결과에서 닿는 값을 모듈 번호 순으로
// const 지역 변수로 낸다. 같은 모듈·속성은 같은 byte다(locale·C runtime 무관 — 6.6절).
//
// 사용 예:
//   MaterialGeneratedSource source(allocator);
//   if (auto written = WriteMaterialSource(allocator, translation.module, traits, asset, graph,
//                                          source);
//       !written) {
//       // 할당 실패
//   }
//   if (source.succeeded) {
//       Use(source.text.View());
//   }
namespace rocks::material {

// out을 채운다(이전 내용은 지운다). 생성할 수 없는 값(MS2a: Double·WorldPositionTile)은 원인 노드에
// NotYetLowered를 진단하고 out.succeeded가 거짓이다. 반환 실패는 할당 실패나 모듈 손상(값 번호가
// 범위 밖)뿐이다. asset·graph는 진단 위치다.
ROCKS_MATERIALCOMPILER_API Result<void>
WriteMaterialSource(Allocator& allocator, const IrModule& module, const MaterialTraitsDesc& traits,
                    Guid asset, Guid graph, MaterialGeneratedSource& out) noexcept;

} // namespace rocks::material
