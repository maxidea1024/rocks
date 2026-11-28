#pragma once

#include <Base/Types.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <MaterialSchema/MaterialQualityLevel.h>
#include <Meta/Annotations.h>

// (셰이더 형식, 품질) → artifact 번호 표의 항목이다(Docs/Design/MaterialSystem 12.4절, 12.8절 MS3d2
// — 품질은 여기에만 둔다, artifact는 품질·형식 사이에 공유된다).
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialFormatArtifact {
    ROCKS_GENERATED_BODY()

    explicit MaterialFormatArtifact(Allocator& allocator) noexcept : formatId(allocator) {
    }

    ROCKS_PROPERTY() String formatId;
    ROCKS_PROPERTY() MaterialQualityLevel quality = MaterialQualityLevel::None;
    // Material::artifacts 번호다.
    ROCKS_PROPERTY() uint32 artifact = 0;
};

} // namespace rocks::material
