#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/NamePool.h>
#include <Material/MaterialGraphReference.h>
#include <NodeGraph/GraphDocument.h>

// 그래프의 에셋 참조 수집이다(Docs/Design/MaterialSystem 19.4.3절 "참조 선언" — 에셋 Build 의존
// hook(4.6.1절)과 쿠킹 참조 동기화·번역 입력 모으기(12.8절 MS3c)가 같은 수집을 쓴다).
//
// 사용 예:
//   Array<MaterialGraphReference> references(allocator);
//   if (auto collected = CollectMaterialGraphReferences(names, material.graph, references);
//       !collected) { ... }
namespace rocks {

// document의 모든 그래프·노드가 선언한 에셋 참조(엔진 노드 정의 AssetReferences)와 레이어 노드
// (AttributeLayers·LayerStack)의 레이어·혼합 함수(역할 Function)를 out 끝에 더한다. null 참조와
// 읽지 못하는 속성은 건너뛴다(번역이 진단한다). 순서는 그래프·노드·선언 순이고 중복을 그대로
// 둔다. 플러그인 노드의 선언은 아직 읽지 않는다(엔진 노드만 — 플러그인 카탈로그 연결은
// ModuleLoader 단계). 오류: 할당 실패.
ROCKS_MATERIAL_API Result<void>
CollectMaterialGraphReferences(const NamePool& names, const nodegraph::GraphDocument& document,
                               Array<MaterialGraphReference>& out) noexcept;

} // namespace rocks
