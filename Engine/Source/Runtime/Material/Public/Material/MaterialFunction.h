#pragma once

#include <Base/Compiler.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Material/MaterialFunctionBase.h>
#include <MaterialSchema/MaterialTraitEnums.h>
#include <NodeGraph/GraphDocument.h>

// 머티리얼 함수 에셋이다(Docs/Design/MaterialSystem 4.2·4.6절, UE UMaterialFunction). 쿠킹
// 런타임에는 남지 않는다: 함수를 쓰는 머티리얼에 펼쳐져 들어간다(Build 의존 — 4.4절).
namespace rocks {

// 사용 예:
//   function->usage = material::MaterialFunctionUsage::Layer;
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_MATERIAL_API MaterialFunction final : public MaterialFunctionBase {
    ROCKS_GENERATED_BODY()

public:
#if ROCKS_WITH_EDITOR_DATA
    explicit MaterialFunction(Allocator& allocator) noexcept
        : graph(allocator), layout(allocator), libraryCategories(allocator), description(allocator),
          userExposedCaption(allocator) {
    }
#else
    explicit MaterialFunction(Allocator&) noexcept {
    }
#endif

#if ROCKS_WITH_EDITOR_DATA
    // 함수 그래프다(도메인 "MaterialFunction" — 입출력 노드 여럿).
    ROCKS_PROPERTY(EditorOnly) nodegraph::GraphDocument graph;
    ROCKS_PROPERTY(EditorOnly) nodegraph::GraphDocumentLayout layout;
    // 노드 팔레트에 보이는가와 범주(UE bExposeToLibrary·LibraryCategoriesText).
    ROCKS_PROPERTY(EditorOnly) bool exposeToLibrary = false;
    ROCKS_PROPERTY(EditorOnly) Array<String> libraryCategories;
    ROCKS_PROPERTY(EditorOnly) String description;
    // 함수 호출 노드에 보일 이름이다(UE UserExposedCaption).
    ROCKS_PROPERTY(EditorOnly) String userExposedCaption;
#endif

    ROCKS_PROPERTY()
    material::MaterialFunctionUsage usage = material::MaterialFunctionUsage::General;
};

} // namespace rocks
