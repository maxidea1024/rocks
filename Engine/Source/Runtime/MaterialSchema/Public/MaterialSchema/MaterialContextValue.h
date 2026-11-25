#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>
#include <MaterialSchema/MaterialContextFeature.h>
#include <MaterialSchema/MaterialNodeDefinition.h>
#include <MaterialSchema/MaterialPinType.h>
#include <RenderData/MaterialEnums.h>

// 문맥 값 표다(Docs/Design/MaterialSystem 19.3.2·19.13.5절). 노드가 읽는 렌더러 공급 값(필드 또는
// 함수) 하나마다 반환 타입·공급 기능·허용 영역·단계·최소 기능 등급·미분·공급하지 않았을 때의 값·
// 이전 프레임 변형 여부를 둔다. 정본은 IR이다: 노드가 어느 값을 읽는지는 lowering이 IR extern
// 값으로 기록하고(6.4절), 검증(V5·V6)과 진입점별 사용 정의(19.3.1절)가 이 표를 본다.
//
// 사용 예:
//   const MaterialContextValue* time = FindMaterialContextValue("Time");
//   if (time && !HasFeature(supplied, time->feature)) { /* UE 기본값(time->defaultValue)을 쓴다 */
//   }
namespace rocks::material {

// 값의 형태다.
enum class MaterialContextValueForm : uint8 {
    // 문맥 구조체 필드다(인자 없음).
    Field = 0,
    // 함수 호출이다(argumentCount개 인자 — UE 정의의 $0·$1).
    Function = 1
};

// 미분 성질이다(UE EDerivativeStatus — 외부 코드 선언 Derivative).
enum class MaterialDerivative : uint8 {
    // 미분을 모른다(번역이 유한 차분을 쓴다).
    NotAware = 0,
    // 미분이 0이다(픽셀 사이에 바뀌지 않는다).
    Zero = 1,
    // 해석적 미분이 있다.
    Valid = 2
};

// 기능 등급이다(Docs/Design/MaterialSystem 02절 CapabilityTier — UE ERHIFeatureLevel의 머티리얼
// 용도, 형식 정의가 준다 — 셰이더 시스템 MSR9). 순서가 등급 순서다.
enum class MaterialCapabilityTier : uint8 {
    Mobile = 0,
    Sm5 = 1,
    Sm6 = 2
};

// 영역(MaterialDomain) 비트 집합이다.
using MaterialDomainMask = uint8;

constexpr MaterialDomainMask ToMask(MaterialDomain domain) noexcept {
    return static_cast<MaterialDomainMask>(1u << static_cast<uint32>(domain));
}

inline constexpr MaterialDomainMask AllMaterialDomains =
    ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::DeferredDecal) |
    ToMask(MaterialDomain::LightFunction) | ToMask(MaterialDomain::Volume) |
    ToMask(MaterialDomain::PostProcess) | ToMask(MaterialDomain::UserInterface);

// 공급하지 않았을 때의 값이다(성분 넷 — 행렬은 0 행렬, 텍스처 서술자는 무효).
struct MaterialContextDefault {
    float values[MaxMaterialPinDefaultComponents] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct MaterialContextValue {
    // Rocks 이름이다(19.5절 규칙). 표 안에서 대소문자 무시로 유일하다.
    StringView name;
    // UE 외부 코드 선언 이름이다. UE 템플릿 필드에서 온 값은 비어 있다.
    StringView unrealName;
    MaterialPinType type = MaterialPinType::Float1;
    MaterialContextValueForm form = MaterialContextValueForm::Field;
    uint8 argumentCount = 0;
    MaterialContextFeature feature = MaterialContextFeature::View;
    MaterialDomainMask domains = AllMaterialDomains;
    MaterialStageMask stages = AllMaterialStages;
    MaterialCapabilityTier minimumTier = MaterialCapabilityTier::Mobile;
    MaterialDerivative derivative = MaterialDerivative::NotAware;
    MaterialContextDefault defaultValue;
    // 이전 프레임 변형이 있다(UE 정의의 <PREV> — 19.3.5절).
    bool hasPrevious = false;
};

// 표 전체다(정적 data — 수명은 프로세스).
ROCKS_MATERIALSCHEMA_API Span<const MaterialContextValue> MaterialContextValues() noexcept;

// 이름의 값이다(대소문자 구분). 없으면 nullptr이다.
ROCKS_MATERIALSCHEMA_API const MaterialContextValue*
FindMaterialContextValue(StringView name) noexcept;

} // namespace rocks::material
