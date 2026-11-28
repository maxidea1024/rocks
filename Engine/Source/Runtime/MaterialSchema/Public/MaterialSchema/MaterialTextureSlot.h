#pragma once

#include <Base/Types.h>
#include <MaterialSchema/MaterialReferenceIndex.h>
#include <MaterialSchema/MaterialSamplerSource.h>
#include <MaterialSchema/MaterialTextureKind.h>
#include <MaterialSchema/MaterialTextureSource.h>
#include <Meta/Annotations.h>

// 머티리얼 artifact의 텍스처 slot 하나다(Docs/Design/MaterialSystem 12.8절 MS3d2 — 생성 원문의
// MaterialTexture<k>). 텍스처 객체는 Material::referencedTextures 번호로 가리킨다(Hard 간선은 그
// 필드가 가진다 — UE 텍스처 uniform 식의 ReferencedTextures 번호).
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialTextureSlot {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() MaterialTextureSource source = MaterialTextureSource::Asset;
    ROCKS_PROPERTY() MaterialTextureKind kind = MaterialTextureKind::Texture2D;
    ROCKS_PROPERTY() MaterialSamplerSource sampler = MaterialSamplerSource::None;
    // 파라미터 slot이면 Material::parameters 번호다(아니면 NoMaterialReference).
    ROCKS_PROPERTY() uint32 parameter = NoMaterialReference;
    // 참조 텍스처 또는 파라미터 기본 텍스처의 Material::referencedTextures 번호다.
    ROCKS_PROPERTY() uint32 texture = NoMaterialReference;
    // 상수 프로그램이 이 slot의 논리 크기(TextureSize)를 읽는가.
    ROCKS_PROPERTY() bool logicalSize = false;
    // 전체 상주 전용인가(7.4절 — 상주 mip helper를 쓸 수 없는 샘플). 상주 mip helper(MS4
    // TextureStreaming 연결)가 생기면 번역이 채우고, 그 전에는 늘 거짓이다.
    ROCKS_PROPERTY() bool wholeResident = false;
};

} // namespace rocks::material
