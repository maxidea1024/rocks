#pragma once

#include <Base/Guid.h>
#include <Core/Text/StringView.h>
#include <MaterialSchema/MaterialTraitEnums.h>
#include <NodeGraph/GraphDocument.h>

// 번역이 읽는 다른 에셋의 그래프 snapshot이다(Docs/Design/MaterialSystem 19.4.2절, 6.12절 MS2e2 —
// 함수·다른 머티리얼). 번역은 에셋 객체가 아니라 이것만 본다.
//
// 수명: document와 name이 가리키는 data는 번역이 끝날 때까지 살아 있어야 한다. 문서의 Name은 번역
// 입력 names와 같은 pool이어야 한다.
namespace rocks::material {

struct MaterialGraphSnapshot {
    // 에셋 id다(함수 호출·MaterialSample 노드 속성 MaterialAssetReference.assetId).
    Guid asset;
    // 진단 접두에 쓰는 영어 표시 이름이다(UE MaterialFunction->GetName() — "(Function A|B) ").
    StringView name;
    // 그래프 문서다(주 그래프를 펼친다).
    const nodegraph::GraphDocument* document = nullptr;
    // 함수 용도다(머티리얼이면 General).
    MaterialFunctionUsage usage = MaterialFunctionUsage::General;
};

} // namespace rocks::material
