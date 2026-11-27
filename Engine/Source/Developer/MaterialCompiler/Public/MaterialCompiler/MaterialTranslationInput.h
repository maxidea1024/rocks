#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Span.h>
#include <Core/Text/NamePool.h>
#include <MaterialCompiler/MaterialCollectionLayout.h>
#include <MaterialCompiler/MaterialSourceProvider.h>
#include <MaterialCompiler/MaterialStaticParameterValue.h>
#include <MaterialCompiler/MaterialTextureFacts.h>
#include <MaterialCompiler/MaterialTranslationTarget.h>
#include <MaterialSchema/MaterialAttributeTable.h>
#include <MaterialSchema/MaterialNodeCatalog.h>
#include <MaterialSchema/MaterialTraitsDesc.h>
#include <NodeGraph/GraphDocument.h>

// 머티리얼 그래프 번역의 입력이다(Docs/Design/MaterialSystem 6.3·6.8절). 객체 주소를 들고 가지
// 않는다: 그래프 문서 snapshot, 노드 카탈로그, 기본 속성, 번역 대상, static 값, 컬렉션 layout을
// 값으로 받는다.
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
    // static 파라미터 값이다(6.10절 MS2c2 — 없는 파라미터는 노드 기본값).
    Span<const MaterialStaticParameterValue> staticParameters{};
    // 머티리얼이 참조할 수 있는 파라미터 컬렉션 layout이다(6.10절 MS2c2 — 없으면 컬렉션 노드는
    // "invalid Collection" 오류다).
    Span<const MaterialCollectionLayout> collections{};
    // 머티리얼이 참조하는 텍스처 에셋 사실이다(6.10절 MS2c3 — 없는 asset 참조는 해석 오류).
    Span<const MaterialTextureFacts> textures{};
    // 등록된 custom 속성 표다(6.12절 MS2e1 — 없으면 내장 표만). 번역 동안 살아 있어야 한다.
    const MaterialAttributeTable* attributes = nullptr;
    // 원본 공급이다(6.12절 MS2e2 — 함수 호출·MaterialSample). 없으면 그 노드는 에셋 없음 진단이다.
    const MaterialSourceProvider* sources = nullptr;
};

} // namespace rocks::material
