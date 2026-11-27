#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>
#include <MaterialSchema/MaterialContextFeature.h>
#include <MaterialSchema/MaterialGraphKind.h>
#include <MaterialSchema/MaterialPinDefinition.h>
#include <MaterialSchema/MaterialPinType.h>
#include <MaterialSchema/MaterialPrerequisite.h>
#include <MaterialSchema/MaterialStageMask.h>
#include <NodeGraph/NodeTypeDescriptor.h>

namespace rocks {
struct TypeSchema;
} // namespace rocks

// 머티리얼 노드 정의 형식이다(Docs/Design/MaterialSystem 5.2·19.2절, 계획 14.6절
// MaterialNodeDescriptor). 정의는 정적 데이터다: 이름은 StringView이고 MaterialNodeCatalog가 등록할
// 때 NamePool로 intern해 NodeGraph NodeTypeDescriptor를 만든다. NodeGraph 서술에 없는 머티리얼
// 확장(입력 기본값·필수·static 요구·단계·문맥 기능·상태·선행)을 함께 둔다. lowering은 여기 없다
// (MaterialCompiler — 3.2절 금지 항목).
//
// 수명: 정의와 그것이 가리키는 문자열·배열은 등록한 모듈의 정적 data다. 등록 module lease가 지킨다.
//
// 사용 예(입력 둘·출력 하나인 곱셈 노드):
//   constexpr MaterialPinDefinition multiplyPins[] = {
//       MaterialInput("A", MaterialPinType::Numeric, "A", MaterialPinDefault::Scalar(0.0f)),
//       MaterialInput("B", MaterialPinType::Numeric, "B", MaterialPinDefault::Scalar(1.0f)),
//       MaterialOutput("Result", MaterialPinType::Numeric, "Result"),
//   };
//   constexpr MaterialNodeDefinition multiply =
//       MaterialNode("Material.Multiply")
//           .Pins(multiplyPins)
//           .Menu("Multiply", "Math", "*", "Multiplies two values component-wise.");
namespace rocks::material {

// 비활성 노드의 통과 쌍 정의다(NodeGraph PassThroughPair의 정적 형식 — 키는 등록 때 intern한다).
struct MaterialPassThrough {
    StringView input;
    StringView output;
};

// 노드 표지 비트다.
enum class MaterialNodeFlags : uint8 {
    None = 0,
    // 팔레트에 보이지 않는다(UE meta=(Private)·팔레트 제외 규칙, 19.1절).
    HiddenFromPalette = 1u << 0,
    // UE Experimental 표지 노드다(19.1절, 결정 MD11).
    Experimental = 1u << 1,
    // 파라미터 노드다(이름·ParameterId 속성을 가진다 — 9.2절).
    Parameter = 1u << 2
};

constexpr MaterialNodeFlags operator|(MaterialNodeFlags left, MaterialNodeFlags right) noexcept {
    return static_cast<MaterialNodeFlags>(static_cast<uint8>(left) | static_cast<uint8>(right));
}

constexpr bool HasNodeFlag(MaterialNodeFlags set, MaterialNodeFlags flag) noexcept {
    return (static_cast<uint8>(set) & static_cast<uint8>(flag)) == static_cast<uint8>(flag);
}

// 정의된 노드 표지 비트 전체다.
inline constexpr uint8 MaterialNodeFlagMask = (1u << 3) - 1;

// 새 노드의 String·Name 속성 기본값 하나다(Docs/Design/MaterialSystem 19.13.2절). UE 생성자 기본값
// 중 C++ 초기값으로 줄 수 없는 것(String은 allocator, Name은 NamePool이 필요하다 — 예: Custom 노드
// Code "1")을 정적 data로 둔다. 노드를 새로 만드는 쪽(에디터 노드 추가)이 속성 struct 기본 생성
// 값에 이 값을 넣은 뒤 Record를 쓴다. field는 propertiesSchema의 최상위 String·Name 필드다.
struct MaterialPropertyTextDefault {
    StringView field;
    StringView text;
};

// 속성 struct의 생성 스키마를 돌려준다(GeneratedType<T>::Schema). 속성이 없는 노드는 nullptr이다.
using MaterialPropertiesSchemaFn = const TypeSchema& (*)() noexcept;

// 노드 종류 하나의 정의다.
struct MaterialNodeDefinition {
    // "Material.<이름>" 또는 플러그인 "<모듈>.<이름>"(19.5절).
    StringView type;
    // 서술 버전이다. 1보다 크면 migrate가 있어야 한다.
    uint32 schemaVersion = 1;
    MaterialPropertiesSchemaFn propertiesSchema = nullptr;
    // 의미 바이트에서 빼는 속성 필드 이름이다(파라미터 기본값 — 5.5절). 원소는 propertiesSchema의
    // 필드 이름이어야 한다(등록 검사).
    Span<const StringView> nonSemanticFields;
    // 새 노드의 String·Name 속성 기본값이다(등록 검사가 필드 이름·종류를 본다).
    Span<const MaterialPropertyTextDefault> textDefaults;
    Span<const MaterialPinDefinition> pins;
    // 선택이다(개수가 속성으로 정해지는 핀 — 임의 HLSL·함수 호출·Switch).
    nodegraph::DynamicPinsFn dynamicPins = nullptr;
    // 선택이다. 입력은 정적 입력 핀 키, 출력은 정적 출력 핀 키다.
    Span<const MaterialPassThrough> passThrough;
    nodegraph::NodeMenuInfo menu;
    nodegraph::NodeCapabilities capabilities = nodegraph::DefaultNodeCapabilities;
    nodegraph::MigrateFn migrate = nullptr;
    MaterialNodeFlags flags = MaterialNodeFlags::None;
    MaterialGraphKindMask graphKinds = AllMaterialGraphKinds;
    MaterialStageMask stages = AllMaterialStages;
    // 이 노드가 읽는 문맥 공급 기능이다(속성에 따라 줄 수 있다 — 실제 집합은 번역이 기록한다).
    MaterialContextFeatureMask features = NoMaterialContextFeatures;
    MaterialNodeStatus status = MaterialNodeStatus::Implemented;
    MaterialPrerequisite prerequisite = MaterialPrerequisite::None;

    // 아래 설정 함수는 값을 바꾼 사본을 돌려준다(constexpr 사슬 — 정적 정의 표를 쓰기 위함).
    constexpr MaterialNodeDefinition Version(uint32 value, nodegraph::MigrateFn fn) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.schemaVersion = value;
        copy.migrate = fn;
        return copy;
    }

    constexpr MaterialNodeDefinition
    Properties(MaterialPropertiesSchemaFn value,
               Span<const StringView> nonSemantic = {}) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.propertiesSchema = value;
        copy.nonSemanticFields = nonSemantic;
        return copy;
    }

    constexpr MaterialNodeDefinition
    TextDefaults(Span<const MaterialPropertyTextDefault> value) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.textDefaults = value;
        return copy;
    }

    constexpr MaterialNodeDefinition Pins(Span<const MaterialPinDefinition> value) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.pins = value;
        return copy;
    }

    constexpr MaterialNodeDefinition DynamicPins(nodegraph::DynamicPinsFn value) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.dynamicPins = value;
        return copy;
    }

    constexpr MaterialNodeDefinition
    PassThrough(Span<const MaterialPassThrough> value) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.passThrough = value;
        return copy;
    }

    constexpr MaterialNodeDefinition Menu(StringView displayName, StringView category,
                                          StringView keywords, StringView tooltip) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.menu.displayName = displayName;
        copy.menu.category = category;
        copy.menu.keywords = keywords;
        copy.menu.tooltip = tooltip;
        return copy;
    }

    constexpr MaterialNodeDefinition
    Capabilities(nodegraph::NodeCapabilities value) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.capabilities = value;
        return copy;
    }

    constexpr MaterialNodeDefinition Flags(MaterialNodeFlags value) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.flags = value;
        return copy;
    }

    constexpr MaterialNodeDefinition GraphKinds(MaterialGraphKindMask value) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.graphKinds = value;
        return copy;
    }

    constexpr MaterialNodeDefinition Stages(MaterialStageMask value) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.stages = value;
        return copy;
    }

    constexpr MaterialNodeDefinition Features(MaterialContextFeatureMask value) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.features = value;
        return copy;
    }

    // 상태를 PrerequisiteMissing("TODO" 상태)으로 하고 선행을 정한다(19.2절).
    constexpr MaterialNodeDefinition Missing(MaterialPrerequisite value) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.status = MaterialNodeStatus::PrerequisiteMissing;
        copy.prerequisite = value;
        return copy;
    }

    // 검사용 원시 설정이다(정의 검사 시험이 일관되지 않은 값을 만든다).
    constexpr MaterialNodeDefinition RawStatus(MaterialNodeStatus value,
                                               MaterialPrerequisite required) const noexcept {
        MaterialNodeDefinition copy = *this;
        copy.status = value;
        copy.prerequisite = required;
        return copy;
    }
};

// 종류 이름만 정한 정의다. 나머지는 설정 함수로 채운다.
//
// 사용 예:
//   constexpr MaterialNodeDefinition add =
//       MaterialNode("Material.Add").Pins(addPins).Menu("Add", "Math", "+", "Adds two values.");
constexpr MaterialNodeDefinition MaterialNode(StringView type) noexcept {
    MaterialNodeDefinition definition;
    definition.type = type;
    return definition;
}

} // namespace rocks::material
