#pragma once

#include <Base/Guid.h>
#include <Core/Text/NamePool.h>
#include <MaterialCompiler/MaterialTranslationTarget.h>
#include <MaterialSchema/MaterialNodeCatalog.h>
#include <MaterialSchema/MaterialTraitsDesc.h>
#include <NodeGraph/GraphDocument.h>

// 머티리얼 그래프 번역의 입력이다(Docs/Design/MaterialSystem 6.3·6.8절). 객체 주소를 들고 가지
// 않는다: 그래프 문서 snapshot, 노드 카탈로그, 기본 속성만 받는다. 품질 수준·static 변형은 MS2b가
// 더한다.
//
// 수명: 가리키는 문서·카탈로그·이름 pool·속성은 번역이 끝날 때까지 살아 있고 바뀌지 않아야 한다.
namespace rocks::material {

struct MaterialTranslationInput {
    // 도메인 Material인 그래프 문서다. 번역은 주 그래프(mainGraph)의 출력 노드에서 시작한다.
    const nodegraph::GraphDocument& document;
    // 문서 노드 종류의 정의다(종류 Name으로 찾는다).
    const MaterialNodeCatalog& catalog;
    // 문서 Name이 속한 pool이다. 번역은 TryFind로만 찾고 정적 키를 intern한다.
    NamePool& names;
    // 영역·블렌드 모드·반투명 속성(출력 핀 활성 판정 — V5).
    const MaterialTraitsDesc& traits;
    // 진단에 넣는 자산 id(AssetId의 Guid 값)다.
    Guid asset;
    // 번역 대상(품질·형식 사실·프로필 기능 — 6.9절 MS2b3)이다. 기본값은 UE 기본이다.
    MaterialTranslationTarget target{};
};

} // namespace rocks::material
