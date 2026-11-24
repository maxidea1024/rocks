#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>

// 셰이더 형식의 공용 어휘다(Docs/Design/ShaderSystem 12.2.1절 SSD1). 형식 정의 읽기
// (ShaderCompilerCommon)와 런타임(셰이더 집합·Graphics 장치)이 같은 정의를 쓰도록 Runtime 모듈
// ShaderCore에 둔다: backend, 단계 비트, 기능 비트, 장치 요구 비트, 셰이더 모델 버전, 단계별 상한.
// 형식 정의 파일(JSON)의 어휘 철자와 읽기·검증은 ShaderCompilerCommon(ShaderFormatDefinition.h)에
// 있다.
//
// 사용 예:
//   if ((facts.features & ShaderFeatureWaveOperations) != 0 &&
//       (type.stage & ShaderStageCompute) != 0) { ... }
namespace rocks::shader {

// 형식 id의 최대 byte 수(NUL 포함)다.
inline constexpr usize MaxShaderFormatIdBytes = 32;

// 상한이 없다는 표지다(uavTotal이 없는 형식, uniform 에뮬레이션이 없는 형식의 uniformVectors).
inline constexpr uint32 NoShaderLimit = NumericLimits<uint32>::Max();

enum class ShaderBackend : uint8 {
    D3D12,
    D3D11,
    Vulkan,
    Metal,
    // OpenGL ES backend다(UE EWindowsRHI::OpenGL과 같은 이름, 12.3절).
    OpenGL
};

// 단계 비트("stages"·"uavStages").
enum ShaderStageBits : uint32 {
    ShaderStageVertex = 1u << 0,
    ShaderStagePixel = 1u << 1,
    ShaderStageCompute = 1u << 2
};

// 지원 기능 비트("features"). "이 형식이 그 기능을 쓰는 셰이더를 컴파일한다"는 뜻이다. 셰이더
// 타입이 요구한 기능이 없으면 그 형식에서 쿠킹하지 않는다(2절, 12.2절 거르기).
//
// 기능은 두 범주다(15절 결정 12).
// - 장치 보장 기능: 형식의 deviceRequirements가 보장하므로 그 형식을 고른 장치는 늘 쓸 수 있다.
// - 런타임 확인 기능(RuntimeCheckedShaderFeatures): 같은 형식 안에서도 장치마다 다르다. 런타임
//   형식 선택은 이 기능으로 장치를 거르지 않고, 셰이더 집합이 장치 능력(Graphics DeviceCaps)과
//   대조해 없는 장치에서 그 셰이더 생성을 Unsupported로 막는다. 호출자(렌더러)가 대체 경로를
//   고른다.
enum ShaderFeatureBits : uint32 {
    ShaderFeatureCompute = 1u << 0,
    // SM 6 웨이브 연산(2절, UE EShaderOptionalDataKey::ShaderModel6에 해당).
    ShaderFeatureWaveOperations = 1u << 1,
    // VS 출력 SV_RenderTargetArrayIndex·SV_ViewportArrayIndex(7.2절 7항). 런타임 확인 기능이다.
    // 장치 쪽 값은 DeviceCaps의 vertexViewportLayerOutput이다.
    ShaderFeatureVertexViewportLayerOutput = 1u << 2,
    // 형식 확장 typed UAV load(R32 밖 형식의 typed UAV 읽기, 11.1절 E8). 장치 보장 기능이며 D3D12는
    // 장치 요구 D3D12_TYPED_UAV_LOAD_ADDITIONAL_FORMATS로 보장한다.
    ShaderFeatureTypedUavLoad = 1u << 3
};

// 모든 기능 비트다(SC10이 기능마다 ROCKS_FEATURE_<이름>을 정의한다 — 어휘를 더하면 이 값도 바꾼다).
inline constexpr uint32 AllShaderFeatures = ShaderFeatureCompute | ShaderFeatureWaveOperations |
                                            ShaderFeatureVertexViewportLayerOutput |
                                            ShaderFeatureTypedUavLoad;

// 런타임 확인 기능 비트 묶음이다. 나머지 기능은 장치 보장 기능이다. 셰이더 라이브러리 항목은
// 요구 기능 가운데 이 비트만 담는다(12.2절).
inline constexpr uint32 RuntimeCheckedShaderFeatures = ShaderFeatureVertexViewportLayerOutput;

// feature 비트 하나가 런타임 확인 기능인가.
constexpr bool IsRuntimeCheckedShaderFeature(uint32 feature) noexcept {
    return (feature & RuntimeCheckedShaderFeatures) != 0;
}

// 장치 요구 비트("deviceRequirements"). 런타임 backend가 장치를 고를 때 대조한다.
enum ShaderDeviceRequirementBits : uint32 {
    DeviceRequiresD3DFeatureLevel11_0 = 1u << 0,
    DeviceRequiresD3D12ShaderModel6_6 = 1u << 1,
    DeviceRequiresVulkan1_1 = 1u << 2,
    DeviceRequiresOpenGL4_3 = 1u << 3,
    DeviceRequiresOpenGLES3_2 = 1u << 4,
    DeviceRequiresGlArbEs32Compatibility = 1u << 5,
    DeviceRequiresGlArbClipControl = 1u << 6,
    // D3D12 TypedUAVLoadAdditionalFormats(typedUavLoad 기능의 장치 보장, 11.1절 E8).
    DeviceRequiresD3D12TypedUavLoadAdditionalFormats = 1u << 7
};

// 셰이더 모델·언어 버전(major.minor)이다. 셰이더 모델 6.6은 {6, 6}, MSL 2.4는 {2, 4}다.
struct ShaderVersion {
    uint8 major = 0;
    uint8 minor = 0;

    friend constexpr bool operator==(ShaderVersion, ShaderVersion) noexcept = default;
};

// 단계별 상한(7.2절 4항)이다. 모든 상한은 job 형식의 이 값으로 검사한다(프로필 공통 상한은 없다).
// 핵심 최소 보장을 넘는 값은 장치 요구이며, 런타임 형식 선택이 장치 능력과 대조한다.
struct ShaderFormatLimits {
    uint32 constantBuffers = 0;
    uint32 sampledTextures = 0;
    uint32 storageBuffers = 0;
    uint32 storageImages = 0;
    uint32 samplers = 0;
    // 파이프라인 전체 UAV 수(D3D Tier 1 FL 11.0의 8). 없으면 NoShaderLimit.
    uint32 uavTotal = NoShaderLimit;
    // UAV(구조화 버퍼 쓰기·storage image)를 쓸 수 있는 단계 비트. 데스크톱 공통은 CS만이다.
    uint32 uavStages = 0;
    uint32 renderTargets = 0;
    uint32 interStageVectors = 0;
    uint32 rootConstantBytes = 0;
    // GLES 상수 버퍼 에뮬레이션의 단계별 vec4 합계 상한(VS·PS·CS 순). 에뮬레이션이 없으면
    // NoShaderLimit.
    uint32 uniformVectors[3] = {NoShaderLimit, NoShaderLimit, NoShaderLimit};
};

// backend 이름이다("D3D12"·"D3D11"·"Vulkan"·"Metal"·"OpenGL" — 형식 정의 어휘 철자, 정적 수명).
// 범위 밖이면 "unknown".
ROCKS_SHADERCORE_API const char* ShaderBackendName(ShaderBackend backend) noexcept;

} // namespace rocks::shader
