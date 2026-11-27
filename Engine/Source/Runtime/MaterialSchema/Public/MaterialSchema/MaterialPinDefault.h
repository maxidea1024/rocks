#pragma once

#include <Base/Types.h>

// 머티리얼 입력 핀 기본값이다(Docs/Design/MaterialSystem 5.2절). 노드 정의(MaterialPinDefinition)와
// 문맥 값 표(MaterialContextValue)가 성분 수 상한을 함께 쓴다.
namespace rocks::material {

// 핀 기본값의 최대 성분 수다(float4).
inline constexpr uint8 MaxMaterialPinDefaultComponents = 4;

// 입력 핀을 연결하지 않고 사용자 값도 없을 때의 값이다. componentCount 0은 기본값 없음이다(연결이
// 없으면 필수 입력 오류 또는 노드가 정한 동작).
struct MaterialPinDefault {
    float values[MaxMaterialPinDefaultComponents] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint8 componentCount = 0;

    static constexpr MaterialPinDefault Scalar(float x) noexcept {
        return MaterialPinDefault{{x, 0.0f, 0.0f, 0.0f}, 1};
    }

    static constexpr MaterialPinDefault Vector2(float x, float y) noexcept {
        return MaterialPinDefault{{x, y, 0.0f, 0.0f}, 2};
    }

    static constexpr MaterialPinDefault Vector3(float x, float y, float z) noexcept {
        return MaterialPinDefault{{x, y, z, 0.0f}, 3};
    }

    static constexpr MaterialPinDefault Vector4(float x, float y, float z, float w) noexcept {
        return MaterialPinDefault{{x, y, z, w}, 4};
    }
};

} // namespace rocks::material
