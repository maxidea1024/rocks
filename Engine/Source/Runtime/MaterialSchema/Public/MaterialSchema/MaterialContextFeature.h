#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// 문맥 공급 기능이다(Docs/Design/MaterialSystem 19.3절, 결정 MD13). 렌더러가 채우는 값을 읽는
// 노드는 문맥 구조체(MaterialInputs·MaterialVertexInputs)의 기능별 필드 묶음을 읽는다. 머티리얼은
// 쓰는 기능 집합을 기록하고, pass 셰이더 타입은 채우는 기능 집합을 선언한다. 거르기가 둘을 대조해
// 채우지 않는 조합을 V6로 거부한다. 생성 원문의 `MATERIAL_CONTEXT_<이름>` 정의와 1:1이다.
//
// 비트 번호는 셰이더 특성(MaterialShaderTraits)의 쿠킹 값에 들어가므로 바꾸지 않고 끝에만 더한다.
//
// 사용 예:
//   MaterialContextFeatureMask used = ToMask(MaterialContextFeature::View) |
//                                     ToMask(MaterialContextFeature::Vertex);
//   bool needsScene = HasFeature(used, MaterialContextFeature::Scene);
namespace rocks::material {

enum class MaterialContextFeature : uint8 {
    View = 0,
    Vertex = 1,
    Primitive = 2,
    Skin = 3,
    Lightmap = 4,
    Light = 5,
    Sky = 6,
    PostProcess = 7,
    Scene = 8,
    Decal = 9,
    Particle = 10,
    DistanceField = 11,
    Hair = 12,
    Water = 13,
    Volume = 14,
    RuntimeVirtualTexture = 15,
    MeshPaint = 16,
    PhysicsField = 17,
    SpeedTree = 18,
    Neural = 19,
    FirstPerson = 20,
    Xr = 21,
    PathTracing = 22,
    UserInterface = 23
};

// 기능 수다(마지막 값 + 1).
inline constexpr uint32 MaterialContextFeatureCount = 24;

// 기능 비트 집합이다(비트 i = 기능 i).
using MaterialContextFeatureMask = uint32;

inline constexpr MaterialContextFeatureMask NoMaterialContextFeatures = 0;

constexpr MaterialContextFeatureMask ToMask(MaterialContextFeature feature) noexcept {
    return MaterialContextFeatureMask{1} << static_cast<uint32>(feature);
}

constexpr bool HasFeature(MaterialContextFeatureMask mask,
                          MaterialContextFeature feature) noexcept {
    return (mask & ToMask(feature)) != 0;
}

// 정의된 비트 전체다.
inline constexpr MaterialContextFeatureMask AllMaterialContextFeatures =
    (MaterialContextFeatureMask{1} << MaterialContextFeatureCount) - 1;

// 생성 원문 정의 이름의 꼬리다(`MATERIAL_CONTEXT_` 뒤, 대문자 — 예: "VIEW", "DISTANCE_FIELD"). 범위
// 밖이면 빈 문자열이다.
ROCKS_MATERIALSCHEMA_API StringView
MaterialContextFeatureDefineName(MaterialContextFeature feature) noexcept;

// 진단용 영어 이름이다(19.3절 키 — 예: "view", "distancefield"). 범위 밖이면 빈 문자열이다.
ROCKS_MATERIALSCHEMA_API StringView
MaterialContextFeatureKey(MaterialContextFeature feature) noexcept;

} // namespace rocks::material
