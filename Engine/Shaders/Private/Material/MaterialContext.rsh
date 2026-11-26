// Generated from the material context value table by WriteMaterialContextSource
// (Docs/Design/MaterialSystem 6.9, 19.3). Do not edit: change the table and regenerate.
//
// A pass defines MATERIAL_SUPPLY_<FEATURE> 1 for every feature its shader type and vertex
// input type supply before including MaterialCommon.rsh. Accessors of features that are not
// supplied return the Unreal Engine default value, so generated material source compiles in
// every pass.
#pragma once

#include "/Engine/Private/Material/MaterialDouble.rsh"

// Supply flags (0 unless the pass defines them).
#ifndef MATERIAL_SUPPLY_VIEW
#define MATERIAL_SUPPLY_VIEW 0
#endif
#ifndef MATERIAL_SUPPLY_VERTEX
#define MATERIAL_SUPPLY_VERTEX 0
#endif
#ifndef MATERIAL_SUPPLY_PRIMITIVE
#define MATERIAL_SUPPLY_PRIMITIVE 0
#endif
#ifndef MATERIAL_SUPPLY_SKIN
#define MATERIAL_SUPPLY_SKIN 0
#endif
#ifndef MATERIAL_SUPPLY_LIGHTMAP
#define MATERIAL_SUPPLY_LIGHTMAP 0
#endif
#ifndef MATERIAL_SUPPLY_LIGHT
#define MATERIAL_SUPPLY_LIGHT 0
#endif
#ifndef MATERIAL_SUPPLY_SKY
#define MATERIAL_SUPPLY_SKY 0
#endif
#ifndef MATERIAL_SUPPLY_POST_PROCESS
#define MATERIAL_SUPPLY_POST_PROCESS 0
#endif
#ifndef MATERIAL_SUPPLY_SCENE
#define MATERIAL_SUPPLY_SCENE 0
#endif
#ifndef MATERIAL_SUPPLY_DECAL
#define MATERIAL_SUPPLY_DECAL 0
#endif
#ifndef MATERIAL_SUPPLY_PARTICLE
#define MATERIAL_SUPPLY_PARTICLE 0
#endif
#ifndef MATERIAL_SUPPLY_DISTANCE_FIELD
#define MATERIAL_SUPPLY_DISTANCE_FIELD 0
#endif
#ifndef MATERIAL_SUPPLY_HAIR
#define MATERIAL_SUPPLY_HAIR 0
#endif
#ifndef MATERIAL_SUPPLY_WATER
#define MATERIAL_SUPPLY_WATER 0
#endif
#ifndef MATERIAL_SUPPLY_VOLUME
#define MATERIAL_SUPPLY_VOLUME 0
#endif
#ifndef MATERIAL_SUPPLY_RUNTIME_VIRTUAL_TEXTURE
#define MATERIAL_SUPPLY_RUNTIME_VIRTUAL_TEXTURE 0
#endif
#ifndef MATERIAL_SUPPLY_MESH_PAINT
#define MATERIAL_SUPPLY_MESH_PAINT 0
#endif
#ifndef MATERIAL_SUPPLY_PHYSICS_FIELD
#define MATERIAL_SUPPLY_PHYSICS_FIELD 0
#endif
#ifndef MATERIAL_SUPPLY_SPEED_TREE
#define MATERIAL_SUPPLY_SPEED_TREE 0
#endif
#ifndef MATERIAL_SUPPLY_NEURAL
#define MATERIAL_SUPPLY_NEURAL 0
#endif
#ifndef MATERIAL_SUPPLY_FIRST_PERSON
#define MATERIAL_SUPPLY_FIRST_PERSON 0
#endif
#ifndef MATERIAL_SUPPLY_XR
#define MATERIAL_SUPPLY_XR 0
#endif
#ifndef MATERIAL_SUPPLY_PATH_TRACING
#define MATERIAL_SUPPLY_PATH_TRACING 0
#endif
#ifndef MATERIAL_SUPPLY_USER_INTERFACE
#define MATERIAL_SUPPLY_USER_INTERFACE 0
#endif

struct MaterialVertexInputs {
    float deltaTime;
    float4 particleColor;
    float3 particleDirection;
    float particleMotionBlurFade;
    float3 particlePositionTranslated;
    float3 previousParticlePositionTranslated;
    float particleRelativeTime;
    float particleRandom;
    float particleSpeed;
    float2 particleSize;
    float2 particleSubUvCoords0;
    float2 particleSubUvCoords1;
    float particleSubUvLerp;
    float3 preSkinnedPosition;
    float3 preSkinnedNormal;
    float4 vertexColor;
    float3 vertexNormal;
    float3 vertexTangent;
    float4x4 cameraViewToTranslatedWorld;
    float4x4 previousCameraViewToTranslatedWorld;
    float4x4 particleToWorld;
    float3x3 tangentToWorld;
    float4x4 translatedWorldToCameraView;
    float4x4 previousTranslatedWorldToCameraView;
    float4x4 translatedWorldToView;
    float4x4 previousTranslatedWorldToView;
    float4x4 viewToTranslatedWorld;
    float4x4 previousViewToTranslatedWorld;
    float4x4 worldToParticle;
    float time;
    float previousTime;
    float realTime;
    float previousRealTime;
    MaterialDouble3 cameraPosition;
    MaterialDouble3 previousCameraPosition;
    float2 viewSize;
    float2 sceneTexelSize;
    MaterialDouble3 worldPosition;
    MaterialDouble3 previousWorldPosition;
    float3 translatedWorldPosition;
    float3 previousTranslatedWorldPosition;
    MaterialDouble3 worldPositionNoShaderOffsets;
    float3 localPosition;
    float3 previousLocalPosition;
    float4 screenPosition;
    MaterialDouble3 objectPosition;
    MaterialDouble3 actorPosition;
    MaterialDouble3 previousActorPosition;
    float distanceCullFade;
};

struct MaterialInputs {
    float4 svPosition;
    float3 cameraVector;
    float deltaTime;
    float3 lightVector;
    float4 particleColor;
    float3 particleDirection;
    float particleMotionBlurFade;
    float3 particlePositionTranslated;
    float particleRelativeTime;
    float particleRandom;
    float particleSpeed;
    float2 particleSize;
    float2 particleSubUvCoords0;
    float2 particleSubUvCoords1;
    float particleSubUvLerp;
    float precomputedAoMask;
    float3 reflectionVector;
    float twoSidedSign;
    float4 vertexColor;
    float3 vertexNormal;
    float3 vertexTangent;
    float4x4 cameraViewToTranslatedWorld;
    float4x4 particleToWorld;
    float3x3 tangentToWorld;
    float4x4 translatedWorldToCameraView;
    float4x4 translatedWorldToView;
    float4x4 viewToTranslatedWorld;
    float4x4 worldToParticle;
    float time;
    float realTime;
    MaterialDouble3 cameraPosition;
    float2 viewSize;
    float2 sceneTexelSize;
    MaterialDouble3 worldPosition;
    float3 translatedWorldPosition;
    MaterialDouble3 worldPositionNoShaderOffsets;
    float3 localPosition;
    float3 pixelNormal;
    float4 screenPosition;
    MaterialDouble3 objectPosition;
    MaterialDouble3 actorPosition;
    float distanceCullFade;
};

// Accessors (MaterialContext<Name>, previous frame MaterialContextPrevious<Name>).

#if MATERIAL_SUPPLY_VIEW
#define MaterialContextCameraVector(c) ((c).cameraVector)
#define MaterialContextDeltaTime(c) ((c).deltaTime)
#define MaterialContextIsOrthographic(c) MaterialPassIsOrthographic(c)
#define MaterialContextPixelDepth(c) MaterialPassPixelDepth(c)
#define MaterialContextPixelPosition(c) MaterialPassPixelPosition(c)
#define MaterialContextReflectionVector(c) ((c).reflectionVector)
#define MaterialContextScalarBlueNoise(c) MaterialPassScalarBlueNoise(c)
#define MaterialContextViewportUv(c) MaterialPassViewportUv(c)
#define MaterialContextLightBakePass(c) MaterialPassLightBakePass(c)
#define MaterialContextVirtualizedGeometryPass(c) MaterialPassVirtualizedGeometryPass(c)
#define MaterialContextReflectionCapturePass(c) MaterialPassReflectionCapturePass(c)
#define MaterialContextShadowPass(c) MaterialPassShadowPass(c)
#define MaterialContextCameraViewToTranslatedWorld(c) ((c).cameraViewToTranslatedWorld)
#define MaterialContextPreviousCameraViewToTranslatedWorld(c) ((c).previousCameraViewToTranslatedWorld)
#define MaterialContextPreViewTranslation(c) MaterialPassPreViewTranslation(c)
#define MaterialContextPreviousPreViewTranslation(c) MaterialPassPreviousPreViewTranslation(c)
#define MaterialContextTranslatedWorldToCameraView(c) ((c).translatedWorldToCameraView)
#define MaterialContextPreviousTranslatedWorldToCameraView(c) ((c).previousTranslatedWorldToCameraView)
#define MaterialContextTranslatedWorldToView(c) ((c).translatedWorldToView)
#define MaterialContextPreviousTranslatedWorldToView(c) ((c).previousTranslatedWorldToView)
#define MaterialContextViewToTranslatedWorld(c) ((c).viewToTranslatedWorld)
#define MaterialContextPreviousViewToTranslatedWorld(c) ((c).previousViewToTranslatedWorld)
#define MaterialContextTime(c) ((c).time)
#define MaterialContextPreviousTime(c) ((c).previousTime)
#define MaterialContextRealTime(c) ((c).realTime)
#define MaterialContextPreviousRealTime(c) ((c).previousRealTime)
#define MaterialContextCameraPosition(c) ((c).cameraPosition)
#define MaterialContextPreviousCameraPosition(c) ((c).previousCameraPosition)
#define MaterialContextViewSize(c) ((c).viewSize)
#define MaterialContextSceneTexelSize(c) ((c).sceneTexelSize)
#define MaterialContextViewProperty(c, a0) MaterialPassViewProperty(c, a0)
#define MaterialContextPreviousViewProperty(c, a0) MaterialPassPreviousViewProperty(c, a0)
#define MaterialContextScreenPosition(c) ((c).screenPosition)
#else
#define MaterialContextCameraVector(c) float3(0.0, 0.0, 0.0)
#define MaterialContextDeltaTime(c) (0.0)
#define MaterialContextIsOrthographic(c) (0.0)
#define MaterialContextPixelDepth(c) (0.0)
#define MaterialContextPixelPosition(c) float2(0.0, 0.0)
#define MaterialContextReflectionVector(c) float3(0.0, 0.0, 0.0)
#define MaterialContextScalarBlueNoise(c) (0.0)
#define MaterialContextViewportUv(c) float2(0.0, 0.0)
#define MaterialContextLightBakePass(c) (false)
#define MaterialContextVirtualizedGeometryPass(c) (false)
#define MaterialContextReflectionCapturePass(c) (false)
#define MaterialContextShadowPass(c) (false)
#define MaterialContextCameraViewToTranslatedWorld(c) ((float4x4)0)
#define MaterialContextPreviousCameraViewToTranslatedWorld(c) ((float4x4)0)
#define MaterialContextPreViewTranslation(c) ((MaterialDouble3)0)
#define MaterialContextPreviousPreViewTranslation(c) ((MaterialDouble3)0)
#define MaterialContextTranslatedWorldToCameraView(c) ((float4x4)0)
#define MaterialContextPreviousTranslatedWorldToCameraView(c) ((float4x4)0)
#define MaterialContextTranslatedWorldToView(c) ((float4x4)0)
#define MaterialContextPreviousTranslatedWorldToView(c) ((float4x4)0)
#define MaterialContextViewToTranslatedWorld(c) ((float4x4)0)
#define MaterialContextPreviousViewToTranslatedWorld(c) ((float4x4)0)
#define MaterialContextTime(c) (0.0)
#define MaterialContextPreviousTime(c) (0.0)
#define MaterialContextRealTime(c) (0.0)
#define MaterialContextPreviousRealTime(c) (0.0)
#define MaterialContextCameraPosition(c) ((MaterialDouble3)0)
#define MaterialContextPreviousCameraPosition(c) ((MaterialDouble3)0)
#define MaterialContextViewSize(c) float2(0.0, 0.0)
#define MaterialContextSceneTexelSize(c) float2(0.0, 0.0)
#define MaterialContextViewProperty(c, a0) float4(0.0, 0.0, 0.0, 0.0)
#define MaterialContextPreviousViewProperty(c, a0) float4(0.0, 0.0, 0.0, 0.0)
#define MaterialContextScreenPosition(c) float4(0.0, 0.0, 0.0, 0.0)
#endif

#if MATERIAL_SUPPLY_VERTEX
#define MaterialContextTwoSidedSign(c) ((c).twoSidedSign)
#define MaterialContextVertexColor(c) ((c).vertexColor)
#define MaterialContextVertexNormal(c) ((c).vertexNormal)
#define MaterialContextVertexTangent(c) ((c).vertexTangent)
#define MaterialContextTangentToWorld(c) ((c).tangentToWorld)
#define MaterialContextTexCoord(c, a0) MaterialPassTexCoord(c, a0)
#define MaterialContextWorldPosition(c) ((c).worldPosition)
#define MaterialContextPreviousWorldPosition(c) ((c).previousWorldPosition)
#define MaterialContextTranslatedWorldPosition(c) ((c).translatedWorldPosition)
#define MaterialContextPreviousTranslatedWorldPosition(c) ((c).previousTranslatedWorldPosition)
#define MaterialContextWorldPositionNoShaderOffsets(c) ((c).worldPositionNoShaderOffsets)
#define MaterialContextLocalPosition(c) ((c).localPosition)
#define MaterialContextPreviousLocalPosition(c) ((c).previousLocalPosition)
#define MaterialContextPixelNormal(c) ((c).pixelNormal)
#else
#define MaterialContextTwoSidedSign(c) (0.0)
#define MaterialContextVertexColor(c) float4(0.0, 0.0, 0.0, 0.0)
#define MaterialContextVertexNormal(c) float3(0.0, 0.0, 0.0)
#define MaterialContextVertexTangent(c) float3(0.0, 0.0, 0.0)
#define MaterialContextTangentToWorld(c) ((float3x3)0)
#define MaterialContextTexCoord(c, a0) float2(0.0, 0.0)
#define MaterialContextWorldPosition(c) ((MaterialDouble3)0)
#define MaterialContextPreviousWorldPosition(c) ((MaterialDouble3)0)
#define MaterialContextTranslatedWorldPosition(c) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTranslatedWorldPosition(c) float3(0.0, 0.0, 0.0)
#define MaterialContextWorldPositionNoShaderOffsets(c) ((MaterialDouble3)0)
#define MaterialContextLocalPosition(c) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousLocalPosition(c) float3(0.0, 0.0, 0.0)
#define MaterialContextPixelNormal(c) float3(0.0, 0.0, 0.0)
#endif

#if MATERIAL_SUPPLY_PRIMITIVE
#define MaterialContextObjectBounds(c) MaterialPassObjectBounds(c)
#define MaterialContextObjectOrientation(c) MaterialPassObjectOrientation(c)
#define MaterialContextObjectRadius(c) MaterialPassObjectRadius(c)
#define MaterialContextPerInstanceCustomData(c, a0, a1) MaterialPassPerInstanceCustomData(c, a0, a1)
#define MaterialContextPerInstanceCustomData3Vector(c, a0, a1) MaterialPassPerInstanceCustomData3Vector(c, a0, a1)
#define MaterialContextPerInstanceFadeAmount(c) MaterialPassPerInstanceFadeAmount(c)
#define MaterialContextPerInstanceRandom(c) MaterialPassPerInstanceRandom(c)
#define MaterialContextInstanceLocalBoundsHalf(c) MaterialPassInstanceLocalBoundsHalf(c)
#define MaterialContextInstanceLocalBoundsFull(c) MaterialPassInstanceLocalBoundsFull(c)
#define MaterialContextInstanceLocalBoundsMin(c) MaterialPassInstanceLocalBoundsMin(c)
#define MaterialContextInstanceLocalBoundsMax(c) MaterialPassInstanceLocalBoundsMax(c)
#define MaterialContextObjectLocalBoundsHalf(c) MaterialPassObjectLocalBoundsHalf(c)
#define MaterialContextObjectLocalBoundsFull(c) MaterialPassObjectLocalBoundsFull(c)
#define MaterialContextObjectLocalBoundsMin(c) MaterialPassObjectLocalBoundsMin(c)
#define MaterialContextObjectLocalBoundsMax(c) MaterialPassObjectLocalBoundsMax(c)
#define MaterialContextPreSkinnedLocalBoundsHalf(c) MaterialPassPreSkinnedLocalBoundsHalf(c)
#define MaterialContextPreSkinnedLocalBoundsFull(c) MaterialPassPreSkinnedLocalBoundsFull(c)
#define MaterialContextPreSkinnedLocalBoundsMin(c) MaterialPassPreSkinnedLocalBoundsMin(c)
#define MaterialContextPreSkinnedLocalBoundsMax(c) MaterialPassPreSkinnedLocalBoundsMax(c)
#define MaterialContextInstanceToWorld(c) MaterialPassInstanceToWorld(c)
#define MaterialContextPreviousInstanceToWorld(c) MaterialPassPreviousInstanceToWorld(c)
#define MaterialContextInstanceToWorldDouble(c) MaterialPassInstanceToWorldDouble(c)
#define MaterialContextPreviousInstanceToWorldDouble(c) MaterialPassPreviousInstanceToWorldDouble(c)
#define MaterialContextLocalToWorldDouble(c) MaterialPassLocalToWorldDouble(c)
#define MaterialContextPreviousLocalToWorldDouble(c) MaterialPassPreviousLocalToWorldDouble(c)
#define MaterialContextWorldToInstance(c) MaterialPassWorldToInstance(c)
#define MaterialContextWorldToInstanceDouble(c) MaterialPassWorldToInstanceDouble(c)
#define MaterialContextWorldToLocal(c) MaterialPassWorldToLocal(c)
#define MaterialContextPreviousWorldToLocal(c) MaterialPassPreviousWorldToLocal(c)
#define MaterialContextWorldToLocalDouble(c) MaterialPassWorldToLocalDouble(c)
#define MaterialContextPreviousWorldToLocalDouble(c) MaterialPassPreviousWorldToLocalDouble(c)
#define MaterialContextObjectPosition(c) ((c).objectPosition)
#define MaterialContextActorPosition(c) ((c).actorPosition)
#define MaterialContextPreviousActorPosition(c) ((c).previousActorPosition)
#define MaterialContextDistanceCullFade(c) ((c).distanceCullFade)
#else
#define MaterialContextObjectBounds(c) float3(0.0, 0.0, 0.0)
#define MaterialContextObjectOrientation(c) float3(0.0, 0.0, 0.0)
#define MaterialContextObjectRadius(c) (0.0)
#define MaterialContextPerInstanceCustomData(c, a0, a1) (0.0)
#define MaterialContextPerInstanceCustomData3Vector(c, a0, a1) float3(0.0, 0.0, 0.0)
#define MaterialContextPerInstanceFadeAmount(c) (0.0)
#define MaterialContextPerInstanceRandom(c) (0.0)
#define MaterialContextInstanceLocalBoundsHalf(c) float3(0.0, 0.0, 0.0)
#define MaterialContextInstanceLocalBoundsFull(c) float3(0.0, 0.0, 0.0)
#define MaterialContextInstanceLocalBoundsMin(c) float3(0.0, 0.0, 0.0)
#define MaterialContextInstanceLocalBoundsMax(c) float3(0.0, 0.0, 0.0)
#define MaterialContextObjectLocalBoundsHalf(c) float3(0.0, 0.0, 0.0)
#define MaterialContextObjectLocalBoundsFull(c) float3(0.0, 0.0, 0.0)
#define MaterialContextObjectLocalBoundsMin(c) float3(0.0, 0.0, 0.0)
#define MaterialContextObjectLocalBoundsMax(c) float3(0.0, 0.0, 0.0)
#define MaterialContextPreSkinnedLocalBoundsHalf(c) float3(0.0, 0.0, 0.0)
#define MaterialContextPreSkinnedLocalBoundsFull(c) float3(0.0, 0.0, 0.0)
#define MaterialContextPreSkinnedLocalBoundsMin(c) float3(0.0, 0.0, 0.0)
#define MaterialContextPreSkinnedLocalBoundsMax(c) float3(0.0, 0.0, 0.0)
#define MaterialContextInstanceToWorld(c) ((float4x4)0)
#define MaterialContextPreviousInstanceToWorld(c) ((float4x4)0)
#define MaterialContextInstanceToWorldDouble(c) ((float4x4)0)
#define MaterialContextPreviousInstanceToWorldDouble(c) ((float4x4)0)
#define MaterialContextLocalToWorldDouble(c) ((float4x4)0)
#define MaterialContextPreviousLocalToWorldDouble(c) ((float4x4)0)
#define MaterialContextWorldToInstance(c) ((float4x4)0)
#define MaterialContextWorldToInstanceDouble(c) ((float4x4)0)
#define MaterialContextWorldToLocal(c) ((float4x4)0)
#define MaterialContextPreviousWorldToLocal(c) ((float4x4)0)
#define MaterialContextWorldToLocalDouble(c) ((float4x4)0)
#define MaterialContextPreviousWorldToLocalDouble(c) ((float4x4)0)
#define MaterialContextObjectPosition(c) ((MaterialDouble3)0)
#define MaterialContextActorPosition(c) ((MaterialDouble3)0)
#define MaterialContextPreviousActorPosition(c) ((MaterialDouble3)0)
#define MaterialContextDistanceCullFade(c) (0.0)
#endif

#if MATERIAL_SUPPLY_SKIN
#define MaterialContextPreSkinnedPosition(c) ((c).preSkinnedPosition)
#define MaterialContextPreSkinnedNormal(c) ((c).preSkinnedNormal)
#else
#define MaterialContextPreSkinnedPosition(c) float3(0.0, 0.0, 0.0)
#define MaterialContextPreSkinnedNormal(c) float3(0.0, 0.0, 0.0)
#endif

#if MATERIAL_SUPPLY_LIGHTMAP
#define MaterialContextLightmapUvs(c) MaterialPassLightmapUvs(c)
#define MaterialContextPrecomputedAoMask(c) ((c).precomputedAoMask)
#else
#define MaterialContextLightmapUvs(c) float2(0.0, 0.0)
#define MaterialContextPrecomputedAoMask(c) (0.0)
#endif

#if MATERIAL_SUPPLY_LIGHT
#define MaterialContextLightVector(c) ((c).lightVector)
#else
#define MaterialContextLightVector(c) float3(0.0, 0.0, 0.0)
#endif

#if MATERIAL_SUPPLY_SKY
#define MaterialContextAtmosphericLightVector(c) MaterialPassAtmosphericLightVector(c)
#define MaterialContextAtmosphericLightColor(c) MaterialPassAtmosphericLightColor(c)
#define MaterialContextSkyAtmosphereAerialPerspective(c, a0) MaterialPassSkyAtmosphereAerialPerspective(c, a0)
#define MaterialContextSkyAtmosphereDistantLightScatteredLuminance(c) MaterialPassSkyAtmosphereDistantLightScatteredLuminance(c)
#define MaterialContextSkyAtmosphereLightDirection(c, a0) MaterialPassSkyAtmosphereLightDirection(c, a0)
#define MaterialContextSkyAtmosphereLightDiskLuminance(c, a0, a1) MaterialPassSkyAtmosphereLightDiskLuminance(c, a0, a1)
#define MaterialContextSkyAtmosphereLightIlluminance(c, a0, a1) MaterialPassSkyAtmosphereLightIlluminance(c, a0, a1)
#define MaterialContextSkyAtmosphereLightIlluminanceOnGround(c, a0) MaterialPassSkyAtmosphereLightIlluminanceOnGround(c, a0)
#define MaterialContextSkyAtmosphereViewLuminance(c, a0) MaterialPassSkyAtmosphereViewLuminance(c, a0)
#define MaterialContextSkyLightEnvironmentSample(c, a0, a1) MaterialPassSkyLightEnvironmentSample(c, a0, a1)
#else
#define MaterialContextAtmosphericLightVector(c) float3(0.0, 0.0, 0.0)
#define MaterialContextAtmosphericLightColor(c) float3(0.0, 0.0, 0.0)
#define MaterialContextSkyAtmosphereAerialPerspective(c, a0) float4(0.0, 0.0, 0.0, 0.0)
#define MaterialContextSkyAtmosphereDistantLightScatteredLuminance(c) float3(0.0, 0.0, 0.0)
#define MaterialContextSkyAtmosphereLightDirection(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextSkyAtmosphereLightDiskLuminance(c, a0, a1) float3(0.0, 0.0, 0.0)
#define MaterialContextSkyAtmosphereLightIlluminance(c, a0, a1) float3(0.0, 0.0, 0.0)
#define MaterialContextSkyAtmosphereLightIlluminanceOnGround(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextSkyAtmosphereViewLuminance(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextSkyLightEnvironmentSample(c, a0, a1) float3(0.0, 0.0, 0.0)
#endif

#if MATERIAL_SUPPLY_POST_PROCESS
#define MaterialContextEyeAdaptation(c) MaterialPassEyeAdaptation(c)
#define MaterialContextEyeAdaptationInverse(c, a0) MaterialPassEyeAdaptationInverse(c, a0)
#define MaterialContextDepthOfFieldFunction(c, a0, a1) MaterialPassDepthOfFieldFunction(c, a0, a1)
#else
#define MaterialContextEyeAdaptation(c) (0.0)
#define MaterialContextEyeAdaptationInverse(c, a0) (0.0)
#define MaterialContextDepthOfFieldFunction(c, a0, a1) (0.0)
#endif

#if MATERIAL_SUPPLY_DECAL
#define MaterialContextDecalColor(c) MaterialPassDecalColor(c)
#define MaterialContextDecalLifetimeOpacity(c) MaterialPassDecalLifetimeOpacity(c)
#define MaterialContextDecalMipmapLevel(c, a0) MaterialPassDecalMipmapLevel(c, a0)
#else
#define MaterialContextDecalColor(c) float4(0.0, 0.0, 0.0, 0.0)
#define MaterialContextDecalLifetimeOpacity(c) (0.0)
#define MaterialContextDecalMipmapLevel(c, a0) (0.0)
#endif

#if MATERIAL_SUPPLY_PARTICLE
#define MaterialContextParticleColor(c) ((c).particleColor)
#define MaterialContextParticleDirection(c) ((c).particleDirection)
#define MaterialContextParticleMacroUv(c) MaterialPassParticleMacroUv(c)
#define MaterialContextParticleMotionBlurFade(c) ((c).particleMotionBlurFade)
#define MaterialContextParticlePositionAbsolute(c) MaterialPassParticlePositionAbsolute(c)
#define MaterialContextPreviousParticlePositionAbsolute(c) MaterialPassPreviousParticlePositionAbsolute(c)
#define MaterialContextParticlePositionTranslated(c) ((c).particlePositionTranslated)
#define MaterialContextPreviousParticlePositionTranslated(c) ((c).previousParticlePositionTranslated)
#define MaterialContextParticleRadius(c) MaterialPassParticleRadius(c)
#define MaterialContextParticleRelativeTime(c) ((c).particleRelativeTime)
#define MaterialContextParticleRandom(c) ((c).particleRandom)
#define MaterialContextParticleSpeed(c) ((c).particleSpeed)
#define MaterialContextParticleSize(c) ((c).particleSize)
#define MaterialContextParticleSpriteRotation(c) MaterialPassParticleSpriteRotation(c)
#define MaterialContextParticleSubUvCoords0(c) ((c).particleSubUvCoords0)
#define MaterialContextParticleSubUvCoords1(c) ((c).particleSubUvCoords1)
#define MaterialContextParticleSubUvLerp(c) ((c).particleSubUvLerp)
#define MaterialContextSphericalParticleOpacity(c, a0) MaterialPassSphericalParticleOpacity(c, a0)
#define MaterialContextGetParticleToWorld(c) MaterialPassGetParticleToWorld(c)
#define MaterialContextGetWorldToParticle(c) MaterialPassGetWorldToParticle(c)
#define MaterialContextParticleToWorld(c) ((c).particleToWorld)
#define MaterialContextWorldToParticle(c) ((c).worldToParticle)
#else
#define MaterialContextParticleColor(c) float4(1.0, 1.0, 1.0, 1.0)
#define MaterialContextParticleDirection(c) float3(0.0, 0.0, 0.0)
#define MaterialContextParticleMacroUv(c) float2(0.0, 0.0)
#define MaterialContextParticleMotionBlurFade(c) (0.0)
#define MaterialContextParticlePositionAbsolute(c) ((MaterialDouble3)0)
#define MaterialContextPreviousParticlePositionAbsolute(c) ((MaterialDouble3)0)
#define MaterialContextParticlePositionTranslated(c) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousParticlePositionTranslated(c) float3(0.0, 0.0, 0.0)
#define MaterialContextParticleRadius(c) (0.0)
#define MaterialContextParticleRelativeTime(c) (0.0)
#define MaterialContextParticleRandom(c) (0.0)
#define MaterialContextParticleSpeed(c) (0.0)
#define MaterialContextParticleSize(c) float2(0.0, 0.0)
#define MaterialContextParticleSpriteRotation(c) float2(0.0, 0.0)
#define MaterialContextParticleSubUvCoords0(c) float2(0.0, 0.0)
#define MaterialContextParticleSubUvCoords1(c) float2(0.0, 0.0)
#define MaterialContextParticleSubUvLerp(c) (0.0)
#define MaterialContextSphericalParticleOpacity(c, a0) (0.0)
#define MaterialContextGetParticleToWorld(c) ((float4x4)0)
#define MaterialContextGetWorldToParticle(c) ((float4x4)0)
#define MaterialContextParticleToWorld(c) ((float4x4)0)
#define MaterialContextWorldToParticle(c) ((float4x4)0)
#endif

#if MATERIAL_SUPPLY_VOLUME
#define MaterialContextCloudSampleAltitude(c) MaterialPassCloudSampleAltitude(c)
#define MaterialContextCloudSampleAltitudeInLayer(c) MaterialPassCloudSampleAltitudeInLayer(c)
#define MaterialContextCloudSampleNormAltitudeInLayer(c) MaterialPassCloudSampleNormAltitudeInLayer(c)
#define MaterialContextCloudSampleShadowSampleDistance(c) MaterialPassCloudSampleShadowSampleDistance(c)
#define MaterialContextVolumeSampleConservativeDensity(c) MaterialPassVolumeSampleConservativeDensity(c)
#define MaterialContextCloudEmptySpaceSkippingSphereCenterWorldPosition(c) MaterialPassCloudEmptySpaceSkippingSphereCenterWorldPosition(c)
#define MaterialContextCloudEmptySpaceSkippingSphereRadius(c) MaterialPassCloudEmptySpaceSkippingSphereRadius(c)
#else
#define MaterialContextCloudSampleAltitude(c) (0.0)
#define MaterialContextCloudSampleAltitudeInLayer(c) (0.0)
#define MaterialContextCloudSampleNormAltitudeInLayer(c) (0.0)
#define MaterialContextCloudSampleShadowSampleDistance(c) (0.0)
#define MaterialContextVolumeSampleConservativeDensity(c) float4(0.0, 0.0, 0.0, 0.0)
#define MaterialContextCloudEmptySpaceSkippingSphereCenterWorldPosition(c) float3(0.0, 0.0, 0.0)
#define MaterialContextCloudEmptySpaceSkippingSphereRadius(c) (0.0)
#endif

#if MATERIAL_SUPPLY_MESH_PAINT
#define MaterialContextMeshPaintTextureCoordinateIndex(c) MaterialPassMeshPaintTextureCoordinateIndex(c)
#define MaterialContextMeshPaintTextureValid(c) MaterialPassMeshPaintTextureValid(c)
#else
#define MaterialContextMeshPaintTextureCoordinateIndex(c) (0.0)
#define MaterialContextMeshPaintTextureValid(c) (false)
#endif

#if MATERIAL_SUPPLY_FIRST_PERSON
#define MaterialContextIsFirstPerson(c) MaterialPassIsFirstPerson(c)
#else
#define MaterialContextIsFirstPerson(c) (0.0)
#endif

#if MATERIAL_SUPPLY_USER_INTERFACE
#define MaterialContextFontSignedDistanceData(c) MaterialPassFontSignedDistanceData(c)
#else
#define MaterialContextFontSignedDistanceData(c) float4(0.0, 0.0, 0.0, 0.0)
#endif

// Features the pass must fill: supplied and used by any entry point of the material.
#define MATERIAL_CONTEXT_VIEW (MATERIAL_SUPPLY_VIEW && (MATERIAL_VERTEX_USES_VIEW || MATERIAL_VERTEX_PREVIOUS_USES_VIEW || MATERIAL_SURFACE_USES_VIEW || MATERIAL_OPACITY_USES_VIEW || MATERIAL_OPACITY_MASK_USES_VIEW || MATERIAL_PIXEL_DEPTH_OFFSET_USES_VIEW || MATERIAL_CUSTOM_OUTPUT_USES_VIEW || MATERIAL_PREVIEW_USES_VIEW))
#define MATERIAL_CONTEXT_VERTEX (MATERIAL_SUPPLY_VERTEX && (MATERIAL_VERTEX_USES_VERTEX || MATERIAL_VERTEX_PREVIOUS_USES_VERTEX || MATERIAL_SURFACE_USES_VERTEX || MATERIAL_OPACITY_USES_VERTEX || MATERIAL_OPACITY_MASK_USES_VERTEX || MATERIAL_PIXEL_DEPTH_OFFSET_USES_VERTEX || MATERIAL_CUSTOM_OUTPUT_USES_VERTEX || MATERIAL_PREVIEW_USES_VERTEX))
#define MATERIAL_CONTEXT_PRIMITIVE (MATERIAL_SUPPLY_PRIMITIVE && (MATERIAL_VERTEX_USES_PRIMITIVE || MATERIAL_VERTEX_PREVIOUS_USES_PRIMITIVE || MATERIAL_SURFACE_USES_PRIMITIVE || MATERIAL_OPACITY_USES_PRIMITIVE || MATERIAL_OPACITY_MASK_USES_PRIMITIVE || MATERIAL_PIXEL_DEPTH_OFFSET_USES_PRIMITIVE || MATERIAL_CUSTOM_OUTPUT_USES_PRIMITIVE || MATERIAL_PREVIEW_USES_PRIMITIVE))
#define MATERIAL_CONTEXT_SKIN (MATERIAL_SUPPLY_SKIN && (MATERIAL_VERTEX_USES_SKIN || MATERIAL_VERTEX_PREVIOUS_USES_SKIN || MATERIAL_SURFACE_USES_SKIN || MATERIAL_OPACITY_USES_SKIN || MATERIAL_OPACITY_MASK_USES_SKIN || MATERIAL_PIXEL_DEPTH_OFFSET_USES_SKIN || MATERIAL_CUSTOM_OUTPUT_USES_SKIN || MATERIAL_PREVIEW_USES_SKIN))
#define MATERIAL_CONTEXT_LIGHTMAP (MATERIAL_SUPPLY_LIGHTMAP && (MATERIAL_VERTEX_USES_LIGHTMAP || MATERIAL_VERTEX_PREVIOUS_USES_LIGHTMAP || MATERIAL_SURFACE_USES_LIGHTMAP || MATERIAL_OPACITY_USES_LIGHTMAP || MATERIAL_OPACITY_MASK_USES_LIGHTMAP || MATERIAL_PIXEL_DEPTH_OFFSET_USES_LIGHTMAP || MATERIAL_CUSTOM_OUTPUT_USES_LIGHTMAP || MATERIAL_PREVIEW_USES_LIGHTMAP))
#define MATERIAL_CONTEXT_LIGHT (MATERIAL_SUPPLY_LIGHT && (MATERIAL_VERTEX_USES_LIGHT || MATERIAL_VERTEX_PREVIOUS_USES_LIGHT || MATERIAL_SURFACE_USES_LIGHT || MATERIAL_OPACITY_USES_LIGHT || MATERIAL_OPACITY_MASK_USES_LIGHT || MATERIAL_PIXEL_DEPTH_OFFSET_USES_LIGHT || MATERIAL_CUSTOM_OUTPUT_USES_LIGHT || MATERIAL_PREVIEW_USES_LIGHT))
#define MATERIAL_CONTEXT_SKY (MATERIAL_SUPPLY_SKY && (MATERIAL_VERTEX_USES_SKY || MATERIAL_VERTEX_PREVIOUS_USES_SKY || MATERIAL_SURFACE_USES_SKY || MATERIAL_OPACITY_USES_SKY || MATERIAL_OPACITY_MASK_USES_SKY || MATERIAL_PIXEL_DEPTH_OFFSET_USES_SKY || MATERIAL_CUSTOM_OUTPUT_USES_SKY || MATERIAL_PREVIEW_USES_SKY))
#define MATERIAL_CONTEXT_POST_PROCESS (MATERIAL_SUPPLY_POST_PROCESS && (MATERIAL_VERTEX_USES_POST_PROCESS || MATERIAL_VERTEX_PREVIOUS_USES_POST_PROCESS || MATERIAL_SURFACE_USES_POST_PROCESS || MATERIAL_OPACITY_USES_POST_PROCESS || MATERIAL_OPACITY_MASK_USES_POST_PROCESS || MATERIAL_PIXEL_DEPTH_OFFSET_USES_POST_PROCESS || MATERIAL_CUSTOM_OUTPUT_USES_POST_PROCESS || MATERIAL_PREVIEW_USES_POST_PROCESS))
#define MATERIAL_CONTEXT_SCENE (MATERIAL_SUPPLY_SCENE && (MATERIAL_VERTEX_USES_SCENE || MATERIAL_VERTEX_PREVIOUS_USES_SCENE || MATERIAL_SURFACE_USES_SCENE || MATERIAL_OPACITY_USES_SCENE || MATERIAL_OPACITY_MASK_USES_SCENE || MATERIAL_PIXEL_DEPTH_OFFSET_USES_SCENE || MATERIAL_CUSTOM_OUTPUT_USES_SCENE || MATERIAL_PREVIEW_USES_SCENE))
#define MATERIAL_CONTEXT_DECAL (MATERIAL_SUPPLY_DECAL && (MATERIAL_VERTEX_USES_DECAL || MATERIAL_VERTEX_PREVIOUS_USES_DECAL || MATERIAL_SURFACE_USES_DECAL || MATERIAL_OPACITY_USES_DECAL || MATERIAL_OPACITY_MASK_USES_DECAL || MATERIAL_PIXEL_DEPTH_OFFSET_USES_DECAL || MATERIAL_CUSTOM_OUTPUT_USES_DECAL || MATERIAL_PREVIEW_USES_DECAL))
#define MATERIAL_CONTEXT_PARTICLE (MATERIAL_SUPPLY_PARTICLE && (MATERIAL_VERTEX_USES_PARTICLE || MATERIAL_VERTEX_PREVIOUS_USES_PARTICLE || MATERIAL_SURFACE_USES_PARTICLE || MATERIAL_OPACITY_USES_PARTICLE || MATERIAL_OPACITY_MASK_USES_PARTICLE || MATERIAL_PIXEL_DEPTH_OFFSET_USES_PARTICLE || MATERIAL_CUSTOM_OUTPUT_USES_PARTICLE || MATERIAL_PREVIEW_USES_PARTICLE))
#define MATERIAL_CONTEXT_DISTANCE_FIELD (MATERIAL_SUPPLY_DISTANCE_FIELD && (MATERIAL_VERTEX_USES_DISTANCE_FIELD || MATERIAL_VERTEX_PREVIOUS_USES_DISTANCE_FIELD || MATERIAL_SURFACE_USES_DISTANCE_FIELD || MATERIAL_OPACITY_USES_DISTANCE_FIELD || MATERIAL_OPACITY_MASK_USES_DISTANCE_FIELD || MATERIAL_PIXEL_DEPTH_OFFSET_USES_DISTANCE_FIELD || MATERIAL_CUSTOM_OUTPUT_USES_DISTANCE_FIELD || MATERIAL_PREVIEW_USES_DISTANCE_FIELD))
#define MATERIAL_CONTEXT_HAIR (MATERIAL_SUPPLY_HAIR && (MATERIAL_VERTEX_USES_HAIR || MATERIAL_VERTEX_PREVIOUS_USES_HAIR || MATERIAL_SURFACE_USES_HAIR || MATERIAL_OPACITY_USES_HAIR || MATERIAL_OPACITY_MASK_USES_HAIR || MATERIAL_PIXEL_DEPTH_OFFSET_USES_HAIR || MATERIAL_CUSTOM_OUTPUT_USES_HAIR || MATERIAL_PREVIEW_USES_HAIR))
#define MATERIAL_CONTEXT_WATER (MATERIAL_SUPPLY_WATER && (MATERIAL_VERTEX_USES_WATER || MATERIAL_VERTEX_PREVIOUS_USES_WATER || MATERIAL_SURFACE_USES_WATER || MATERIAL_OPACITY_USES_WATER || MATERIAL_OPACITY_MASK_USES_WATER || MATERIAL_PIXEL_DEPTH_OFFSET_USES_WATER || MATERIAL_CUSTOM_OUTPUT_USES_WATER || MATERIAL_PREVIEW_USES_WATER))
#define MATERIAL_CONTEXT_VOLUME (MATERIAL_SUPPLY_VOLUME && (MATERIAL_VERTEX_USES_VOLUME || MATERIAL_VERTEX_PREVIOUS_USES_VOLUME || MATERIAL_SURFACE_USES_VOLUME || MATERIAL_OPACITY_USES_VOLUME || MATERIAL_OPACITY_MASK_USES_VOLUME || MATERIAL_PIXEL_DEPTH_OFFSET_USES_VOLUME || MATERIAL_CUSTOM_OUTPUT_USES_VOLUME || MATERIAL_PREVIEW_USES_VOLUME))
#define MATERIAL_CONTEXT_RUNTIME_VIRTUAL_TEXTURE (MATERIAL_SUPPLY_RUNTIME_VIRTUAL_TEXTURE && (MATERIAL_VERTEX_USES_RUNTIME_VIRTUAL_TEXTURE || MATERIAL_VERTEX_PREVIOUS_USES_RUNTIME_VIRTUAL_TEXTURE || MATERIAL_SURFACE_USES_RUNTIME_VIRTUAL_TEXTURE || MATERIAL_OPACITY_USES_RUNTIME_VIRTUAL_TEXTURE || MATERIAL_OPACITY_MASK_USES_RUNTIME_VIRTUAL_TEXTURE || MATERIAL_PIXEL_DEPTH_OFFSET_USES_RUNTIME_VIRTUAL_TEXTURE || MATERIAL_CUSTOM_OUTPUT_USES_RUNTIME_VIRTUAL_TEXTURE || MATERIAL_PREVIEW_USES_RUNTIME_VIRTUAL_TEXTURE))
#define MATERIAL_CONTEXT_MESH_PAINT (MATERIAL_SUPPLY_MESH_PAINT && (MATERIAL_VERTEX_USES_MESH_PAINT || MATERIAL_VERTEX_PREVIOUS_USES_MESH_PAINT || MATERIAL_SURFACE_USES_MESH_PAINT || MATERIAL_OPACITY_USES_MESH_PAINT || MATERIAL_OPACITY_MASK_USES_MESH_PAINT || MATERIAL_PIXEL_DEPTH_OFFSET_USES_MESH_PAINT || MATERIAL_CUSTOM_OUTPUT_USES_MESH_PAINT || MATERIAL_PREVIEW_USES_MESH_PAINT))
#define MATERIAL_CONTEXT_PHYSICS_FIELD (MATERIAL_SUPPLY_PHYSICS_FIELD && (MATERIAL_VERTEX_USES_PHYSICS_FIELD || MATERIAL_VERTEX_PREVIOUS_USES_PHYSICS_FIELD || MATERIAL_SURFACE_USES_PHYSICS_FIELD || MATERIAL_OPACITY_USES_PHYSICS_FIELD || MATERIAL_OPACITY_MASK_USES_PHYSICS_FIELD || MATERIAL_PIXEL_DEPTH_OFFSET_USES_PHYSICS_FIELD || MATERIAL_CUSTOM_OUTPUT_USES_PHYSICS_FIELD || MATERIAL_PREVIEW_USES_PHYSICS_FIELD))
#define MATERIAL_CONTEXT_SPEED_TREE (MATERIAL_SUPPLY_SPEED_TREE && (MATERIAL_VERTEX_USES_SPEED_TREE || MATERIAL_VERTEX_PREVIOUS_USES_SPEED_TREE || MATERIAL_SURFACE_USES_SPEED_TREE || MATERIAL_OPACITY_USES_SPEED_TREE || MATERIAL_OPACITY_MASK_USES_SPEED_TREE || MATERIAL_PIXEL_DEPTH_OFFSET_USES_SPEED_TREE || MATERIAL_CUSTOM_OUTPUT_USES_SPEED_TREE || MATERIAL_PREVIEW_USES_SPEED_TREE))
#define MATERIAL_CONTEXT_NEURAL (MATERIAL_SUPPLY_NEURAL && (MATERIAL_VERTEX_USES_NEURAL || MATERIAL_VERTEX_PREVIOUS_USES_NEURAL || MATERIAL_SURFACE_USES_NEURAL || MATERIAL_OPACITY_USES_NEURAL || MATERIAL_OPACITY_MASK_USES_NEURAL || MATERIAL_PIXEL_DEPTH_OFFSET_USES_NEURAL || MATERIAL_CUSTOM_OUTPUT_USES_NEURAL || MATERIAL_PREVIEW_USES_NEURAL))
#define MATERIAL_CONTEXT_FIRST_PERSON (MATERIAL_SUPPLY_FIRST_PERSON && (MATERIAL_VERTEX_USES_FIRST_PERSON || MATERIAL_VERTEX_PREVIOUS_USES_FIRST_PERSON || MATERIAL_SURFACE_USES_FIRST_PERSON || MATERIAL_OPACITY_USES_FIRST_PERSON || MATERIAL_OPACITY_MASK_USES_FIRST_PERSON || MATERIAL_PIXEL_DEPTH_OFFSET_USES_FIRST_PERSON || MATERIAL_CUSTOM_OUTPUT_USES_FIRST_PERSON || MATERIAL_PREVIEW_USES_FIRST_PERSON))
#define MATERIAL_CONTEXT_XR (MATERIAL_SUPPLY_XR && (MATERIAL_VERTEX_USES_XR || MATERIAL_VERTEX_PREVIOUS_USES_XR || MATERIAL_SURFACE_USES_XR || MATERIAL_OPACITY_USES_XR || MATERIAL_OPACITY_MASK_USES_XR || MATERIAL_PIXEL_DEPTH_OFFSET_USES_XR || MATERIAL_CUSTOM_OUTPUT_USES_XR || MATERIAL_PREVIEW_USES_XR))
#define MATERIAL_CONTEXT_PATH_TRACING (MATERIAL_SUPPLY_PATH_TRACING && (MATERIAL_VERTEX_USES_PATH_TRACING || MATERIAL_VERTEX_PREVIOUS_USES_PATH_TRACING || MATERIAL_SURFACE_USES_PATH_TRACING || MATERIAL_OPACITY_USES_PATH_TRACING || MATERIAL_OPACITY_MASK_USES_PATH_TRACING || MATERIAL_PIXEL_DEPTH_OFFSET_USES_PATH_TRACING || MATERIAL_CUSTOM_OUTPUT_USES_PATH_TRACING || MATERIAL_PREVIEW_USES_PATH_TRACING))
#define MATERIAL_CONTEXT_USER_INTERFACE (MATERIAL_SUPPLY_USER_INTERFACE && (MATERIAL_VERTEX_USES_USER_INTERFACE || MATERIAL_VERTEX_PREVIOUS_USES_USER_INTERFACE || MATERIAL_SURFACE_USES_USER_INTERFACE || MATERIAL_OPACITY_USES_USER_INTERFACE || MATERIAL_OPACITY_MASK_USES_USER_INTERFACE || MATERIAL_PIXEL_DEPTH_OFFSET_USES_USER_INTERFACE || MATERIAL_CUSTOM_OUTPUT_USES_USER_INTERFACE || MATERIAL_PREVIEW_USES_USER_INTERFACE))
