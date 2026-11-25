#include <MaterialSchema/MaterialPinType.h>

// 머티리얼 핀 타입 표다(Docs/Design/MaterialSystem 5.3절).
namespace rocks::material {

namespace {

// MaterialPinType 값 순서와 같다.
constexpr nodegraph::PinTypeInfo PinTypeInfos[MaterialPinTypeCount] = {
    {"Numeric"},
    {"Float1"},
    {"Float2"},
    {"Float3"},
    {"Float4"},
    {"Int1"},
    {"Int2"},
    {"Int3"},
    {"Int4"},
    {"UInt1"},
    {"UInt2"},
    {"UInt3"},
    {"UInt4"},
    {"Bool"},
    {"Float3x3"},
    {"Float4x4"},
    {"Double1"},
    {"Double2"},
    {"Double3"},
    {"Double4"},
    {"Texture2D"},
    {"Texture2DArray"},
    {"TextureCube"},
    {"TextureCubeArray"},
    {"Texture3D"},
    {"TextureAny"},
    {"TextureExternal"},
    {"TextureVirtual"},
    {"SparseVolumeTexture"},
    {"TextureCollection"},
    {"TextureMeshPaint"},
    {"TextureMaterialCache"},
    {"StaticBool"},
    {"StaticInt"},
    {"Surface"},
    {"MaterialAttributes"},
    {"ShadingModel"},
    {"StaticEnum"},
};

constexpr MaterialPinTypeClass PinTypeClasses[MaterialPinTypeCount] = {
    MaterialPinTypeClass::Numeric,      MaterialPinTypeClass::Numeric,
    MaterialPinTypeClass::Numeric,      MaterialPinTypeClass::Numeric,
    MaterialPinTypeClass::Numeric,      MaterialPinTypeClass::Numeric,
    MaterialPinTypeClass::Numeric,      MaterialPinTypeClass::Numeric,
    MaterialPinTypeClass::Numeric,      MaterialPinTypeClass::Numeric,
    MaterialPinTypeClass::Numeric,      MaterialPinTypeClass::Numeric,
    MaterialPinTypeClass::Numeric,      MaterialPinTypeClass::Numeric,
    MaterialPinTypeClass::Numeric,      MaterialPinTypeClass::Numeric,
    MaterialPinTypeClass::Numeric,      MaterialPinTypeClass::Numeric,
    MaterialPinTypeClass::Numeric,      MaterialPinTypeClass::Numeric,
    MaterialPinTypeClass::Texture,      MaterialPinTypeClass::Texture,
    MaterialPinTypeClass::Texture,      MaterialPinTypeClass::Texture,
    MaterialPinTypeClass::Texture,      MaterialPinTypeClass::TextureAny,
    MaterialPinTypeClass::Pending,      MaterialPinTypeClass::Pending,
    MaterialPinTypeClass::Pending,      MaterialPinTypeClass::Pending,
    MaterialPinTypeClass::Pending,      MaterialPinTypeClass::Pending,
    MaterialPinTypeClass::Static,       MaterialPinTypeClass::Static,
    MaterialPinTypeClass::Surface,      MaterialPinTypeClass::Attributes,
    MaterialPinTypeClass::ShadingModel, MaterialPinTypeClass::Static,
};

constexpr nodegraph::PinTypeTable PinTypeTableValue{PinTypeInfos};

static_assert(static_cast<uint32>(MaterialPinType::StaticEnum) + 1 == MaterialPinTypeCount,
              "MaterialPinTypeCount must follow the last MaterialPinType value");

} // namespace

const nodegraph::PinTypeTable& MaterialPinTypes() noexcept {
    return PinTypeTableValue;
}

StringView MaterialPinTypeName(MaterialPinType type) noexcept {
    const auto index = static_cast<uint32>(type);
    if (index >= MaterialPinTypeCount) {
        return "Invalid";
    }

    return PinTypeInfos[index].name;
}

MaterialPinTypeClass ClassifyMaterialPinType(MaterialPinType type) noexcept {
    const auto index = static_cast<uint32>(type);
    if (index >= MaterialPinTypeCount) {
        return MaterialPinTypeClass::Pending;
    }

    return PinTypeClasses[index];
}

} // namespace rocks::material
