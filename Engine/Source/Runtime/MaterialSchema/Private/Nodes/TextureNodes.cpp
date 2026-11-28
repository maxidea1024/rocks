#include "MaterialBuiltinNodes.h"

#include "MaterialDynamicPins.h"
#include <MaterialSchema/Nodes/Texture/AntialiasedTextureMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/AntialiasedTextureMaskProperties.h>
#include <MaterialSchema/Nodes/Texture/ParticleSubUvProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/ParticleSubUvProperties.h>
#include <MaterialSchema/Nodes/Texture/PathTracingBufferTextureProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/PathTracingBufferTextureProperties.h>
#include <MaterialSchema/Nodes/Texture/SceneColorProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/SceneColorProperties.h>
#include <MaterialSchema/Nodes/Texture/SceneTextureProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/SceneTextureProperties.h>
#include <MaterialSchema/Nodes/Texture/SparseVolumeTextureObjectParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/SparseVolumeTextureObjectParameterProperties.h>
#include <MaterialSchema/Nodes/Texture/SparseVolumeTextureObjectProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/SparseVolumeTextureObjectProperties.h>
#include <MaterialSchema/Nodes/Texture/SparseVolumeTextureSampleParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/SparseVolumeTextureSampleParameterProperties.h>
#include <MaterialSchema/Nodes/Texture/SparseVolumeTextureSampleProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/SparseVolumeTextureSampleProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureCollectionParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureCollectionParameterProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureCollectionProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureCollectionProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectFromCollectionProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectFromCollectionProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectParameterProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.h>
#include <MaterialSchema/Nodes/Texture/TexturePropertyProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TexturePropertyProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameter2DArrayProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameter2DArrayProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameter2DProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameter2DProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterCubeArrayProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterCubeArrayProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterCubeProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterCubeProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterSubUvProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterSubUvProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterVolumeProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleParameterVolumeProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleProperties.h>
#include <MaterialSchema/Nodes/Texture/UserSceneTextureProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/UserSceneTextureProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Texture 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

// 속성 에셋 참조 선언이다(Docs/Design/MaterialSystem 19.4.3절 "참조 선언").
constexpr MaterialAssetReferenceField TextureReference[] = {
    {"texture", MaterialAssetReferenceRole::Texture}};

constexpr StringView TextureObjectParameterNonSemantic[] = {"texture", "channelNames", "group",
                                                            "sortPriority"};
constexpr MaterialPinDefinition TextureSamplePins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("TextureObject", MaterialPinType::TextureAny, "TextureObject", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("AutomaticViewMipBiasValue", MaterialPinType::StaticBool, "Apply View MipBias",
                  {}, MaterialPinFlags::StaticOnly | MaterialPinFlags::HideValue),
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("R", MaterialPinType::Float1, "R"),
    MaterialOutput("G", MaterialPinType::Float1, "G"),
    MaterialOutput("B", MaterialPinType::Float1, "B"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
};

constexpr StringView TextureSampleParameter2DNonSemantic[] = {"texture", "channelNames", "group",
                                                              "sortPriority"};
constexpr MaterialPinDefinition TextureSampleParameter2DPins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("AutomaticViewMipBiasValue", MaterialPinType::StaticBool, "Apply View MipBias",
                  {}, MaterialPinFlags::StaticOnly | MaterialPinFlags::HideValue),
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("R", MaterialPinType::Float1, "R"),
    MaterialOutput("G", MaterialPinType::Float1, "G"),
    MaterialOutput("B", MaterialPinType::Float1, "B"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
};

constexpr StringView TextureSampleParameter2DArrayNonSemantic[] = {"texture", "channelNames",
                                                                   "group", "sortPriority"};
constexpr MaterialPinDefinition TextureSampleParameter2DArrayPins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("AutomaticViewMipBiasValue", MaterialPinType::StaticBool, "Apply View MipBias",
                  {}, MaterialPinFlags::StaticOnly | MaterialPinFlags::HideValue),
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("R", MaterialPinType::Float1, "R"),
    MaterialOutput("G", MaterialPinType::Float1, "G"),
    MaterialOutput("B", MaterialPinType::Float1, "B"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
};

constexpr StringView TextureSampleParameterCubeNonSemantic[] = {"texture", "channelNames", "group",
                                                                "sortPriority"};
constexpr MaterialPinDefinition TextureSampleParameterCubePins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("AutomaticViewMipBiasValue", MaterialPinType::StaticBool, "Apply View MipBias",
                  {}, MaterialPinFlags::StaticOnly | MaterialPinFlags::HideValue),
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("R", MaterialPinType::Float1, "R"),
    MaterialOutput("G", MaterialPinType::Float1, "G"),
    MaterialOutput("B", MaterialPinType::Float1, "B"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
};

constexpr StringView TextureSampleParameterCubeArrayNonSemantic[] = {"texture", "channelNames",
                                                                     "group", "sortPriority"};
constexpr MaterialPinDefinition TextureSampleParameterCubeArrayPins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("AutomaticViewMipBiasValue", MaterialPinType::StaticBool, "Apply View MipBias",
                  {}, MaterialPinFlags::StaticOnly | MaterialPinFlags::HideValue),
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("R", MaterialPinType::Float1, "R"),
    MaterialOutput("G", MaterialPinType::Float1, "G"),
    MaterialOutput("B", MaterialPinType::Float1, "B"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
};

constexpr StringView TextureSampleParameterVolumeNonSemantic[] = {"texture", "channelNames",
                                                                  "group", "sortPriority"};
constexpr MaterialPinDefinition TextureSampleParameterVolumePins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("AutomaticViewMipBiasValue", MaterialPinType::StaticBool, "Apply View MipBias",
                  {}, MaterialPinFlags::StaticOnly | MaterialPinFlags::HideValue),
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("R", MaterialPinType::Float1, "R"),
    MaterialOutput("G", MaterialPinType::Float1, "G"),
    MaterialOutput("B", MaterialPinType::Float1, "B"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
};

constexpr StringView TextureSampleParameterSubUvNonSemantic[] = {"texture", "channelNames", "group",
                                                                 "sortPriority"};
constexpr MaterialPinDefinition TextureSampleParameterSubUvPins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("AutomaticViewMipBiasValue", MaterialPinType::StaticBool, "Apply View MipBias",
                  {}, MaterialPinFlags::StaticOnly | MaterialPinFlags::HideValue),
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("R", MaterialPinType::Float1, "R"),
    MaterialOutput("G", MaterialPinType::Float1, "G"),
    MaterialOutput("B", MaterialPinType::Float1, "B"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
};

constexpr MaterialPinDefinition TexturePropertyPins[] = {
    MaterialInput("TextureObject", MaterialPinType::TextureAny, "TextureObject", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr StringView AntialiasedTextureMaskNonSemantic[] = {"texture", "channelNames", "group",
                                                            "sortPriority"};
constexpr MaterialPinDefinition AntialiasedTextureMaskPins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("AutomaticViewMipBiasValue", MaterialPinType::StaticBool, "Apply View MipBias",
                  {}, MaterialPinFlags::StaticOnly | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Float1, ""),
};

constexpr MaterialPinDefinition RecordTextureStreamingInfoPins[] = {
    MaterialInput("TextureObject", MaterialPinType::Texture2D, "TextureObject", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Coordinates", MaterialPinType::Float2, "Coordinates", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Tex", MaterialPinType::Texture2D, "Tex"),
};

constexpr MaterialPinDefinition TextureCollectionPins[] = {
    MaterialOutput("TextureCollection", MaterialPinType::TextureCollection, "TextureCollection"),
    MaterialOutput("TextureCount", MaterialPinType::UInt1, "TextureCount"),
};

constexpr StringView TextureCollectionParameterNonSemantic[] = {"textureCollection", "group",
                                                                "sortPriority"};
constexpr MaterialPinDefinition TextureCollectionParameterPins[] = {
    MaterialOutput("TextureCollection", MaterialPinType::TextureCollection, "TextureCollection"),
    MaterialOutput("TextureCount", MaterialPinType::UInt1, "TextureCount"),
};

constexpr MaterialPinDefinition TextureObjectFromCollectionPins[] = {
    MaterialInput("TextureCollection", MaterialPinType::TextureCollection, "TextureCollection", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("CollectionIndex", MaterialPinType::UInt1, "CollectionIndex",
                  MaterialPinDefault::Scalar(0.0f)),
};

constexpr MaterialPinDefinition ParticleSubUvPins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("TextureObject", MaterialPinType::TextureAny, "TextureObject", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("AutomaticViewMipBiasValue", MaterialPinType::StaticBool, "Apply View MipBias",
                  {}, MaterialPinFlags::StaticOnly | MaterialPinFlags::HideValue),
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("R", MaterialPinType::Float1, "R"),
    MaterialOutput("G", MaterialPinType::Float1, "G"),
    MaterialOutput("B", MaterialPinType::Float1, "B"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
};

constexpr MaterialPinDefinition SceneTexturePins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Color", MaterialPinType::Float4, "Color"),
    MaterialOutput("Size", MaterialPinType::Numeric, "Size"),
    MaterialOutput("InvSize", MaterialPinType::Numeric, "InvSize"),
};

constexpr MaterialPinDefinition SceneColorPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input",
                  MaterialPinDefault::Vector2(0.0f, 0.0f)),
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
};

constexpr MaterialPinDefinition UserSceneTexturePins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Color", MaterialPinType::Float4, "Color"),
    MaterialOutput("Size", MaterialPinType::Numeric, "Size"),
    MaterialOutput("InvSize", MaterialPinType::Numeric, "InvSize"),
};

constexpr MaterialPinDefinition SparseVolumeTextureObjectPins[] = {
    MaterialOutput("Result", MaterialPinType::SparseVolumeTexture, ""),
};

constexpr StringView SparseVolumeTextureObjectParameterNonSemantic[] = {"sparseVolumeTexture",
                                                                        "group", "sortPriority"};
constexpr MaterialPinDefinition SparseVolumeTextureObjectParameterPins[] = {
    MaterialOutput("Result", MaterialPinType::SparseVolumeTexture, ""),
};

constexpr MaterialPinDefinition SparseVolumeTextureSamplePins[] = {
    MaterialInput("Coordinates", MaterialPinType::Float3, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("TextureObject", MaterialPinType::SparseVolumeTexture, "TextureObject", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Attributes A", MaterialPinType::Float4, "Attributes A"),
    MaterialOutput("Attributes B", MaterialPinType::Float4, "Attributes B"),
};

constexpr StringView SparseVolumeTextureSampleParameterNonSemantic[] = {"sparseVolumeTexture",
                                                                        "group", "sortPriority"};
constexpr MaterialPinDefinition SparseVolumeTextureSampleParameterPins[] = {
    MaterialInput("Coordinates", MaterialPinType::Float3, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("TextureObject", MaterialPinType::SparseVolumeTexture, "TextureObject", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Attributes A", MaterialPinType::Float4, "Attributes A"),
    MaterialOutput("Attributes B", MaterialPinType::Float4, "Attributes B"),
};

constexpr MaterialPinDefinition PathTracingBufferTexturePins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
};

constexpr MaterialNodeDefinition TextureDefinitions[] = {
    MaterialNode("Material.TextureObject")
        .AssetReferences(TextureReference)
        .Properties(&GeneratedType<TextureObjectProperties>::Schema)
        .DynamicPins(&TextureObjectOutputPins<TextureObjectProperties>)
        .Menu("Texture Object", "Texture", "Texture Functions", "Texture Object."),
    MaterialNode("Material.TextureObjectParameter")
        .AssetReferences(TextureReference)
        .Properties(&GeneratedType<TextureObjectParameterProperties>::Schema,
                    TextureObjectParameterNonSemantic)
        .DynamicPins(&TextureObjectOutputPins<TextureObjectParameterProperties>)
        .Menu(
            "Param Tex Object", "Texture", "Texture Parameters",
            "Node which creates a texture parameter and outputs the texture object itself, instead "
            "of sampling the texture first. This is used with material functions to implement "
            "texture parameters without actually putting the parameter in the function.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.TextureSample")
        .AssetReferences(TextureReference)
        .Properties(&GeneratedType<TextureSampleProperties>::Schema)
        .Pins(TextureSamplePins)
        .DynamicPins(&TextureSampleMipPins<TextureSampleProperties>)
        .Menu("Texture Sample", "Texture", "Texture", "Texture Sample."),
    MaterialNode("Material.TextureSampleParameter2D")
        .AssetReferences(TextureReference)
        .Properties(&GeneratedType<TextureSampleParameter2DProperties>::Schema,
                    TextureSampleParameter2DNonSemantic)
        .Pins(TextureSampleParameter2DPins)
        .DynamicPins(&TextureSampleMipPins<TextureSampleParameter2DProperties>)
        .Menu("Param2D", "Texture", "Texture Parameters", "Param2D.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.TextureSampleParameter2DArray")
        .AssetReferences(TextureReference)
        .Properties(&GeneratedType<TextureSampleParameter2DArrayProperties>::Schema,
                    TextureSampleParameter2DArrayNonSemantic)
        .Pins(TextureSampleParameter2DArrayPins)
        .DynamicPins(&TextureSampleMipPins<TextureSampleParameter2DArrayProperties>)
        .Menu("Param2DArray", "Texture", "Texture Parameters", "Param2DArray.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.TextureSampleParameterCube")
        .AssetReferences(TextureReference)
        .Properties(&GeneratedType<TextureSampleParameterCubeProperties>::Schema,
                    TextureSampleParameterCubeNonSemantic)
        .Pins(TextureSampleParameterCubePins)
        .DynamicPins(&TextureSampleMipPins<TextureSampleParameterCubeProperties>)
        .Menu("ParamCube", "Texture", "Texture Parameters", "ParamCube.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.TextureSampleParameterCubeArray")
        .AssetReferences(TextureReference)
        .Properties(&GeneratedType<TextureSampleParameterCubeArrayProperties>::Schema,
                    TextureSampleParameterCubeArrayNonSemantic)
        .Pins(TextureSampleParameterCubeArrayPins)
        .DynamicPins(&TextureSampleMipPins<TextureSampleParameterCubeArrayProperties>)
        .Menu("ParamCubeArray", "Texture", "Texture Parameters", "ParamCubeArray.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.TextureSampleParameterVolume")
        .AssetReferences(TextureReference)
        .Properties(&GeneratedType<TextureSampleParameterVolumeProperties>::Schema,
                    TextureSampleParameterVolumeNonSemantic)
        .Pins(TextureSampleParameterVolumePins)
        .DynamicPins(&TextureSampleMipPins<TextureSampleParameterVolumeProperties>)
        .Menu("ParamVolume", "Texture", "Texture Parameters", "ParamVolume.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.TextureSampleParameterSubUv")
        .AssetReferences(TextureReference)
        .Properties(&GeneratedType<TextureSampleParameterSubUvProperties>::Schema,
                    TextureSampleParameterSubUvNonSemantic)
        .Pins(TextureSampleParameterSubUvPins)
        .DynamicPins(&TextureSampleMipPins<TextureSampleParameterSubUvProperties>)
        .Menu("Texture Sample Parameter Sub Uv", "Texture",
              "Texture Parameters Particles TextureSampleParameterSubUV",
              "Texture Sample Parameter Sub Uv.")
        .Flags(MaterialNodeFlags::Parameter)
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.TextureProperty")
        .Properties(&GeneratedType<TexturePropertyProperties>::Schema)
        .Pins(TexturePropertyPins)
        .Menu("Texture Property", "Texture", "Texture", "Texture Property."),
    MaterialNode("Material.AntialiasedTextureMask")
        .AssetReferences(TextureReference)
        .Properties(&GeneratedType<AntialiasedTextureMaskProperties>::Schema,
                    AntialiasedTextureMaskNonSemantic)
        .Pins(AntialiasedTextureMaskPins)
        .DynamicPins(&TextureSampleMipPins<AntialiasedTextureMaskProperties>)
        .Menu("AAMasked Param2D", "Texture", "Utility", "AAMasked Param2D.")
        .Flags(MaterialNodeFlags::Parameter),
    MaterialNode("Material.RecordTextureStreamingInfo")
        .Pins(RecordTextureStreamingInfoPins)
        .Menu("Record Texture Streaming", "Texture", "Texture",
              "Adds functionality to record the material UV scales for use by the automatic "
              "texture streaming system."),
    MaterialNode("Material.TextureCollection")
        .Properties(&GeneratedType<TextureCollectionProperties>::Schema)
        .Pins(TextureCollectionPins)
        .Menu("Texture Collection", "Texture", "Texture", "Texture Collection.")
        .Missing(MaterialPrerequisite::TextureCollection),
    MaterialNode("Material.TextureCollectionParameter")
        .Properties(&GeneratedType<TextureCollectionParameterProperties>::Schema,
                    TextureCollectionParameterNonSemantic)
        .Pins(TextureCollectionParameterPins)
        .Menu("Texture Collection Parameter", "Texture", "Texture Parameters",
              "Texture Collection Parameter.")
        .Flags(MaterialNodeFlags::Parameter)
        .Missing(MaterialPrerequisite::TextureCollection),
    MaterialNode("Material.TextureObjectFromCollection")
        .Properties(&GeneratedType<TextureObjectFromCollectionProperties>::Schema)
        .Pins(TextureObjectFromCollectionPins)
        .DynamicPins(&TextureObjectFromCollectionDynamicPins)
        .Menu("Texture Object From Collection", "Texture", "Texture",
              "Texture Object From Collection.")
        .Missing(MaterialPrerequisite::TextureCollection),
    MaterialNode("Material.ParticleSubUv")
        .AssetReferences(TextureReference)
        .Properties(&GeneratedType<ParticleSubUvProperties>::Schema)
        .Pins(ParticleSubUvPins)
        .DynamicPins(&TextureSampleMipPins<ParticleSubUvProperties>)
        .Menu("Particle SubUV", "Texture", "Texture Particles ParticleSubUV", "Particle SubUV.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.SceneTexture")
        .Properties(&GeneratedType<SceneTextureProperties>::Schema)
        .Pins(SceneTexturePins)
        .Menu("Scene Texture", "Texture", "Texture", "Scene Texture.")
        .Features(ToMask(MaterialContextFeature::Scene)),
    MaterialNode("Material.SceneColor")
        .Properties(&GeneratedType<SceneColorProperties>::Schema)
        .Pins(SceneColorPins)
        .Menu("Scene Color", "Texture", "Texture", "Scene Color.")
        .Features(ToMask(MaterialContextFeature::Scene)),
    MaterialNode("Material.UserSceneTexture")
        .Properties(&GeneratedType<UserSceneTextureProperties>::Schema)
        .Pins(UserSceneTexturePins)
        .Menu("User Scene Texture", "Texture", "Texture", "User Scene Texture.")
        .Features(ToMask(MaterialContextFeature::Scene)),
    MaterialNode("Material.SparseVolumeTextureObject")
        .Properties(&GeneratedType<SparseVolumeTextureObjectProperties>::Schema)
        .Pins(SparseVolumeTextureObjectPins)
        .Menu("SparseVolumeTexture Object", "Texture", "Texture Functions",
              "SparseVolumeTexture Object.")
        .Missing(MaterialPrerequisite::SparseVolumeTexture),
    MaterialNode("Material.SparseVolumeTextureObjectParameter")
        .Properties(&GeneratedType<SparseVolumeTextureObjectParameterProperties>::Schema,
                    SparseVolumeTextureObjectParameterNonSemantic)
        .Pins(SparseVolumeTextureObjectParameterPins)
        .Menu("Param Tex Object", "Texture", "Texture Parameters", "Param Tex Object.")
        .Flags(MaterialNodeFlags::Parameter)
        .Missing(MaterialPrerequisite::SparseVolumeTexture),
    MaterialNode("Material.SparseVolumeTextureSample")
        .Properties(&GeneratedType<SparseVolumeTextureSampleProperties>::Schema)
        .Pins(SparseVolumeTextureSamplePins)
        .DynamicPins(&SparseVolumeTextureSampleMipPins<SparseVolumeTextureSampleProperties>)
        .Menu("Sparse Volume Texture Sample", "Texture", "Texture",
              "Material expression for sampling from a runtime virtual texture.")
        .Missing(MaterialPrerequisite::SparseVolumeTexture),
    MaterialNode("Material.SparseVolumeTextureSampleParameter")
        .Properties(&GeneratedType<SparseVolumeTextureSampleParameterProperties>::Schema,
                    SparseVolumeTextureSampleParameterNonSemantic)
        .Pins(SparseVolumeTextureSampleParameterPins)
        .DynamicPins(
            &SparseVolumeTextureSampleMipPins<SparseVolumeTextureSampleParameterProperties>)
        .Menu("Sparse Volume Texture Sample Parameter", "Texture", "Texture Parameters",
              "Sparse Volume Texture Sample Parameter.")
        .Flags(MaterialNodeFlags::Parameter)
        .Missing(MaterialPrerequisite::SparseVolumeTexture),
    MaterialNode("Material.PathTracingBufferTexture")
        .Properties(&GeneratedType<PathTracingBufferTextureProperties>::Schema)
        .Pins(PathTracingBufferTexturePins)
        .Menu("Path Tracing Buffer Texture", "Texture", "PathTracing",
              "Path tracing buffer is only accessable in postprocess material.")
        .Features(ToMask(MaterialContextFeature::PathTracing)),
};

} // namespace

Span<const MaterialNodeDefinition> TextureNodeDefinitions() noexcept {
    return TextureDefinitions;
}

} // namespace rocks::material::detail
