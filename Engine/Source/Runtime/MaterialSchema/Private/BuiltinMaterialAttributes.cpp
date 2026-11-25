#include <MaterialSchema/MaterialAttributeTable.h>

// 내장 머티리얼 속성 표다(Docs/Design/MaterialSystem 19.12절). Guid·타입·기본값·빈도는 UE
// RE/Private/Materials/MaterialAttributeDefinitionMap.cpp:395–434와 같다(UE FGuid(A, B, C, D)는
// Guid{A:B, C:D}). 이 파일은 그 표를 옮긴 것이므로 값을 바꾸지 않는다.
namespace rocks::material {

namespace {

constexpr MaterialAttributeDefinition Attribute(Guid id, StringView name, MaterialPinType type,
                                                MaterialPinDefault value,
                                                MaterialStageMask frequency,
                                                bool hidden = false) noexcept {
    MaterialAttributeDefinition definition;
    definition.id = id;
    definition.name = name;
    definition.type = type;
    definition.defaultValue = value;
    definition.frequency = frequency;
    definition.hidden = hidden;
    return definition;
}

constexpr MaterialAttributeDefinition BuiltinAttributes[] = {
    // UE 395행.
    Attribute(Guid{0x69B8D33616ED4D49ull, 0x9AA497292F050F7Aull}, "BaseColor",
              MaterialPinType::Float3, MaterialPinDefault::Vector3(0.0f, 0.0f, 0.0f),
              MaterialPixelStage),
    // UE 396행.
    Attribute(Guid{0x57C3A1617F064296ull, 0xB00B24A5A496F34Cull}, "Metallic",
              MaterialPinType::Float1, MaterialPinDefault::Scalar(0.0f), MaterialPixelStage),
    // UE 397행.
    Attribute(Guid{0x9FDAB39925564CC9ull, 0x8CD2D572C12C8FEDull}, "Specular",
              MaterialPinType::Float1, MaterialPinDefault::Scalar(0.5f), MaterialPixelStage),
    // UE 398행.
    Attribute(Guid{0xD1DD967C4CAD47D3ull, 0x9E6346FB08ECF210ull}, "Roughness",
              MaterialPinType::Float1, MaterialPinDefault::Scalar(0.5f), MaterialPixelStage),
    // UE 399행.
    Attribute(Guid{0x55E2B4FBC1C54DB2ull, 0x9F11875F7231EB1Eull}, "Anisotropy",
              MaterialPinType::Float1, MaterialPinDefault::Scalar(0.0f), MaterialPixelStage),
    // UE 400행.
    Attribute(Guid{0xB769B54DD08D4440ull, 0xABC21BA6CD27D0E2ull}, "EmissiveColor",
              MaterialPinType::Float3, MaterialPinDefault::Vector3(0.0f, 0.0f, 0.0f),
              MaterialPixelStage),
    // UE 401행.
    Attribute(Guid{0xB8F50FBA2A754EC1ull, 0x9EF672CFEB27BF51ull}, "Opacity",
              MaterialPinType::Float1, MaterialPinDefault::Scalar(1.0f), MaterialPixelStage),
    // UE 402행.
    Attribute(Guid{0x679FFB172BB5422Cull, 0xAD520483166E0C75ull}, "OpacityMask",
              MaterialPinType::Float1, MaterialPinDefault::Scalar(1.0f), MaterialPixelStage),
    // UE 403행.
    Attribute(Guid{0x0FA2821A200F4A4Aull, 0xB719B789C1259C64ull}, "Normal", MaterialPinType::Float3,
              MaterialPinDefault::Vector3(0.0f, 0.0f, 1.0f), MaterialPixelStage),
    // UE 404행.
    Attribute(Guid{0xD5F8E9CFCDC3468Dull, 0xB10E4465596A7BBAull}, "Tangent",
              MaterialPinType::Float3, MaterialPinDefault::Vector3(1.0f, 0.0f, 0.0f),
              MaterialPixelStage),
    // UE 407행.
    Attribute(Guid{0xF905F895D5814314ull, 0x916D24348C40CE9Eull}, "WorldPositionOffset",
              MaterialPinType::Float3, MaterialPinDefault::Vector3(0.0f, 0.0f, 0.0f),
              MaterialVertexStage),
    // UE 408행.
    Attribute(Guid{0x199A7166C67041DCull, 0xA68EAD0D7017D0ADull}, "Displacement",
              MaterialPinType::Float1, MaterialPinDefault::Scalar(0.5f), MaterialPixelStage),
    // UE 409행.
    Attribute(Guid{0x5B8FC67951CE4082ull, 0x9D777BEEF4F72C44ull}, "SubsurfaceColor",
              MaterialPinType::Float3, MaterialPinDefault::Vector3(1.0f, 1.0f, 1.0f),
              MaterialPixelStage),
    // UE 410행.
    Attribute(Guid{0x9E502E693C8F48FAull, 0x94645CFD28E5428Dull}, "CustomData0",
              MaterialPinType::Float1, MaterialPinDefault::Scalar(1.0f), MaterialPixelStage),
    // UE 411행.
    Attribute(Guid{0xBE4F2FFD12FC4296ull, 0xB0124EEA12C28D92ull}, "CustomData1",
              MaterialPinType::Float1, MaterialPinDefault::Scalar(0.1f), MaterialPixelStage),
    // UE 412행.
    Attribute(Guid{0xE8EBD0ADB1654CBEull, 0xB079C3A8B39B9F15ull}, "AmbientOcclusion",
              MaterialPinType::Float1, MaterialPinDefault::Scalar(1.0f), MaterialPixelStage),
    // UE 413행.
    Attribute(Guid{0xD0B0FA0314D74455ull, 0xA851BAC581A0788Bull}, "Refraction",
              MaterialPinType::Float3, MaterialPinDefault::Vector3(1.0f, 0.0f, 0.0f),
              MaterialPixelStage),
    // UE 414행.
    Attribute(Guid{0x0AC97EC3E3D047BAull, 0xB610167DC4D919FFull}, "PixelDepthOffset",
              MaterialPinType::Float1, MaterialPinDefault::Scalar(0.0f), MaterialPixelStage),
    // UE 415행.
    Attribute(Guid{0xD9423FFFD77E4D82ull, 0x8FF9CF5E055D1255ull}, "ShadingModel",
              MaterialPinType::ShadingModel, {}, MaterialPixelStage),
    // UE 416행.
    Attribute(Guid{0x42BDD2E0BE714189ull, 0xA0984BC3DD0BE872ull}, "SurfaceThickness",
              MaterialPinType::Float1, MaterialPinDefault::Scalar(0.01f), MaterialPixelStage),
    // UE 417행.
    Attribute(Guid{0x5973A03E13A74E08ull, 0x92D0CEDDF2936CF8ull}, "FrontMaterial",
              MaterialPinType::Surface, {}, MaterialPixelStage),
    // UE 420행.
    Attribute(Guid{0xE0ED040B82794D93ull, 0xBD2D59B2A5BBF41Cull}, "MaterialAttributes",
              MaterialPinType::MaterialAttributes, {}, MaterialPixelStage, true),
    // UE 423행.
    Attribute(Guid{0xD30EC284E13A4160ull, 0x87BB52302ED115DCull}, "CustomizedUV0",
              MaterialPinType::Float2, MaterialPinDefault::Vector2(0.0f, 0.0f),
              MaterialVertexStage),
    // UE 424행.
    Attribute(Guid{0xC67B093C2A5249AAull, 0xABC97ADE4A1F49C5ull}, "CustomizedUV1",
              MaterialPinType::Float2, MaterialPinDefault::Vector2(0.0f, 0.0f),
              MaterialVertexStage),
    // UE 425행.
    Attribute(Guid{0x85C15B24F3E047CAull, 0x8585687201AE0F4Full}, "CustomizedUV2",
              MaterialPinType::Float2, MaterialPinDefault::Vector2(0.0f, 0.0f),
              MaterialVertexStage),
    // UE 426행.
    Attribute(Guid{0x777819DC31AE4676ull, 0xB864EF77B807E873ull}, "CustomizedUV3",
              MaterialPinType::Float2, MaterialPinDefault::Vector2(0.0f, 0.0f),
              MaterialVertexStage),
    // UE 427행.
    Attribute(Guid{0xDA63B233DDF44CADull, 0xB93D867B8DAFDBCCull}, "CustomizedUV4",
              MaterialPinType::Float2, MaterialPinDefault::Vector2(0.0f, 0.0f),
              MaterialVertexStage),
    // UE 428행.
    Attribute(Guid{0xC2F52B764A034388ull, 0x891195282071B190ull}, "CustomizedUV5",
              MaterialPinType::Float2, MaterialPinDefault::Vector2(0.0f, 0.0f),
              MaterialVertexStage),
    // UE 429행.
    Attribute(Guid{0x8214A8CA0CB944CFull, 0x9DFD78DBE48BB55Full}, "CustomizedUV6",
              MaterialPinType::Float2, MaterialPinDefault::Vector2(0.0f, 0.0f),
              MaterialVertexStage),
    // UE 430행.
    Attribute(Guid{0xD8F8D01FC6F74715ull, 0xA3CFB4FF9EF51FACull}, "CustomizedUV7",
              MaterialPinType::Float2, MaterialPinDefault::Vector2(0.0f, 0.0f),
              MaterialVertexStage),
    // UE 433행.
    Attribute(Guid{0x68934E1B70EB411Bull, 0x86DF5AA5DF2F626Cull}, "DiffuseColor",
              MaterialPinType::Float3, MaterialPinDefault::Vector3(0.0f, 0.0f, 0.0f),
              MaterialPixelStage, true),
    // UE 434행.
    Attribute(Guid{0xE89CBD8462EA48BEull, 0x80F885212B0C403Cull}, "SpecularColor",
              MaterialPinType::Float3, MaterialPinDefault::Vector3(0.0f, 0.0f, 0.0f),
              MaterialPixelStage, true),
};

} // namespace

Span<const MaterialAttributeDefinition> BuiltinMaterialAttributes() noexcept {
    return BuiltinAttributes;
}

} // namespace rocks::material
