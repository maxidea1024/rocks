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
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
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
    // UE ObjectOrientation.
    ContextValue("ObjectOrientation", "ObjectOrientation", MaterialPinType::Float3,
                 MaterialContextValueForm::Function, 0, MaterialContextFeature::Primitive,
                 ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume),
                 AllMaterialStages, MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {},
                 false),
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
                 MaterialDerivative::Zero, {}, false),
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
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE SphericalParticleOpacity.
    ContextValue("SphericalParticleOpacity", "SphericalParticleOpacity", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Particle,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
    // UE DecalMipmapLevel.
    ContextValue("DecalMipmapLevel", "DecalMipmapLevel", MaterialPinType::Float1,
                 MaterialContextValueForm::Function, 1, MaterialContextFeature::Decal,
                 AllMaterialDomains, MaterialPixelStage, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
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
                 MaterialDerivative::Valid, {}, false),
    // UE VertexTangent.
    ContextValue("VertexTangent", "VertexTangent", MaterialPinType::Float3,
                 MaterialContextValueForm::Field, 0, MaterialContextFeature::Vertex,
                 AllMaterialDomains, AllMaterialStages, MaterialCapabilityTier::Mobile,
                 MaterialDerivative::NotAware, {}, false),
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
                 MaterialContextFeature::Sky, AllMaterialDomains, AllMaterialStages,
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
                 MaterialContextFeature::Sky, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE SkyAtmosphereLightIlluminance.
    ContextValue("SkyAtmosphereLightIlluminance", "SkyAtmosphereLightIlluminance",
                 MaterialPinType::Float3, MaterialContextValueForm::Function, 2,
                 MaterialContextFeature::Sky, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE SkyAtmosphereLightIlluminanceOnGround.
    ContextValue("SkyAtmosphereLightIlluminanceOnGround", "SkyAtmosphereLightIlluminanceOnGround",
                 MaterialPinType::Float3, MaterialContextValueForm::Function, 1,
                 MaterialContextFeature::Sky, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::NotAware, {}, false),
    // UE SkyAtmosphereViewLuminance.
    ContextValue("SkyAtmosphereViewLuminance", "SkyAtmosphereViewLuminance",
                 MaterialPinType::Float3, MaterialContextValueForm::Function, 1,
                 MaterialContextFeature::Sky, AllMaterialDomains, AllMaterialStages,
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
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE 템플릿 필드 View.RealTime(Time 노드 IgnorePause).
    ContextValue("RealTime", "", MaterialPinType::Float1, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE 템플릿 필드 ResolvedView.WorldCameraOrigin(CameraPositionWS).
    ContextValue("CameraPosition", "", MaterialPinType::Double3, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE 템플릿 필드 View.ViewSizeAndInvSize.xy.
    ContextValue("ViewSize", "", MaterialPinType::Float2, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE 템플릿 필드 View.BufferSizeAndInvSize.zw.
    ContextValue("SceneTexelSize", "", MaterialPinType::Float2, MaterialContextValueForm::Field, 0,
                 MaterialContextFeature::View, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE 템플릿 필드 ViewProperty 노드 — 속성 번호 하나.
    ContextValue("ViewProperty", "", MaterialPinType::Float4, MaterialContextValueForm::Function, 1,
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
                 MaterialContextFeature::Primitive, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
    // UE 템플릿 필드 GetDistanceCullFade().
    ContextValue("DistanceCullFade", "", MaterialPinType::Float1, MaterialContextValueForm::Field,
                 0, MaterialContextFeature::Primitive, AllMaterialDomains, AllMaterialStages,
                 MaterialCapabilityTier::Mobile, MaterialDerivative::Zero, {}, false),
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
