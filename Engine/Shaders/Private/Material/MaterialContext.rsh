// Generated from the material context value table by WriteMaterialContextSource
// (Docs/Design/MaterialSystem 6.9, 19.3). Do not edit: change the table and regenerate.
//
// A pass defines MATERIAL_SUPPLY_<FEATURE> 1 for every feature its shader type and vertex
// input type supply before including MaterialCommon.rsh. Accessors of features that are not
// supplied return the Unreal Engine default value, so generated material source compiles in
// every pass.
#pragma once

#include "/Engine/Private/Material/MaterialDouble.rsh"

// UV channel and custom interpolant vector limits (Docs/Design/MaterialSystem 6.9).
#define MATERIAL_MAX_TEXCOORDS 8
#define MATERIAL_MAX_INTERPOLANT_VECTORS 16

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
    float3 primitivePosition;
    float3 previousPrimitivePosition;
    float3 objectTranslatedPosition;
    float3 actorTranslatedPosition;
    float3 previousActorTranslatedPosition;
    float4 xrPassthroughCameraUv0;
    float4 xrPassthroughCameraUv1;
    float2 viewFieldOfView;
    float2 previousViewFieldOfView;
    float2 viewTanHalfFieldOfView;
    float2 previousViewTanHalfFieldOfView;
    float2 viewCotanHalfFieldOfView;
    float2 previousViewCotanHalfFieldOfView;
    MaterialDouble3 worldViewOrigin;
    MaterialDouble3 previousWorldViewOrigin;
    float temporalSampleCount;
    float temporalSampleIndex;
    float2 temporalSampleOffset;
    float runtimeVirtualTextureOutputLevel;
    float2 runtimeVirtualTextureOutputDerivative;
    float runtimeVirtualTextureMaxLevel;
    float preExposure;
    float oneOverPreExposure;
    float resolutionFraction;
    float resolutionFractionInverse;
    float postVolumeUserFlags;
    float2 firstPersonFieldOfView;
    float2 previousFirstPersonFieldOfView;
    float2 firstPersonTanHalfFieldOfView;
    float2 previousFirstPersonTanHalfFieldOfView;
    float2 firstPersonCotanHalfFieldOfView;
    float2 previousFirstPersonCotanHalfFieldOfView;
    float firstPersonScale;
    float previousFirstPersonScale;
    float nearPlane;
};

struct MaterialInputs {
    float4 svPosition;
    float4 interpolants[MATERIAL_MAX_INTERPOLANT_VECTORS];
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
    float3 translatedWorldPositionNoShaderOffsets;
    float3 localPositionNoShaderOffsets;
    float3 primitivePosition;
    float3 primitivePositionNoShaderOffsets;
    float3 objectTranslatedPosition;
    float3 actorTranslatedPosition;
    float unMirrored;
    float4 xrPassthroughCameraUv0;
    float4 xrPassthroughCameraUv1;
    float2 viewFieldOfView;
    float2 viewTanHalfFieldOfView;
    float2 viewCotanHalfFieldOfView;
    MaterialDouble3 worldViewOrigin;
    float temporalSampleCount;
    float temporalSampleIndex;
    float2 temporalSampleOffset;
    float runtimeVirtualTextureOutputLevel;
    float2 runtimeVirtualTextureOutputDerivative;
    float runtimeVirtualTextureMaxLevel;
    float preExposure;
    float oneOverPreExposure;
    float resolutionFraction;
    float resolutionFractionInverse;
    float postVolumeUserFlags;
    float2 firstPersonFieldOfView;
    float2 firstPersonTanHalfFieldOfView;
    float2 firstPersonCotanHalfFieldOfView;
    float firstPersonScale;
    float nearPlane;
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
#define MaterialContextScreenPosition(c) ((c).screenPosition)
#define MaterialContextViewFieldOfView(c) ((c).viewFieldOfView)
#define MaterialContextPreviousViewFieldOfView(c) ((c).previousViewFieldOfView)
#define MaterialContextViewTanHalfFieldOfView(c) ((c).viewTanHalfFieldOfView)
#define MaterialContextPreviousViewTanHalfFieldOfView(c) ((c).previousViewTanHalfFieldOfView)
#define MaterialContextViewCotanHalfFieldOfView(c) ((c).viewCotanHalfFieldOfView)
#define MaterialContextPreviousViewCotanHalfFieldOfView(c) ((c).previousViewCotanHalfFieldOfView)
#define MaterialContextWorldViewOrigin(c) ((c).worldViewOrigin)
#define MaterialContextPreviousWorldViewOrigin(c) ((c).previousWorldViewOrigin)
#define MaterialContextTemporalSampleCount(c) ((c).temporalSampleCount)
#define MaterialContextTemporalSampleIndex(c) ((c).temporalSampleIndex)
#define MaterialContextTemporalSampleOffset(c) ((c).temporalSampleOffset)
#define MaterialContextPreExposure(c) ((c).preExposure)
#define MaterialContextOneOverPreExposure(c) ((c).oneOverPreExposure)
#define MaterialContextResolutionFraction(c) ((c).resolutionFraction)
#define MaterialContextResolutionFractionInverse(c) ((c).resolutionFractionInverse)
#define MaterialContextPostVolumeUserFlags(c) ((c).postVolumeUserFlags)
#define MaterialContextFirstPersonFieldOfView(c) ((c).firstPersonFieldOfView)
#define MaterialContextPreviousFirstPersonFieldOfView(c) ((c).previousFirstPersonFieldOfView)
#define MaterialContextFirstPersonTanHalfFieldOfView(c) ((c).firstPersonTanHalfFieldOfView)
#define MaterialContextPreviousFirstPersonTanHalfFieldOfView(c) ((c).previousFirstPersonTanHalfFieldOfView)
#define MaterialContextFirstPersonCotanHalfFieldOfView(c) ((c).firstPersonCotanHalfFieldOfView)
#define MaterialContextPreviousFirstPersonCotanHalfFieldOfView(c) ((c).previousFirstPersonCotanHalfFieldOfView)
#define MaterialContextFirstPersonScale(c) ((c).firstPersonScale)
#define MaterialContextPreviousFirstPersonScale(c) ((c).previousFirstPersonScale)
#define MaterialContextNearPlane(c) ((c).nearPlane)
#define MaterialContextTransformTranslatedWorldPositionToCamera(c, a0) MaterialPassTransformTranslatedWorldPositionToCamera(c, a0)
#define MaterialContextPreviousTransformTranslatedWorldPositionToCamera(c, a0) MaterialPassPreviousTransformTranslatedWorldPositionToCamera(c, a0)
#define MaterialContextTransformTranslatedWorldVectorToCamera(c, a0) MaterialPassTransformTranslatedWorldVectorToCamera(c, a0)
#define MaterialContextPreviousTransformTranslatedWorldVectorToCamera(c, a0) MaterialPassPreviousTransformTranslatedWorldVectorToCamera(c, a0)
#define MaterialContextTransformTranslatedWorldPositionToView(c, a0) MaterialPassTransformTranslatedWorldPositionToView(c, a0)
#define MaterialContextPreviousTransformTranslatedWorldPositionToView(c, a0) MaterialPassPreviousTransformTranslatedWorldPositionToView(c, a0)
#define MaterialContextTransformTranslatedWorldVectorToView(c, a0) MaterialPassTransformTranslatedWorldVectorToView(c, a0)
#define MaterialContextPreviousTransformTranslatedWorldVectorToView(c, a0) MaterialPassPreviousTransformTranslatedWorldVectorToView(c, a0)
#define MaterialContextTransformCameraPositionToTranslatedWorld(c, a0) MaterialPassTransformCameraPositionToTranslatedWorld(c, a0)
#define MaterialContextPreviousTransformCameraPositionToTranslatedWorld(c, a0) MaterialPassPreviousTransformCameraPositionToTranslatedWorld(c, a0)
#define MaterialContextTransformCameraVectorToTranslatedWorld(c, a0) MaterialPassTransformCameraVectorToTranslatedWorld(c, a0)
#define MaterialContextPreviousTransformCameraVectorToTranslatedWorld(c, a0) MaterialPassPreviousTransformCameraVectorToTranslatedWorld(c, a0)
#define MaterialContextTransformViewPositionToTranslatedWorld(c, a0) MaterialPassTransformViewPositionToTranslatedWorld(c, a0)
#define MaterialContextPreviousTransformViewPositionToTranslatedWorld(c, a0) MaterialPassPreviousTransformViewPositionToTranslatedWorld(c, a0)
#define MaterialContextTransformViewVectorToTranslatedWorld(c, a0) MaterialPassTransformViewVectorToTranslatedWorld(c, a0)
#define MaterialContextPreviousTransformViewVectorToTranslatedWorld(c, a0) MaterialPassPreviousTransformViewVectorToTranslatedWorld(c, a0)
#define MaterialContextPeriodicWorldOrigin(c, a0) MaterialPassPeriodicWorldOrigin(c, a0)
#define MaterialContextPeriodicWorldOriginPow2(c, a0) MaterialPassPeriodicWorldOriginPow2(c, a0)
#define MaterialContextRenderPathDeferred(c) MaterialPassRenderPathDeferred(c)
#define MaterialContextRenderPathForwardPlus(c) MaterialPassRenderPathForwardPlus(c)
#define MaterialContextCapabilityTierSm5(c) MaterialPassCapabilityTierSm5(c)
#define MaterialContextCapabilityTierSm6(c) MaterialPassCapabilityTierSm6(c)
#define MaterialContextGlobalIlluminationPass(c) MaterialPassGlobalIlluminationPass(c)
#define MaterialContextRayTracingPass(c) MaterialPassRayTracingPass(c)
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
#define MaterialContextScreenPosition(c) float4(0.0, 0.0, 0.0, 0.0)
#define MaterialContextViewFieldOfView(c) float2(0.0, 0.0)
#define MaterialContextPreviousViewFieldOfView(c) float2(0.0, 0.0)
#define MaterialContextViewTanHalfFieldOfView(c) float2(0.0, 0.0)
#define MaterialContextPreviousViewTanHalfFieldOfView(c) float2(0.0, 0.0)
#define MaterialContextViewCotanHalfFieldOfView(c) float2(0.0, 0.0)
#define MaterialContextPreviousViewCotanHalfFieldOfView(c) float2(0.0, 0.0)
#define MaterialContextWorldViewOrigin(c) ((MaterialDouble3)0)
#define MaterialContextPreviousWorldViewOrigin(c) ((MaterialDouble3)0)
#define MaterialContextTemporalSampleCount(c) (0.0)
#define MaterialContextTemporalSampleIndex(c) (0.0)
#define MaterialContextTemporalSampleOffset(c) float2(0.0, 0.0)
#define MaterialContextPreExposure(c) (0.0)
#define MaterialContextOneOverPreExposure(c) (0.0)
#define MaterialContextResolutionFraction(c) (0.0)
#define MaterialContextResolutionFractionInverse(c) (0.0)
#define MaterialContextPostVolumeUserFlags(c) (0.0)
#define MaterialContextFirstPersonFieldOfView(c) float2(0.0, 0.0)
#define MaterialContextPreviousFirstPersonFieldOfView(c) float2(0.0, 0.0)
#define MaterialContextFirstPersonTanHalfFieldOfView(c) float2(0.0, 0.0)
#define MaterialContextPreviousFirstPersonTanHalfFieldOfView(c) float2(0.0, 0.0)
#define MaterialContextFirstPersonCotanHalfFieldOfView(c) float2(0.0, 0.0)
#define MaterialContextPreviousFirstPersonCotanHalfFieldOfView(c) float2(0.0, 0.0)
#define MaterialContextFirstPersonScale(c) (0.0)
#define MaterialContextPreviousFirstPersonScale(c) (0.0)
#define MaterialContextNearPlane(c) (0.0)
#define MaterialContextTransformTranslatedWorldPositionToCamera(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformTranslatedWorldPositionToCamera(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformTranslatedWorldVectorToCamera(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformTranslatedWorldVectorToCamera(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformTranslatedWorldPositionToView(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformTranslatedWorldPositionToView(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformTranslatedWorldVectorToView(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformTranslatedWorldVectorToView(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformCameraPositionToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformCameraPositionToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformCameraVectorToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformCameraVectorToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformViewPositionToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformViewPositionToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformViewVectorToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformViewVectorToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPeriodicWorldOrigin(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPeriodicWorldOriginPow2(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextRenderPathDeferred(c) (false)
#define MaterialContextRenderPathForwardPlus(c) (false)
#define MaterialContextCapabilityTierSm5(c) (false)
#define MaterialContextCapabilityTierSm6(c) (false)
#define MaterialContextGlobalIlluminationPass(c) (false)
#define MaterialContextRayTracingPass(c) (false)
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
#define MaterialContextTranslatedWorldPositionNoShaderOffsets(c) ((c).translatedWorldPositionNoShaderOffsets)
#define MaterialContextLocalPositionNoShaderOffsets(c) ((c).localPositionNoShaderOffsets)
#define MaterialContextPrimitivePosition(c) ((c).primitivePosition)
#define MaterialContextPreviousPrimitivePosition(c) ((c).previousPrimitivePosition)
#define MaterialContextPrimitivePositionNoShaderOffsets(c) ((c).primitivePositionNoShaderOffsets)
#define MaterialContextUnMirrored(c) ((c).unMirrored)
#define MaterialContextTransformTangentVectorToWorld(c, a0) MaterialPassTransformTangentVectorToWorld(c, a0)
#define MaterialContextTransformWorldVectorToTangent(c, a0) MaterialPassTransformWorldVectorToTangent(c, a0)
#else
#define MaterialContextTwoSidedSign(c) (0.0)
#define MaterialContextVertexColor(c) float4(0.0, 0.0, 0.0, 0.0)
#define MaterialContextVertexNormal(c) float3(0.0, 0.0, 1.0)
#define MaterialContextVertexTangent(c) float3(1.0, 0.0, 0.0)
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
#define MaterialContextTranslatedWorldPositionNoShaderOffsets(c) float3(0.0, 0.0, 0.0)
#define MaterialContextLocalPositionNoShaderOffsets(c) float3(0.0, 0.0, 0.0)
#define MaterialContextPrimitivePosition(c) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousPrimitivePosition(c) float3(0.0, 0.0, 0.0)
#define MaterialContextPrimitivePositionNoShaderOffsets(c) float3(0.0, 0.0, 0.0)
#define MaterialContextUnMirrored(c) (0.0)
#define MaterialContextTransformTangentVectorToWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformWorldVectorToTangent(c, a0) float3(0.0, 0.0, 0.0)
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
#define MaterialContextObjectTranslatedPosition(c) ((c).objectTranslatedPosition)
#define MaterialContextActorTranslatedPosition(c) ((c).actorTranslatedPosition)
#define MaterialContextPreviousActorTranslatedPosition(c) ((c).previousActorTranslatedPosition)
#define MaterialContextTransformLocalPositionToWorld(c, a0) MaterialPassTransformLocalPositionToWorld(c, a0)
#define MaterialContextPreviousTransformLocalPositionToWorld(c, a0) MaterialPassPreviousTransformLocalPositionToWorld(c, a0)
#define MaterialContextTransformLocalVectorToWorld(c, a0) MaterialPassTransformLocalVectorToWorld(c, a0)
#define MaterialContextPreviousTransformLocalVectorToWorld(c, a0) MaterialPassPreviousTransformLocalVectorToWorld(c, a0)
#define MaterialContextTransformLocalPositionToTranslatedWorld(c, a0) MaterialPassTransformLocalPositionToTranslatedWorld(c, a0)
#define MaterialContextPreviousTransformLocalPositionToTranslatedWorld(c, a0) MaterialPassPreviousTransformLocalPositionToTranslatedWorld(c, a0)
#define MaterialContextTransformTranslatedWorldPositionToLocal(c, a0) MaterialPassTransformTranslatedWorldPositionToLocal(c, a0)
#define MaterialContextPreviousTransformTranslatedWorldPositionToLocal(c, a0) MaterialPassPreviousTransformTranslatedWorldPositionToLocal(c, a0)
#define MaterialContextTransformTranslatedWorldVectorToLocal(c, a0) MaterialPassTransformTranslatedWorldVectorToLocal(c, a0)
#define MaterialContextPreviousTransformTranslatedWorldVectorToLocal(c, a0) MaterialPassPreviousTransformTranslatedWorldVectorToLocal(c, a0)
#define MaterialContextTransformTranslatedWorldPositionToInstance(c, a0) MaterialPassTransformTranslatedWorldPositionToInstance(c, a0)
#define MaterialContextPreviousTransformTranslatedWorldPositionToInstance(c, a0) MaterialPassPreviousTransformTranslatedWorldPositionToInstance(c, a0)
#define MaterialContextTransformTranslatedWorldVectorToInstance(c, a0) MaterialPassTransformTranslatedWorldVectorToInstance(c, a0)
#define MaterialContextTransformWorldPositionToLocal(c, a0) MaterialPassTransformWorldPositionToLocal(c, a0)
#define MaterialContextPreviousTransformWorldPositionToLocal(c, a0) MaterialPassPreviousTransformWorldPositionToLocal(c, a0)
#define MaterialContextTransformWorldVectorToLocal(c, a0) MaterialPassTransformWorldVectorToLocal(c, a0)
#define MaterialContextPreviousTransformWorldVectorToLocal(c, a0) MaterialPassPreviousTransformWorldVectorToLocal(c, a0)
#define MaterialContextTransformWorldPositionToInstance(c, a0) MaterialPassTransformWorldPositionToInstance(c, a0)
#define MaterialContextTransformWorldVectorToInstance(c, a0) MaterialPassTransformWorldVectorToInstance(c, a0)
#define MaterialContextTransformInstancePositionToWorld(c, a0) MaterialPassTransformInstancePositionToWorld(c, a0)
#define MaterialContextPreviousTransformInstancePositionToWorld(c, a0) MaterialPassPreviousTransformInstancePositionToWorld(c, a0)
#define MaterialContextTransformInstanceVectorToWorld(c, a0) MaterialPassTransformInstanceVectorToWorld(c, a0)
#define MaterialContextPreviousTransformInstanceVectorToWorld(c, a0) MaterialPassPreviousTransformInstanceVectorToWorld(c, a0)
#define MaterialContextTransformInstancePositionToTranslatedWorld(c, a0) MaterialPassTransformInstancePositionToTranslatedWorld(c, a0)
#define MaterialContextPreviousTransformInstancePositionToTranslatedWorld(c, a0) MaterialPassPreviousTransformInstancePositionToTranslatedWorld(c, a0)
#define MaterialContextTransformInstanceVectorToTranslatedWorld(c, a0) MaterialPassTransformInstanceVectorToTranslatedWorld(c, a0)
#define MaterialContextPreviousTransformInstanceVectorToTranslatedWorld(c, a0) MaterialPassPreviousTransformInstanceVectorToTranslatedWorld(c, a0)
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
#define MaterialContextObjectTranslatedPosition(c) float3(0.0, 0.0, 0.0)
#define MaterialContextActorTranslatedPosition(c) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousActorTranslatedPosition(c) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformLocalPositionToWorld(c, a0) ((MaterialDouble3)0)
#define MaterialContextPreviousTransformLocalPositionToWorld(c, a0) ((MaterialDouble3)0)
#define MaterialContextTransformLocalVectorToWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformLocalVectorToWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformLocalPositionToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformLocalPositionToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformTranslatedWorldPositionToLocal(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformTranslatedWorldPositionToLocal(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformTranslatedWorldVectorToLocal(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformTranslatedWorldVectorToLocal(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformTranslatedWorldPositionToInstance(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformTranslatedWorldPositionToInstance(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformTranslatedWorldVectorToInstance(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformWorldPositionToLocal(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformWorldPositionToLocal(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformWorldVectorToLocal(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformWorldVectorToLocal(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformWorldPositionToInstance(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformWorldVectorToInstance(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformInstancePositionToWorld(c, a0) ((MaterialDouble3)0)
#define MaterialContextPreviousTransformInstancePositionToWorld(c, a0) ((MaterialDouble3)0)
#define MaterialContextTransformInstanceVectorToWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformInstanceVectorToWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformInstancePositionToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformInstancePositionToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformInstanceVectorToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformInstanceVectorToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
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
#define MaterialContextTransformTranslatedWorldPositionToParticle(c, a0) MaterialPassTransformTranslatedWorldPositionToParticle(c, a0)
#define MaterialContextPreviousTransformTranslatedWorldPositionToParticle(c, a0) MaterialPassPreviousTransformTranslatedWorldPositionToParticle(c, a0)
#define MaterialContextTransformTranslatedWorldVectorToParticle(c, a0) MaterialPassTransformTranslatedWorldVectorToParticle(c, a0)
#define MaterialContextTransformWorldPositionToParticle(c, a0) MaterialPassTransformWorldPositionToParticle(c, a0)
#define MaterialContextTransformWorldVectorToParticle(c, a0) MaterialPassTransformWorldVectorToParticle(c, a0)
#define MaterialContextTransformParticlePositionToWorld(c, a0) MaterialPassTransformParticlePositionToWorld(c, a0)
#define MaterialContextTransformParticleVectorToWorld(c, a0) MaterialPassTransformParticleVectorToWorld(c, a0)
#define MaterialContextTransformParticlePositionToTranslatedWorld(c, a0) MaterialPassTransformParticlePositionToTranslatedWorld(c, a0)
#define MaterialContextPreviousTransformParticlePositionToTranslatedWorld(c, a0) MaterialPassPreviousTransformParticlePositionToTranslatedWorld(c, a0)
#define MaterialContextTransformParticleVectorToTranslatedWorld(c, a0) MaterialPassTransformParticleVectorToTranslatedWorld(c, a0)
#else
#define MaterialContextParticleColor(c) float4(1.0, 1.0, 1.0, 1.0)
#define MaterialContextParticleDirection(c) float3(0.0, 0.0, 0.0)
#define MaterialContextParticleMacroUv(c) float2(0.0, 0.0)
#define MaterialContextParticleMotionBlurFade(c) (1.0)
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
#define MaterialContextTransformTranslatedWorldPositionToParticle(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformTranslatedWorldPositionToParticle(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformTranslatedWorldVectorToParticle(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformWorldPositionToParticle(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformWorldVectorToParticle(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformParticlePositionToWorld(c, a0) ((MaterialDouble3)0)
#define MaterialContextTransformParticleVectorToWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformParticlePositionToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformParticlePositionToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
#define MaterialContextTransformParticleVectorToTranslatedWorld(c, a0) float3(0.0, 0.0, 0.0)
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

#if MATERIAL_SUPPLY_RUNTIME_VIRTUAL_TEXTURE
#define MaterialContextRuntimeVirtualTextureOutputLevel(c) ((c).runtimeVirtualTextureOutputLevel)
#define MaterialContextRuntimeVirtualTextureOutputDerivative(c) ((c).runtimeVirtualTextureOutputDerivative)
#define MaterialContextRuntimeVirtualTextureMaxLevel(c) ((c).runtimeVirtualTextureMaxLevel)
#else
#define MaterialContextRuntimeVirtualTextureOutputLevel(c) (0.0)
#define MaterialContextRuntimeVirtualTextureOutputDerivative(c) float2(0.0, 0.0)
#define MaterialContextRuntimeVirtualTextureMaxLevel(c) (0.0)
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
#define MaterialContextTransformToFirstPerson(c, a0, a1) MaterialPassTransformToFirstPerson(c, a0, a1)
#define MaterialContextPreviousTransformToFirstPerson(c, a0, a1) MaterialPassPreviousTransformToFirstPerson(c, a0, a1)
#else
#define MaterialContextIsFirstPerson(c) (0.0)
#define MaterialContextTransformToFirstPerson(c, a0, a1) float3(0.0, 0.0, 0.0)
#define MaterialContextPreviousTransformToFirstPerson(c, a0, a1) float3(0.0, 0.0, 0.0)
#endif

#if MATERIAL_SUPPLY_XR
#define MaterialContextXrPassthroughCameraUv0(c) ((c).xrPassthroughCameraUv0)
#define MaterialContextXrPassthroughCameraUv1(c) ((c).xrPassthroughCameraUv1)
#else
#define MaterialContextXrPassthroughCameraUv0(c) float4(0.0, 0.0, 0.0, 0.0)
#define MaterialContextXrPassthroughCameraUv1(c) float4(0.0, 0.0, 0.0, 0.0)
#endif

#if MATERIAL_SUPPLY_PATH_TRACING
#define MaterialContextPathTracingPass(c) MaterialPassPathTracingPass(c)
#define MaterialContextPathTracingShadowRay(c) MaterialPassPathTracingShadowRay(c)
#define MaterialContextPathTracingIndirectDiffuseRay(c) MaterialPassPathTracingIndirectDiffuseRay(c)
#define MaterialContextPathTracingIndirectSpecularRay(c) MaterialPassPathTracingIndirectSpecularRay(c)
#define MaterialContextPathTracingIndirectVolumeRay(c) MaterialPassPathTracingIndirectVolumeRay(c)
#else
#define MaterialContextPathTracingPass(c) (false)
#define MaterialContextPathTracingShadowRay(c) (false)
#define MaterialContextPathTracingIndirectDiffuseRay(c) (false)
#define MaterialContextPathTracingIndirectSpecularRay(c) (false)
#define MaterialContextPathTracingIndirectVolumeRay(c) (false)
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
