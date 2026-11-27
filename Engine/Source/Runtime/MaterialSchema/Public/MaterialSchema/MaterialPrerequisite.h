#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// 노드 상태와 머티리얼 층 선행이다(Docs/Design/MaterialSystem 19.2·19.4.1절, 결정 MD12).
//
// "TODO" 노드(PrerequisiteMissing)는 서술·속성은 있고 lowering이 선행 ID를 담은 오류를 낸다. 코드
// 표기는 `// TODO(MPQ-Font): …`처럼 한 형식이며, 괄호 안은 MaterialPrerequisiteId가 돌려주는
// 문자열이다 (policy material-todo가 검사한다).
//
// 사용 예:
//   if (definition.status == MaterialNodeStatus::PrerequisiteMissing) {
//       StringView id = MaterialPrerequisiteId(definition.prerequisite); // "MPQ-Font"
//   }
namespace rocks::material {

enum class MaterialNodeStatus : uint8 {
    // lowering이 머티리얼 층 안에서 완결된다(19.2절 "구현").
    Implemented = 0,
    // 머티리얼 층 선행이 없다(19.2절 "TODO"). prerequisite가 None이 아니다.
    PrerequisiteMissing = 1
};

// 19.4.1절 표의 행이다.
enum class MaterialPrerequisite : uint8 {
    None = 0,
    TextureCollection = 1,
    SparseVolumeTexture = 2,
    VirtualTexture = 3,
    ExternalTexture = 4,
    Font = 5,
    MaterialAggregate = 6,
    SubsurfaceProfile = 7,
    SpecularProfile = 8,
    ToonShading = 9,
    NeuralProfile = 10,
    PhysicalMaterial = 11,
    VirtualizedGeometry = 12,
    ColoredTransmittance = 13,
    AlphaHoldout = 14,
    LightBake = 15,
    // 렌더러 noise 시스템 텍스처(UE View PerlinNoiseGradientTexture·PerlinNoise3DTexture — 6.13절
    // MS2f3).
    NoiseTexture = 16,
};

// 선행 수다(None 포함, 마지막 값 + 1).
inline constexpr uint32 MaterialPrerequisiteCount = 17;

// "MPQ-<이름>" 문자열이다(19.4.1절 ID). None·범위 밖이면 빈 문자열이다.
ROCKS_MATERIALSCHEMA_API StringView
MaterialPrerequisiteId(MaterialPrerequisite prerequisite) noexcept;

} // namespace rocks::material
