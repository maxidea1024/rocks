#include <MaterialSchema/MaterialContextValue.h>

// 문맥 값 표다(Docs/Design/MaterialSystem 19.3.2·19.13.5절). 원본은 UE 5.8
// BaseMaterialExpressions.ini의 외부 코드 선언 101개(CFG/BaseMaterialExpressions.ini:1038–)와
// 노드가 읽는 템플릿 필드다. 이름은 19.5절 규칙으로 바꿨고(Get 접두·WS 접미 제거, 상표 이름, DF →
// Double) UE 이름은 unrealName에 둔다. HLSL 정의는 두지 않는다(MS2b MaterialCommon.rsh의 일).
// 공급하지 않을 때의 값은 UE 템플릿과 같이 0이고 파티클 색만 1이다(결정 MD15 —
// SH/Private/MaterialTemplate.ush:665–670, 761).
namespace rocks::material {

namespace {

constexpr MaterialContextValue
ContextValue(StringView name, StringView unrealName, MaterialPinType type,
             MaterialContextValueForm form, uint8 argumentCount, MaterialContextFeature feature,
             MaterialDomainMask domains, MaterialStageMask stages,
             MaterialCapabilityTier minimumTier, MaterialDerivative derivative,
             MaterialContextDefault defaultValue, bool hasPrevious) noexcept {
    MaterialContextValue value;
    value.name = name;
    value.unrealName = unrealName;
    value.type = type;
    value.form = form;
    value.argumentCount = argumentCount;
    value.feature = feature;
    value.domains = domains;
    value.stages = stages;
    value.minimumTier = minimumTier;
    value.derivative = derivative;
    value.defaultValue = defaultValue;
    value.hasPrevious = hasPrevious;
    return value;
}

constexpr MaterialContextValue ContextValues[] = {
    // UE AtmosphericLightVector.
    ContextValue("AtmosphericLightVector", "AtmosphericLightVector", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Sky,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE AtmosphericLightColor.
    ContextValue("AtmosphericLightColor", "AtmosphericLightColor", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Sky,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE CameraVector.
    ContextValue("CameraVector", "CameraVector", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE DecalColor.
    ContextValue("DecalColor", "DecalColor", MaterialPinType::Float4,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Decal,
                 ToMask(MaterialDomain::DeferredDecal), MaterialPixelStage,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE DecalLifetimeOpacity.
    ContextValue("DecalLifetimeOpacity", "DecalLifetimeOpacity", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Decal,
                 ToMask(MaterialDomain::DeferredDecal), MaterialPixelStage,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE DeltaTime.
    ContextValue("DeltaTime", "DeltaTime", MaterialPinType::Float1, MaterialContextValueForm::Field,
                 0, MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE EyeAdaptation.
    ContextValue("EyeAdaptation", "EyeAdaptation", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::PostProcess,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE EyeAdaptationInverse.
    ContextValue("EyeAdaptationInverse", "EyeAdaptationInverse", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::PostProcess,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE FontSignedDistanceData.
    ContextValue("FontSignedDistanceData", "FontSignedDistanceData", MaterialPinType::Float4,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::UserInterface,
                 ToMask(MaterialDomain::UserInterface), MaterialPixelStage,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE IsFirstPerson.
    ContextValue("IsFirstPerson", "IsFirstPerson", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::FirstPerson,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {},
                 false),
    // UE IsOrthographic.
    ContextValue("IsOrthographic", "IsOrthographic", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE LightVector.
    ContextValue("LightVector", "LightVector", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Light,
                 ToMask(MaterialDomain::LightFunction) | ToMask(MaterialDomain::DeferredDecal),
                 MaterialPixelStage, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, false),
    // UE LightmapUVs.
    ContextValue("LightmapUvs", "LightmapUVs", MaterialPinType::Float2,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Lightmap,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Sm5,
                 MaterialDerivative::Valid, {}, false),
    // UE MeshPaintTextureCoordinateIndex.
    ContextValue("MeshPaintTextureCoordinateIndex", "MeshPaintTextureCoordinateIndex",
                 MaterialPinType::Float1, MaterialContextValueForm::Function, 0,
                 MaterialContextFeature::MeshPaint, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE MeshPaintTextureDescriptor.
    ContextValue("MeshPaintTextureDescriptor", "MeshPaintTextureDescriptor",
                 MaterialPinType::TextureMeshPaint, MaterialContextValueForm::Function, 0,
                 MaterialContextFeature::MeshPaint, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE ObjectBounds.
    ContextValue("ObjectBounds", "ObjectBounds", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {},
                 false),
    // UE ObjectOrientation. UE 기본 번역기는 영역을 검사하지
    // 않는다(HLSLMaterialTranslator.cpp:11716 —
    // ini Domains는 IR 경로만 본다. 6.8절 의미 기준).
    ContextValue("ObjectOrientation", "ObjectOrientation", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE ObjectRadius.
    ContextValue("ObjectRadius", "ObjectRadius", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {},
                 false),
    // UE ParticleColor.
    ContextValue("ParticleColor", "ParticleColor", MaterialPinType::Float4,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {1.0f, 1.0f, 1.0f, 1.0f}, false),
    // UE ParticleDirection.
    ContextValue("ParticleDirection", "ParticleDirection", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE ParticleMacroUV.
    ContextValue("ParticleMacroUv", "ParticleMacroUV", MaterialPinType::Float2,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE ParticleMotionBlurFade.
    ContextValue("ParticleMotionBlurFade", "ParticleMotionBlurFade", MaterialPinType::Float1,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {1.0f}, false),
    // UE ParticlePositionWSAbsolute.
    ContextValue("ParticlePositionAbsolute", "ParticlePositionWSAbsolute", MaterialPinType::Double3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, true),
    // UE ParticlePositionWSTranslated.
    ContextValue("ParticlePositionTranslated", "ParticlePositionWSTranslated",
                 MaterialPinType::Float3, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::Particle, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, true),
    // UE ParticleRadius.
    ContextValue("ParticleRadius", "ParticleRadius", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE ParticleRelativeTime.
    ContextValue("ParticleRelativeTime", "ParticleRelativeTime", MaterialPinType::Float1,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE ParticleRandom.
    ContextValue("ParticleRandom", "ParticleRandom", MaterialPinType::Float1,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE ParticleSpeed.
    ContextValue("ParticleSpeed", "ParticleSpeed", MaterialPinType::Float1,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE ParticleSize.
    ContextValue("ParticleSize", "ParticleSize", MaterialPinType::Float2,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE ParticleSpriteRotation.
    ContextValue("ParticleSpriteRotation", "ParticleSpriteRotation", MaterialPinType::Float2,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE ParticleSubUVCoords0.
    ContextValue("ParticleSubUvCoords0", "ParticleSubUVCoords0", MaterialPinType::Float2,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE ParticleSubUVCoords1.
    ContextValue("ParticleSubUvCoords1", "ParticleSubUVCoords1", MaterialPinType::Float2,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE ParticleSubUVLerp.
    ContextValue("ParticleSubUvLerp", "ParticleSubUVLerp", MaterialPinType::Float1,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE PixelDepth.
    ContextValue("PixelDepth", "PixelDepth", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Valid, {}, false),
    // UE PixelPosition.
    ContextValue("PixelPosition", "PixelPosition", MaterialPinType::Float2,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Valid, {}, false),
    // UE PrecomputedAOMask.
    ContextValue("PrecomputedAoMask", "PrecomputedAOMask", MaterialPinType::Float1,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Lightmap,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Sm5,
                 MaterialDerivative::Zero, {}, false),
    // UE PreSkinnedPosition.
    ContextValue("PreSkinnedPosition", "PreSkinnedPosition", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Skin,
                 AllMaterialDomains, MaterialVertexStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE PreSkinnedNormal.
    ContextValue("PreSkinnedNormal", "PreSkinnedNormal", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Skin,
                 AllMaterialDomains, MaterialVertexStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE ReflectionVector.
    ContextValue("ReflectionVector", "ReflectionVector", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE ScalarBlueNoise.
    ContextValue("ScalarBlueNoise", "ScalarBlueNoise", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::View,
                 ToMask(MaterialDomain::Surface), MaterialPixelStage,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE SphericalParticleOpacity.
    ContextValue("SphericalParticleOpacity", "SphericalParticleOpacity", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Particle,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE DecalMipmapLevel.
    ContextValue("DecalMipmapLevel", "DecalMipmapLevel", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Decal,
                 ToMask(MaterialDomain::DeferredDecal), MaterialPixelStage,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE DepthOfFieldFunction.
    ContextValue("DepthOfFieldFunction", "DepthOfFieldFunction", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 2, MaterialContextFeature::PostProcess,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE TwoSidedSign.
    ContextValue("TwoSidedSign", "TwoSidedSign", MaterialPinType::Float1,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Vertex,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE VertexColor.
    ContextValue("VertexColor", "VertexColor", MaterialPinType::Float4,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Vertex,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Valid, {}, false),
    // UE VertexNormal.
    ContextValue("VertexNormal", "VertexNormal", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Vertex,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Valid, {0.0f, 0.0f, 1.0f}, false),
    // UE VertexTangent.
    ContextValue("VertexTangent", "VertexTangent", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Vertex,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {1.0f, 0.0f, 0.0f}, false),
    // UE ViewportUV.
    ContextValue("ViewportUv", "ViewportUV", MaterialPinType::Float2,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Valid, {}, false),
    // UE GetPerInstanceCustomData.
    ContextValue("PerInstanceCustomData", "GetPerInstanceCustomData", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 2, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE GetPerInstanceCustomData3Vector.
    ContextValue("PerInstanceCustomData3Vector", "GetPerInstanceCustomData3Vector",
                 MaterialPinType::Float3, MaterialContextValueForm::Function, 2,
                 MaterialContextFeature::Primitive, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE GetPerInstanceFadeAmount.
    ContextValue("PerInstanceFadeAmount", "GetPerInstanceFadeAmount", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE GetPerInstanceRandom.
    ContextValue("PerInstanceRandom", "GetPerInstanceRandom", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE GetCloudSampleAltitude.
    ContextValue("CloudSampleAltitude", "GetCloudSampleAltitude", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Volume,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE GetCloudSampleAltitudeInLayer.
    ContextValue("CloudSampleAltitudeInLayer", "GetCloudSampleAltitudeInLayer",
                 MaterialPinType::Float1, MaterialContextValueForm::Function, 0,
                 MaterialContextFeature::Volume, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE GetCloudSampleNormAltitudeInLayer.
    ContextValue("CloudSampleNormAltitudeInLayer", "GetCloudSampleNormAltitudeInLayer",
                 MaterialPinType::Float1, MaterialContextValueForm::Function, 0,
                 MaterialContextFeature::Volume, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE GetCloudSampleShadowSampleDistance.
    ContextValue("CloudSampleShadowSampleDistance", "GetCloudSampleShadowSampleDistance",
                 MaterialPinType::Float1, MaterialContextValueForm::Function, 0,
                 MaterialContextFeature::Volume, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE GetVolumeSampleConservativeDensity.
    ContextValue("VolumeSampleConservativeDensity", "GetVolumeSampleConservativeDensity",
                 MaterialPinType::Float4, MaterialContextValueForm::Function, 0,
                 MaterialContextFeature::Volume, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE GetCloudEmptySpaceSkippingSphereCenterWorldPosition.
    ContextValue("CloudEmptySpaceSkippingSphereCenterWorldPosition",
                 "GetCloudEmptySpaceSkippingSphereCenterWorldPosition", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Volume,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE GetCloudEmptySpaceSkippingSphereRadius.
    ContextValue("CloudEmptySpaceSkippingSphereRadius", "GetCloudEmptySpaceSkippingSphereRadius",
                 MaterialPinType::Float1, MaterialContextValueForm::Function, 0,
                 MaterialContextFeature::Volume, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE SkyAtmosphereAerialPerspective.
    ContextValue("SkyAtmosphereAerialPerspective", "SkyAtmosphereAerialPerspective",
                 MaterialPinType::Float4, MaterialContextValueForm::Function, 1,
                 MaterialContextFeature::Sky, AllMaterialDomains, MaterialPixelStage,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE SkyAtmosphereDistantLightScatteredLuminance.
    ContextValue("SkyAtmosphereDistantLightScatteredLuminance",
                 "SkyAtmosphereDistantLightScatteredLuminance", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Sky,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE SkyAtmosphereLightDirection.
    ContextValue("SkyAtmosphereLightDirection", "SkyAtmosphereLightDirection",
                 MaterialPinType::Float3, MaterialContextValueForm::Function, 1,
                 MaterialContextFeature::Sky, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE SkyAtmosphereLightDiskLuminance.
    ContextValue("SkyAtmosphereLightDiskLuminance", "SkyAtmosphereLightDiskLuminance",
                 MaterialPinType::Float3, MaterialContextValueForm::Function, 2,
                 MaterialContextFeature::Sky, AllMaterialDomains, MaterialPixelStage,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE SkyAtmosphereLightIlluminance.
    ContextValue("SkyAtmosphereLightIlluminance", "SkyAtmosphereLightIlluminance",
                 MaterialPinType::Float3, MaterialContextValueForm::Function, 2,
                 MaterialContextFeature::Sky, AllMaterialDomains, MaterialPixelStage,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE SkyAtmosphereLightIlluminanceOnGround.
    ContextValue("SkyAtmosphereLightIlluminanceOnGround", "SkyAtmosphereLightIlluminanceOnGround",
                 MaterialPinType::Float3, MaterialContextValueForm::Function, 1,
                 MaterialContextFeature::Sky, AllMaterialDomains, MaterialPixelStage,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE SkyAtmosphereViewLuminance.
    ContextValue("SkyAtmosphereViewLuminance", "SkyAtmosphereViewLuminance",
                 MaterialPinType::Float3, MaterialContextValueForm::Function, 1,
                 MaterialContextFeature::Sky, AllMaterialDomains, MaterialPixelStage,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE SkyLightEnvMapSample.
    ContextValue("SkyLightEnvironmentSample", "SkyLightEnvMapSample", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 2, MaterialContextFeature::Sky,
                 ToMask(MaterialDomain::Surface), AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE LightmassReplace.
    ContextValue("LightBakePass", "LightmassReplace", MaterialPinType::Bool,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE MeshPaintTextureReplace.
    ContextValue("MeshPaintTextureValid", "MeshPaintTextureReplace", MaterialPinType::Bool,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::MeshPaint,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE NaniteReplace.
    ContextValue("VirtualizedGeometryPass", "NaniteReplace", MaterialPinType::Bool,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE ReflectionCapturePassSwitch.
    ContextValue("ReflectionCapturePass", "ReflectionCapturePassSwitch", MaterialPinType::Bool,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE ShadowReplace.
    ContextValue("ShadowPass", "ShadowReplace", MaterialPinType::Bool,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE InstanceLocalBoundsHalf.
    ContextValue("InstanceLocalBoundsHalf", "InstanceLocalBoundsHalf", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, false),
    // UE InstanceLocalBoundsFull.
    ContextValue("InstanceLocalBoundsFull", "InstanceLocalBoundsFull", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, false),
    // UE InstanceLocalBoundsMin.
    ContextValue("InstanceLocalBoundsMin", "InstanceLocalBoundsMin", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, false),
    // UE InstanceLocalBoundsMax.
    ContextValue("InstanceLocalBoundsMax", "InstanceLocalBoundsMax", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, false),
    // UE ObjectLocalBoundsHalf.
    ContextValue("ObjectLocalBoundsHalf", "ObjectLocalBoundsHalf", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, false),
    // UE ObjectLocalBoundsFull.
    ContextValue("ObjectLocalBoundsFull", "ObjectLocalBoundsFull", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, false),
    // UE ObjectLocalBoundsMin.
    ContextValue("ObjectLocalBoundsMin", "ObjectLocalBoundsMin", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, false),
    // UE ObjectLocalBoundsMax.
    ContextValue("ObjectLocalBoundsMax", "ObjectLocalBoundsMax", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, false),
    // UE PreSkinnedLocalBoundsHalf.
    ContextValue("PreSkinnedLocalBoundsHalf", "PreSkinnedLocalBoundsHalf", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, false),
    // UE PreSkinnedLocalBoundsFull.
    ContextValue("PreSkinnedLocalBoundsFull", "PreSkinnedLocalBoundsFull", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, false),
    // UE PreSkinnedLocalBoundsMin.
    ContextValue("PreSkinnedLocalBoundsMin", "PreSkinnedLocalBoundsMin", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, false),
    // UE PreSkinnedLocalBoundsMax.
    ContextValue("PreSkinnedLocalBoundsMax", "PreSkinnedLocalBoundsMax", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, false),
    // UE CameraViewToTranslatedWorld.
    ContextValue("CameraViewToTranslatedWorld", "CameraViewToTranslatedWorld",
                 MaterialPinType::Float4x4, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, true),
    // UE GetInstanceToWorld.
    ContextValue("InstanceToWorld", "GetInstanceToWorld", MaterialPinType::Float4x4,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, true),
    // UE GetInstanceToWorldDF.
    ContextValue("InstanceToWorldDouble", "GetInstanceToWorldDF", MaterialPinType::Float4x4,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, true),
    // UE GetLocalToWorldDF.
    ContextValue("LocalToWorldDouble", "GetLocalToWorldDF", MaterialPinType::Float4x4,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, true),
    // UE GetParticleToWorld.
    ContextValue("GetParticleToWorld", "GetParticleToWorld", MaterialPinType::Float4x4,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE GetPreViewTranslation.
    ContextValue("PreViewTranslation", "GetPreViewTranslation", MaterialPinType::Double3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, true),
    // UE GetWorldToInstance.
    ContextValue("WorldToInstance", "GetWorldToInstance", MaterialPinType::Float4x4,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE GetWorldToInstanceDF.
    ContextValue("WorldToInstanceDouble", "GetWorldToInstanceDF", MaterialPinType::Float4x4,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE GetWorldToLocal.
    ContextValue("WorldToLocal", "GetWorldToLocal", MaterialPinType::Float4x4,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {},
                 true),
    // UE GetWorldToLocalDF.
    ContextValue("WorldToLocalDouble", "GetWorldToLocalDF", MaterialPinType::Float4x4,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {},
                 true),
    // UE GetWorldToParticle.
    ContextValue("GetWorldToParticle", "GetWorldToParticle", MaterialPinType::Float4x4,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE ParticleToWorld.
    ContextValue("ParticleToWorld", "ParticleToWorld", MaterialPinType::Float4x4,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE TangentToWorld.
    ContextValue("TangentToWorld", "TangentToWorld", MaterialPinType::Float3x3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Vertex,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE TranslatedWorldToCameraView.
    ContextValue("TranslatedWorldToCameraView", "TranslatedWorldToCameraView",
                 MaterialPinType::Float4x4, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, true),
    // UE TranslatedWorldToView.
    ContextValue("TranslatedWorldToView", "TranslatedWorldToView", MaterialPinType::Float4x4,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, true),
    // UE ViewToTranslatedWorld.
    ContextValue("ViewToTranslatedWorld", "ViewToTranslatedWorld", MaterialPinType::Float4x4,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, true),
    // UE WorldToParticle.
    ContextValue("WorldToParticle", "WorldToParticle", MaterialPinType::Float4x4,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE 템플릿 필드 View.GameTime(Time 노드).
    ContextValue("Time", "", MaterialPinType::Float1, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, true),
    // UE 템플릿 필드 View.RealTime(Time 노드 IgnorePause).
    ContextValue("RealTime", "", MaterialPinType::Float1, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, true),
    // UE 템플릿 필드 ResolvedView.WorldCameraOrigin(CameraPositionWS).
    ContextValue("CameraPosition", "", MaterialPinType::Double3, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, true),
    // UE 템플릿 필드 View.ViewSizeAndInvSize.xy.
    ContextValue("ViewSize", "", MaterialPinType::Float2, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE 템플릿 필드 View.BufferSizeAndInvSize.zw.
    ContextValue("SceneTexelSize", "", MaterialPinType::Float2, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE 템플릿 필드 Parameters.TexCoords[i].
    ContextValue("TexCoord", "", MaterialPinType::Float2, MaterialContextValueForm::Function, 1,
                 MaterialContextFeature::Vertex, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Valid, {}, false),
    // UE 템플릿 필드 GetWorldPosition(Parameters).
    ContextValue("WorldPosition", "", MaterialPinType::Double3, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::Vertex, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Valid, {}, true),
    // UE 템플릿 필드 GetTranslatedWorldPosition(Parameters).
    ContextValue("TranslatedWorldPosition", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Vertex,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Valid, {}, true),
    // UE 템플릿 필드 GetWorldPosition_NoMaterialOffsets(Parameters).
    ContextValue("WorldPositionNoShaderOffsets", "", MaterialPinType::Double3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Vertex,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Valid, {}, false),
    // UE 템플릿 필드 GetLocalPosition(Parameters).
    ContextValue("LocalPosition", "", MaterialPinType::Float3, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::Vertex, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Valid, {}, true),
    // UE 템플릿 필드 Parameters.WorldNormal(픽셀 단계 — 노멀 출력 뒤).
    ContextValue("PixelNormal", "", MaterialPinType::Float3, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::Vertex, AllMaterialDomains, MaterialPixelStage,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Valid, {}, false),
    // UE 템플릿 필드 Parameters.ScreenPosition.
    ContextValue("ScreenPosition", "", MaterialPinType::Float4, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Valid, {}, false),
    // UE 템플릿 필드 GetObjectWorldPosition(Parameters).
    ContextValue("ObjectPosition", "", MaterialPinType::Double3, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::Primitive, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE 템플릿 필드 GetActorWorldPosition(Parameters).
    ContextValue("ActorPosition", "", MaterialPinType::Double3, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::DeferredDecal) |
                     ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {},
                 true),
    // UE 템플릿 필드 GetDistanceCullFade().
    ContextValue("DistanceCullFade", "", MaterialPinType::Float1, MaterialContextValueForm::Field,
                 0, MaterialContextFeature::Primitive, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE 템플릿 GetTranslatedWorldPosition_NoMaterialOffsets(Parameters)(픽셀 단계).
    ContextValue("TranslatedWorldPositionNoShaderOffsets", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Vertex,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Valid, {}, false),
    // UE 템플릿 GetPositionInstanceSpace_NoMaterialOffsets(Parameters)(픽셀 단계).
    ContextValue("LocalPositionNoShaderOffsets", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Vertex,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Valid, {}, false),
    // UE 템플릿 GetPositionPrimitiveSpace(Parameters)(이전 프레임 Prev).
    ContextValue("PrimitivePosition", "", MaterialPinType::Float3, MaterialContextValueForm::Field,
                 0, MaterialContextFeature::Vertex, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Valid, {}, true),
    // UE 템플릿 GetPositionPrimitiveSpace_NoMaterialOffsets(Parameters)(픽셀 단계).
    ContextValue("PrimitivePositionNoShaderOffsets", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Vertex,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Valid, {}, false),
    // UE 템플릿 GetObjectTranslatedWorldPosition(Parameters).
    ContextValue("ObjectTranslatedPosition", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE 템플릿 GetActorTranslatedWorldPosition(Parameters)(이전 프레임 GetPrevious…).
    ContextValue("ActorTranslatedPosition", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::DeferredDecal) |
                     ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {},
                 true),
    // UE 템플릿 Parameters.UnMirrored(UnMirrorU·V — 픽셀 단계).
    ContextValue("UnMirrored", "", MaterialPinType::Float1, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::Vertex, AllMaterialDomains, MaterialPixelStage,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE ResolvedView.XRPassthroughCameraUVs[0].
    ContextValue("XrPassthroughCameraUv0", "", MaterialPinType::Float4,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Xr, AllMaterialDomains,
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {},
                 false),
    // UE ResolvedView.XRPassthroughCameraUVs[1].
    ContextValue("XrPassthroughCameraUv1", "", MaterialPinType::Float4,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Xr, AllMaterialDomains,
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {},
                 false),
    // UE View.<PREV>FieldOfViewWideAngles(ViewProperty FieldOfView).
    ContextValue("ViewFieldOfView", "", MaterialPinType::Float2, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, true),
    // UE Get<PREV>TanHalfFieldOfView().
    ContextValue("ViewTanHalfFieldOfView", "", MaterialPinType::Float2,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, true),
    // UE Get<PREV>CotanHalfFieldOfView().
    ContextValue("ViewCotanHalfFieldOfView", "", MaterialPinType::Float2,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, true),
    // UE Get<PREV>WorldViewOrigin(Parameters).
    ContextValue("WorldViewOrigin", "", MaterialPinType::Double3, MaterialContextValueForm::Field,
                 0, MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, true),
    // UE View.TemporalAAParams.y.
    ContextValue("TemporalSampleCount", "", MaterialPinType::Float1,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE View.TemporalAAParams.x.
    ContextValue("TemporalSampleIndex", "", MaterialPinType::Float1,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE View.TemporalAAParams.zw.
    ContextValue("TemporalSampleOffset", "", MaterialPinType::Float2,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE GetRuntimeVirtualTextureMipLevel().x.
    ContextValue("RuntimeVirtualTextureOutputLevel", "", MaterialPinType::Float1,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::RuntimeVirtualTexture,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE GetRuntimeVirtualTextureMipLevel().zw.
    ContextValue("RuntimeVirtualTextureOutputDerivative", "", MaterialPinType::Float2,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::RuntimeVirtualTexture,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE GetRuntimeVirtualTextureMipLevel().y.
    ContextValue("RuntimeVirtualTextureMaxLevel", "", MaterialPinType::Float1,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::RuntimeVirtualTexture,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE View.PreExposure.
    ContextValue("PreExposure", "", MaterialPinType::Float1, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE View.OneOverPreExposure.
    ContextValue("OneOverPreExposure", "", MaterialPinType::Float1, MaterialContextValueForm::Field,
                 0, MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE View.ResolutionFractionAndInv.x.
    ContextValue("ResolutionFraction", "", MaterialPinType::Float1, MaterialContextValueForm::Field,
                 0, MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE View.ResolutionFractionAndInv.y.
    ContextValue("ResolutionFractionInverse", "", MaterialPinType::Float1,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE View.PostVolumeUserFlags.
    ContextValue("PostVolumeUserFlags", "", MaterialPinType::Float1,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE View.<PREV>FirstPersonFieldOfViewWideAngles.
    ContextValue("FirstPersonFieldOfView", "", MaterialPinType::Float2,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, true),
    // UE View.<PREV>FirstPersonTanAndInvTanHalfFOV.xy.
    ContextValue("FirstPersonTanHalfFieldOfView", "", MaterialPinType::Float2,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, true),
    // UE View.<PREV>FirstPersonTanAndInvTanHalfFOV.zw.
    ContextValue("FirstPersonCotanHalfFieldOfView", "", MaterialPinType::Float2,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, true),
    // UE View.<PREV>FirstPersonScale.
    ContextValue("FirstPersonScale", "", MaterialPinType::Float1, MaterialContextValueForm::Field,
                 0, MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, true),
    // UE View.NearPlane.
    ContextValue("NearPlane", "", MaterialPinType::Float1, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE mul(A, Parameters.TangentToWorld).
    ContextValue("TransformTangentVectorToWorld", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Vertex,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE mul((float3x3)Parameters.TangentToWorld, A)(World·TranslatedWorld → Tangent).
    ContextValue("TransformWorldVectorToTangent", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Vertex,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE TransformLocalPositionTo<PREV>World.
    ContextValue("TransformLocalPositionToWorld", "", MaterialPinType::Double3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE TransformLocalVectorTo<PREV>World.
    ContextValue("TransformLocalVectorToWorld", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE Get<PREV>LocalToWorldDF(Parameters)의 translated world 곱.
    ContextValue("TransformLocalPositionToTranslatedWorld", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE ResolvedView.<PREV>TranslatedWorldToCameraView(위치).
    ContextValue("TransformTranslatedWorldPositionToCamera", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE ResolvedView.<PREV>TranslatedWorldToCameraView(벡터).
    ContextValue("TransformTranslatedWorldVectorToCamera", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE ResolvedView.<PREV>TranslatedWorldToView(위치).
    ContextValue("TransformTranslatedWorldPositionToView", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE ResolvedView.<PREV>TranslatedWorldToView(벡터).
    ContextValue("TransformTranslatedWorldVectorToView", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE Get<PREV>WorldToLocalDF(Parameters)(위치 — Surface·Volume).
    ContextValue("TransformTranslatedWorldPositionToLocal", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, true),
    // UE Get<PREV>WorldToLocalDF(Parameters)(벡터 — Surface·Volume).
    ContextValue("TransformTranslatedWorldVectorToLocal", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, true),
    // UE Parameters.Particle.WorldToParticle(위치 — 이전 프레임은 PrevPreViewTranslation).
    ContextValue("TransformTranslatedWorldPositionToParticle", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE Parameters.Particle.WorldToParticle(벡터).
    ContextValue("TransformTranslatedWorldVectorToParticle", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE GetWorldToInstanceDF(Parameters)(위치 — 이전 프레임은 PrevPreViewTranslation).
    ContextValue("TransformTranslatedWorldPositionToInstance", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE GetWorldToInstanceDF(Parameters)(벡터).
    ContextValue("TransformTranslatedWorldVectorToInstance", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE WSMultiplyDemote(A, Get<PREV>WorldToLocal(Parameters))(Surface·Volume).
    ContextValue("TransformWorldPositionToLocal", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, true),
    // UE WSMultiplyVector(A, Get<PREV>WorldToLocal(Parameters))(Surface·Volume).
    ContextValue("TransformWorldVectorToLocal", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware,
                 {}, true),
    // UE WSMultiplyDemote(A, GetWorldToParticle(Parameters)).
    ContextValue("TransformWorldPositionToParticle", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE WSMultiplyVector(A, GetWorldToParticle(Parameters)).
    ContextValue("TransformWorldVectorToParticle", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE WSMultiplyDemote(A, GetWorldToInstance(Parameters)).
    ContextValue("TransformWorldPositionToInstance", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE WSMultiplyVector(A, GetWorldToInstance(Parameters)).
    ContextValue("TransformWorldVectorToInstance", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE ResolvedView.<PREV>CameraViewToTranslatedWorld(위치).
    ContextValue("TransformCameraPositionToTranslatedWorld", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE ResolvedView.<PREV>CameraViewToTranslatedWorld(벡터).
    ContextValue("TransformCameraVectorToTranslatedWorld", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE ResolvedView.<PREV>ViewToTranslatedWorld(위치).
    ContextValue("TransformViewPositionToTranslatedWorld", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE ResolvedView.<PREV>ViewToTranslatedWorld(벡터).
    ContextValue("TransformViewVectorToTranslatedWorld", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE WSMultiply(A, GetParticleToWorld(Parameters)).
    ContextValue("TransformParticlePositionToWorld", "", MaterialPinType::Double3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE WSMultiplyVector(A, GetParticleToWorld(Parameters)).
    ContextValue("TransformParticleVectorToWorld", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE Parameters.Particle.ParticleToWorld(위치 — 이전 프레임은 PrevPreViewTranslation).
    ContextValue("TransformParticlePositionToTranslatedWorld", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE Parameters.Particle.ParticleToWorld(벡터).
    ContextValue("TransformParticleVectorToTranslatedWorld", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Particle,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE WSMultiply(A, Get<PREV>InstanceToWorld(Parameters)).
    ContextValue("TransformInstancePositionToWorld", "", MaterialPinType::Double3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE WSMultiplyVector(A, Get<PREV>InstanceToWorld(Parameters)).
    ContextValue("TransformInstanceVectorToWorld", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE Get<PREV>InstanceToWorldDF(Parameters)(위치).
    ContextValue("TransformInstancePositionToTranslatedWorld", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE Get<PREV>InstanceToWorldDF(Parameters)(벡터).
    ContextValue("TransformInstanceVectorToTranslatedWorld", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Primitive,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // UE GetPeriodicWorldOrigin(Scale)(MaterialTemplate.ush:1031).
    ContextValue("PeriodicWorldOrigin", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE GetPeriodicWorldOrigin_Pow2(Scale)(MaterialTemplate.ush:1041).
    ContextValue("PeriodicWorldOriginPow2", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE TransformTo<PREVIOUS>FirstPerson(A, alpha).
    ContextValue("TransformToFirstPerson", "", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 2, MaterialContextFeature::FirstPerson,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, true),
    // 조건 값: Deferred 경로 pass(RENDER_PATH_DEFERRED — 6.3절 3항).
    ContextValue("RenderPathDeferred", "", MaterialPinType::Bool,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // 조건 값: Forward+ 경로 pass(RENDER_PATH_FORWARD_PLUS).
    ContextValue("RenderPathForwardPlus", "", MaterialPinType::Bool,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // 조건 값: 형식 기능 등급 SM5 이상(Capability Tier Switch).
    ContextValue("CapabilityTierSm5", "", MaterialPinType::Bool, MaterialContextValueForm::Function,
                 0, MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // 조건 값: 형식 기능 등급 SM6.
    ContextValue("CapabilityTierSm6", "", MaterialPinType::Bool, MaterialContextValueForm::Function,
                 0, MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE GetGIReplaceState()(REFLECTIVE_SHADOW_MAP — Common.ush:2152).
    ContextValue("GlobalIlluminationPass", "", MaterialPinType::Bool,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::View,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE GetRayTracingQualitySwitch()(Common.ush:2171).
    ContextValue("RayTracingPass", "", MaterialPinType::Bool, MaterialContextValueForm::Function, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE GetPathTracingQualitySwitch()(Common.ush:2181).
    ContextValue("PathTracingPass", "", MaterialPinType::Bool, MaterialContextValueForm::Function,
                 0, MaterialContextFeature::PathTracing, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE GetPathTracingIsShadow()(PathTracingShaderUtils.ush).
    ContextValue("PathTracingShadowRay", "", MaterialPinType::Bool,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::PathTracing,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE GetPathTracingIsIndirectDiffuse().
    ContextValue("PathTracingIndirectDiffuseRay", "", MaterialPinType::Bool,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::PathTracing,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE GetPathTracingIsIndirectSpecular().
    ContextValue("PathTracingIndirectSpecularRay", "", MaterialPinType::Bool,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::PathTracing,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
    // UE GetPathTracingIsIndirectVolume().
    ContextValue("PathTracingIndirectVolumeRay", "", MaterialPinType::Bool,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::PathTracing,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::Zero, {}, false),
};

} // namespace

Span<const MaterialContextValue> MaterialContextValues() noexcept {
    return ContextValues;
}

const MaterialContextValue* FindMaterialContextValue(StringView name) noexcept {
    for (const MaterialContextValue& value : ContextValues) {
        if (value.name == name) {
            return &value;
        }
    }

    return nullptr;
}

} // namespace rocks::material
