#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>
#include <MaterialSchema/MaterialPinDefault.h>
#include <MaterialSchema/MaterialPinType.h>
#include <NodeGraph/GraphDocument.h>

// 머티리얼 노드 핀 하나의 정적 정의다(Docs/Design/MaterialSystem 5.2·19.13.3절).
// MaterialNodeDefinition의 핀 배열 원소이고, 동적 핀 함수가 아닌 정적 핀만 이 형식으로 둔다.
// 키·표시 이름은 정적 data다.
namespace rocks::material {

// 입력 핀 표지 비트다.
enum class MaterialPinFlags : uint8 {
    None = 0,
    // 연결이 없으면 번역 오류다(기본값이 있어도 쓰지 않는다).
    Required = 1u << 0,
    // static 값만 받는다(V7).
    StaticOnly = 1u << 1,
    // 고급 표시 묶음이다(NodeGraph PinDescriptor::advanced).
    Advanced = 1u << 2,
    // 사용자 기본값 편집을 숨긴다(NodeGraph PinValuePolicy::Hidden).
    HideValue = 1u << 3
};

constexpr MaterialPinFlags operator|(MaterialPinFlags left, MaterialPinFlags right) noexcept {
    return static_cast<MaterialPinFlags>(static_cast<uint8>(left) | static_cast<uint8>(right));
}

constexpr bool HasPinFlag(MaterialPinFlags set, MaterialPinFlags flag) noexcept {
    return (static_cast<uint8>(set) & static_cast<uint8>(flag)) == static_cast<uint8>(flag);
}

// 정의된 핀 표지 비트 전체다.
inline constexpr uint8 MaterialPinFlagMask = (1u << 4) - 1;

// 핀 하나의 정의다.
struct MaterialPinDefinition {
    // 안정 키다(PinKey 원문). 19.5절: UE 입력 이름을 기본으로 쓴다.
    StringView key;
    nodegraph::PinDirection direction = nodegraph::PinDirection::Input;
    MaterialPinType type = MaterialPinType::Numeric;
    // 영어 표시 이름이다.
    StringView displayName;
    MaterialPinDefault defaultValue;
    MaterialPinFlags flags = MaterialPinFlags::None;
};

constexpr MaterialPinDefinition
MaterialInput(StringView key, MaterialPinType type, StringView displayName,
              MaterialPinDefault defaultValue = {},
              MaterialPinFlags flags = MaterialPinFlags::None) noexcept {
    MaterialPinDefinition pin;
    pin.key = key;
    pin.direction = nodegraph::PinDirection::Input;
    pin.type = type;
    pin.displayName = displayName;
    pin.defaultValue = defaultValue;
    pin.flags = flags;
    return pin;
}

constexpr MaterialPinDefinition MaterialOutput(StringView key, MaterialPinType type,
                                               StringView displayName) noexcept {
    MaterialPinDefinition pin;
    pin.key = key;
    pin.direction = nodegraph::PinDirection::Output;
    pin.type = type;
    pin.displayName = displayName;
    return pin;
}

} // namespace rocks::material
