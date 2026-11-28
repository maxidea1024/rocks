#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Span.h>
#include <MaterialCompiler/MaterialStaticParameterValue.h>
#include <MaterialCompiler/MaterialTranslationTarget.h>
#include <MaterialCook/MaterialCookCatalog.h>
#include <MaterialCook/MaterialCookSources.h>
#include <MaterialSchema/MaterialTraitsDesc.h>
#include <NodeGraph/GraphDocument.h>
#include <Object/ObjectRegistry.h>

// 번역 너겟 키의 입력이다(Docs/Design/MaterialSystem 12.2절 표·12.8절 MS3c "번역 너겟"). 번역
// 입력과 같은 값을 가리킨다(같은 Gather 결과).
//
// 사용 예:
//   MaterialTranslationKeyDesc desc;
//   desc.catalog = &catalog;  desc.registry = context.registry;  desc.access = context.access;
//   desc.root = &material.graph;  desc.traits = &material.traits;  desc.target = target;
//   desc.sources = &sources;  desc.asset = TypeIdOf<Material>().value;
namespace rocks {

struct MaterialTranslationKeyDesc {
    const MaterialCookCatalog* catalog = nullptr;
    // 기본 속성(리플렉션 정규 인코딩)을 쓰는 Registry·접근 문맥이다.
    ObjectRegistry* registry = nullptr;
    ObjectAccessContext access{};
    // 에셋 TypeId 값(Material 또는 static 변형의 MaterialInstance)이다.
    Guid assetType{};
    const nodegraph::GraphDocument* root = nullptr;
    const material::MaterialTraitsDesc* traits = nullptr;
    // static 값이다(사슬 합침 — 순서 무관, 키가 parameterId 순으로 쓴다).
    Span<const material::MaterialStaticParameterValue> statics{};
    material::MaterialTranslationTarget target{};
    const MaterialCookSources* sources = nullptr;
    // 키 공통 머리의 프로젝트 세대다(NuggetCache::ProjectGeneration).
    Guid projectGeneration{};
};

} // namespace rocks
