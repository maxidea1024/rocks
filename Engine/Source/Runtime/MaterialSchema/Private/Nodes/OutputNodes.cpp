#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/MaterialGraphSchema.h>

// 출력 노드 정의다(Docs/Design/MaterialSystem 5.6·19.8절). 기본값은 UE 속성 정의
// (RE/Private/Materials/MaterialAttributeDefinitionMap.cpp:401–423)와 같다. 표면 속성 값은 출력
// 노드 핀이 아니라 Surface 입력 아래 BSDF 노드 핀이다(결정 MD1).
namespace rocks::material::detail {

namespace {

// UE SUBSTRATE_LAYER_DEFAULT_THICKNESS_CM(SH/Shared/SubstrateDefinitions.h:48)과 같다.
constexpr float DefaultSurfaceThicknessCm = 0.01f;
// UE Displacement 기본값(중립 높이)이다.
constexpr float DefaultDisplacement = 0.5f;

constexpr MaterialPinDefinition OutputPins[] = {
    MaterialInput("Surface", MaterialPinType::Surface, "Surface"),
    MaterialInput("Opacity", MaterialPinType::Numeric, "Opacity", MaterialPinDefault::Scalar(1.0f)),
    MaterialInput("OpacityMask", MaterialPinType::Numeric, "Opacity Mask",
                  MaterialPinDefault::Scalar(1.0f)),
    MaterialInput("AmbientOcclusion", MaterialPinType::Numeric, "Ambient Occlusion",
                  MaterialPinDefault::Scalar(1.0f)),
    MaterialInput("WorldPositionOffset", MaterialPinType::Numeric, "World Position Offset",
                  MaterialPinDefault::Vector3(0.0f, 0.0f, 0.0f)),
    MaterialInput("PixelDepthOffset", MaterialPinType::Numeric, "Pixel Depth Offset",
                  MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("Refraction", MaterialPinType::Numeric, "Refraction",
                  MaterialPinDefault::Vector3(1.0f, 0.0f, 0.0f)),
    MaterialInput("SurfaceThickness", MaterialPinType::Numeric, "Surface Thickness",
                  MaterialPinDefault::Scalar(DefaultSurfaceThicknessCm),
                  MaterialPinFlags::Advanced),
    MaterialInput("Displacement", MaterialPinType::Numeric, "Displacement",
                  MaterialPinDefault::Scalar(DefaultDisplacement), MaterialPinFlags::Advanced),
    // 기본값이 없다: 연결이 없으면 정점 입력의 같은 UV 채널을 그대로 쓴다(UE와 같음).
    MaterialInput("CustomizedUv0", MaterialPinType::Numeric, "Customized UV0", {},
                  MaterialPinFlags::Advanced),
    MaterialInput("CustomizedUv1", MaterialPinType::Numeric, "Customized UV1", {},
                  MaterialPinFlags::Advanced),
    MaterialInput("CustomizedUv2", MaterialPinType::Numeric, "Customized UV2", {},
                  MaterialPinFlags::Advanced),
    MaterialInput("CustomizedUv3", MaterialPinType::Numeric, "Customized UV3", {},
                  MaterialPinFlags::Advanced),
    MaterialInput("CustomizedUv4", MaterialPinType::Numeric, "Customized UV4", {},
                  MaterialPinFlags::Advanced),
    MaterialInput("CustomizedUv5", MaterialPinType::Numeric, "Customized UV5", {},
                  MaterialPinFlags::Advanced),
    MaterialInput("CustomizedUv6", MaterialPinType::Numeric, "Customized UV6", {},
                  MaterialPinFlags::Advanced),
    MaterialInput("CustomizedUv7", MaterialPinType::Numeric, "Customized UV7", {},
                  MaterialPinFlags::Advanced),
    // 머티리얼 속성 사용 표시(기본 속성 useMaterialAttributes)일 때만 쓴다(5.6절).
    MaterialInput("Attributes", MaterialPinType::MaterialAttributes, "Material Attributes", {},
                  MaterialPinFlags::Advanced),
};

constexpr MaterialNodeDefinition OutputDefinitions[] = {
    MaterialNode(MaterialOutputNodeTypeName)
        .Pins(OutputPins)
        .Menu("Material Output", "Output", "result root",
              "The material's outputs. Surface properties come from the Surface input.")
        // 문서당 하나, 지울 수 없음(UE 머티리얼 결과 노드와 같다).
        .Capabilities(nodegraph::NodeCapabilities::SinglePerDocument)
        .Flags(MaterialNodeFlags::HiddenFromPalette)
        .GraphKinds(ToMask(MaterialGraphKind::Material)),
};

} // namespace

Span<const MaterialNodeDefinition> OutputNodeDefinitions() noexcept {
    return OutputDefinitions;
}

} // namespace rocks::material::detail
