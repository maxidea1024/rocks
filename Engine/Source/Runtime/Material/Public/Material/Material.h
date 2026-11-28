#pragma once

#include <Base/Compiler.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Material/MaterialBase.h>
#include <Material/MaterialPreviewSettings.h>
#include <Material/ParameterCollection.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/MaterialTraitsDesc.h>
#include <MaterialSchema/VertexInputUsage.h>
#include <NodeGraph/GraphDocument.h>
#include <Object/ObjectRefs.h>
#include <Texture/Texture.h>

// 그래프를 가진 머티리얼 에셋이다(Docs/Design/MaterialSystem 4.2·4.6·19.9절, UE UMaterial).
namespace rocks {

// 그래프(EditorOnly)와 기본 속성·사용 대상·물리 재질 필드를 가진다. 쿠킹 데이터(번역 결과)는 MS3이
// 더한다.
//
// 사용 예:
//   auto material = package.NewAsset<Material>(...);
//   material->traits.blendMode = BlendMode::Masked;
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_MATERIAL_API Material final : public MaterialBase {
    ROCKS_GENERATED_BODY()

public:
    explicit Material(Allocator& allocator) noexcept
        : MaterialBase(allocator),
#if ROCKS_WITH_EDITOR_DATA
          graph(allocator), layout(allocator),
#endif
          usage(allocator), parameterCollections(allocator), referencedTextures(allocator),
          renderTracePhysicalMaterials(allocator) {
    }

    // ---- 에디터 데이터 ----
#if ROCKS_WITH_EDITOR_DATA
    // 머티리얼 그래프다(도메인 "Material", 5절). 쿠킹 데이터에는 번역 결과만 간다.
    ROCKS_PROPERTY(EditorOnly) nodegraph::GraphDocument graph;
    // 그래프 배치(노드 위치·주석 상자)다. semantic hash 밖이다(5.5절).
    ROCKS_PROPERTY(EditorOnly) nodegraph::GraphDocumentLayout layout;
    ROCKS_PROPERTY(EditorOnly) MaterialPreviewSettings preview;
#endif

    // 기본 속성(19.9절)이다.
    ROCKS_PROPERTY() material::MaterialTraitsDesc traits;
    // 사용 대상 선언이다(8.4·19.10절).
    ROCKS_PROPERTY() material::VertexInputUsage usage;
    // 그래프의 컬렉션 노드가 쓰는 파라미터 컬렉션(Hard — 실행 때 layout·기본값을 함께
    // 로드, 4.4·4.6.1절) 이다. 번역이 그래프에서 채운다.
    ROCKS_PROPERTY() ObjectRefArray<ParameterCollection> parameterCollections;
    // 그래프와 그 함수·레이어 닫힘이 참조하는 텍스처(Hard — UE FMaterialCachedExpressionData::
    // ReferencedTextures)다. 참조 동기화(MaterialCook SyncMaterialReferences — 12.8절 MS3c)가
    // 채우고 쿠킹이 번역 결과의 텍스처가 이 안에 있는지 대조한다. 그래프 텍스처가 쿠킹 대상과
    // cookKey에 드는 길이다.
    ROCKS_PROPERTY() ObjectRefArray<Texture> referencedTextures;
    // TODO(MPQ-PhysicalMaterial): 물리 재질 에셋이 생기면 Hard 참조로 바꾸지 않고 대상 클래스를
    // 검사한다 (UE PhysMaterial·PhysMaterialMask·RenderTracePhysicalMaterialOutputs).
    ROCKS_PROPERTY() material::MaterialAssetReference physicalMaterial;
    ROCKS_PROPERTY() material::MaterialAssetReference physicalMaterialMask;
    ROCKS_PROPERTY() Array<material::MaterialAssetReference> renderTracePhysicalMaterials;
    // UE PhysicalMaterialMap[8](물리 재질 마스크 색 칸마다)이다.
    ROCKS_PROPERTY()
    material::MaterialAssetReference physicalMaterialMap[MaterialPhysicalMaterialMapSize];
    // TODO(MPQ-VirtualizedGeometry): 가상화 geometry 경로용 대체 머티리얼(UE
    // NaniteOverrideMaterial).
    ROCKS_PROPERTY() material::MaterialAssetReference virtualizedGeometryOverride;
};

} // namespace rocks
