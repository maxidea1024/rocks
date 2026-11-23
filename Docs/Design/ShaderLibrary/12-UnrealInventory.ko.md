# 셰이더 라이브러리 — 12. UE 파일 전수 분류

[목차](README.ko.md)

## 12. UE 파일 전수 분류

UE 5.8.4 `Engine/Shaders`의 파일 1,227개 전부를 영역 하나에 넣은 결과다(미분류 0). 2026-11-23에 파일 목록을 열거하고 경로 규칙(첫 일치)으로 분류했다. 경로는 `Engine/Shaders/` 기준이며, 같은 폴더의 파일은 한 줄에 모았다. 영역 내용은 4장, 제외 사유는 1.4절이다. 파일 이름만 적는다(내용은 옮기지 않는다).

### A01 플랫폼·공통 매크로 (26)

- `Private/`: `AssertionMacros.ush`, `Common.ush`, `ComputeShaderOutputCommon.ush`, `ComputeShaderUtils.ush`, `Definitions.usf`, `GeneratedUniformBufferTypes.ush`, `PixelShaderOutputCommon.ush`, `ShaderOutputCommon.ush`, `WaveOpUtil.ush`
- `Public/`: `DualPixelVectorization.ush`, `FP16Math.ush`, `LaneVectorization.ush`, `OverloadMacros.ush`, `Platform.ush`, `RootConstants.ush`, `ShaderVersion.ush`, `WaveBroadcastIntrinsics.ush`
- `Public/Platform/D3D/`: `D3DCommon.ush`, `D3DRootConstants.ush`
- `Public/Platform/GL/`: `GLCommon.ush`
- `Public/Platform/Metal/`: `MetalCommon.ush`
- `Public/Platform/Vulkan/`: `VulkanCommon.ush`
- `Shared/`: `HLSLMathAliases.h`, `HLSLReservedSpaces.h`, `HLSLStaticAssert.h`, `HLSLTypeAliases.h`

### A02 수학·인코딩·패킹·난수·표본 (35)

- `Private/`: `Barycentrics.ush`, `BitPacking.ush`, `BlueNoise.ush`, `BlueNoiseCommon.ush`, `DoubleFloat.ush`, `DoubleFloatOperations.ush`, `DoubleFloatVectorDefinition.ush`, `DualNumber.ush`, `FastMath.ush`, `FastMathThirdParty.tps`, `FastMathThirdParty.ush`, `HaltonUtilities.ush`, `Hash.ush`, `IntersectionUtils.ush`, `LWCOperations.ush`, `LargeWorldCoordinates.ush`, `LeonhardGExampleSamplers.tps`, `Matrices.ush`, `MonteCarlo.ush`, `MortonCode.ush`, `OctahedralCommon.ush`, `PackUnpack.ush`, `Quantization.ush`, `Quaternion.ush`, `Random.ush`, `Random1.tps`, `Random2.tps`, `Random3.tps`, `Random4.tps`, `RandomInterleavedGradientNoise.ush`, `RandomPCG.ush`, `SHCommon.ush`, `SobolRandom.ush`, `SphericalGaussian.ush`, `WorldSpaceMath.ush`

### A03 색·전달 함수·톤 곡선 (17)

- `Private/ACES/`: `ACESCommon.ush`, `ACESDisplayEncoding.ush`, `ACESOutputTransform.ush`, `ACESTonescale.ush`, `ACESUtilities.ush`, `ACES_v1.3.tps`, `ACES_v1.3.ush`, `Academy Color Encoding System Core Transforms_v2.0.tps`
- `Private/`: `ColorDeficiency.ush`, `ColorDifference.ush`, `ColorMap.tps`, `ColorMap.ush`, `ColorSpace.ush`, `ColorUtils.ush`, `GammaCorrectionCommon.ush`, `ModelforSimofColorVisionDeficiency.tps`, `TonemapCommon.ush`

### A04 BRDF·셰이딩 모델·광원 적분 (21)

- `Private/`: `AreaLightCommon.ush`, `BRDF.ush`, `CapsuleLight.ush`, `CapsuleLightIntegrate.ush`, `CapsuleLightSampling.ush`, `ClearCoatCommon.ush`, `LightAccumulator.ush`, `MobileGGX.tps`, `RectLight.ush`, `RectLightIntegrate.ush`, `SGGX.ush`, `ShadingCommon.ush`, `ShadingEnergyConservation.ush`, `ShadingEnergyConservationTable.usf`, `ShadingEnergyConservationTemplate.ush`, `ShadingFurnaceTest.usf`, `ShadingModels.ush`, `ShadingModelsMaterial.ush`, `ShadingModelsSampling.ush`, `ThinFilmBSDF.ush`, `TransmissionCommon.ush`

### A05 머티리얼 템플릿·정점 입력 (13)

- `Private/`: `DynamicMeshBounds.usf`, `DynamicMeshBounds.ush`, `LocalVertexFactory.ush`, `LocalVertexFactoryCommon.ush`, `MaterialTemplate.ush`, `MaterialTexture.ush`, `MinMaterialTexCoords.ush`, `SplineMeshCommon.ush`, `SplineMeshSceneTexture.usf`, `VertexFactoryCommon.ush`, `VertexFactoryDefaultFwd.ush`, `VertexFactoryDefaultInterface.ush`
- `Shared/`: `SplineMeshShaderParams.h`

### A06 장면 데이터·view·컬링·HZB (31)

- `Private/`: `CommonViewUniformBuffer.ush`, `DitheredTransitionStencil.usf`, `GenerateConservativeDepth.usf`, `HZB.usf`, `HZB.ush`, `HZBOcclusion.usf`, `HZBTracing.ush`, `OcclusionQueryPixelShader.usf`, `OcclusionQueryVertexShader.usf`, `PositionReconstructionCommon.ush`, `SceneData.ush`, `SceneTextureParameters.ush`, `SceneTexturesCommon.ush`, `ViewData.ush`
- `Private/GPUScene/`: `GPUSceneDataManagement.usf`, `GPUSceneWriter.ush`
- `Private/InstanceCulling/`: `BuildInstanceDrawCommands.usf`, `CompactVisibleInstances.usf`, `InstanceCompactionCommon.ush`, `InstanceCullingCommon.ush`, `InstanceCullingLoadBalancer.ush`, `InstanceCullingOcclusionQuery.usf`, `InstanceCullingSetup.ush`
- `Private/SceneCulling/`: `SceneCulling.ush`, `SceneCullingBuildExplicitBounds.usf`, `SceneCullingDebugRender.usf`
- `Shared/`: `EnvironmentComponentsFlags.h`, `InstanceCullingDefinitions.h`, `SceneCullingDefinitions.h`, `SceneDefinitions.h`, `ViewDefinitions.h`

### A07 깊이·velocity·베이스 패스·GBuffer (15)

- `Private/`: `AnisotropyPassShader.usf`, `BasePassCommon.ush`, `BasePassCommonFunctions.ush`, `BasePassPixelShader.usf`, `BasePassVertexCommon.ush`, `BasePassVertexShader.usf`, `DeferredShadingCommon.ush`, `DepthOnlyPixelShader.usf`, `DepthOnlyVertexShader.usf`, `FirstPersonSelfShadow.usf`, `GBufferHelpers.ush`, `PositionOnlyDepthVertexShader.usf`, `VelocityCommon.ush`, `VelocityShader.usf`, `VelocityUpdate.usf`

### A08 광원 데이터·light grid·광원 함수 (17)

- `Private/`: `IESAtlas.usf`, `IESLightProfilesCommon.ush`, `LightData.ush`, `LightDataUniforms.ush`, `LightFunctionCommon.ush`, `LightFunctionPixelShader.usf`, `LightFunctionVertexShader.usf`, `LightGridCommon.ush`, `LightGridInjection.usf`, `LightShaderParameters.ush`, `RectLightAtlas.usf`
- `Private/LightFunctionAtlas/`: `LightFunctionAtlasCommon.usf`, `LightFunctionAtlasRender.usf`
- `Shared/`: `LightDefinitions.h`, `LightGridDefinitions.h`, `LightSceneData.h`, `LightViewData.h`

### A09 직접 조명·Deferred 조명 (5)

- `Private/`: `DeferredLightPixelShaders.usf`, `DeferredLightVertexShaders.usf`, `DeferredLightingCommon.ush`, `DynamicLightingCommon.ush`, `ForwardLightingCommon.ush`

### A10 그림자 (16)

- `Private/`: `CopyShadowMaps.usf`, `ForwardShadowingCommon.ush`, `ScreenSpaceShadowRayCast.ush`, `ScreenSpaceShadows.usf`, `ShadowDepthCommon.ush`, `ShadowDepthPixelShader.usf`, `ShadowDepthVertexShader.usf`, `ShadowFactorsUpsampleCommon.ush`, `ShadowFilteringCommon.ush`, `ShadowPercentageCloserFiltering.ush`, `ShadowProjectionCommon.ush`, `ShadowProjectionPixelShader.usf`, `ShadowProjectionVertexShader.usf`, `bend_sss_gpu.tps`, `bend_sss_gpu.ush`, `bend_sss_gpu_edit.tps`

### A11 반사·sky light·캡처·SSR (19)

- `Private/`: `AmbientCubemapComposite.usf`, `CubemapCommon.ush`, `DefaultSSRTiles.usf`, `DiffuseIndirectComposite.usf`, `PlanarReflectionShaders.usf`, `PlanarReflectionShared.ush`, `ReflectionEnvironmentComposite.ush`, `ReflectionEnvironmentPixelShader.usf`, `ReflectionEnvironmentShaders.usf`, `ReflectionEnvironmentShared.ush`, `SceneCapturePixelShader.usf`, `ScreenSpaceReflectionTileCommons.ush`, `SkyLightingDiffuseShared.ush`, `SkyLightingShared.ush`
- `Private/SSRT/`: `SSRTPrevFrameReduction.usf`, `SSRTRayCast.ush`, `SSRTReflections.usf`, `SSRTTileClassification.usf`, `SSRTTileClassificationBuffer.ush`

### A12 AO (2)

- `Private/`: `PostProcessAmbientOcclusion.usf`, `PostProcessAmbientOcclusionCommon.ush`

### A13 데칼 (5)

- `Private/`: `DBufferDecalShared.ush`, `DBufferNormalReprojection.ush`, `DecalCommon.ush`, `DeferredDecal.usf`, `MeshDecals.usf`

### A14 정적 조명 표본 (4)

- `Private/`: `LightmapCommon.ush`, `LightmapData.ush`, `VolumetricLightmapShared.ush`, `VolumetricLightmapStreaming.usf`

### A15 반투명·굴절 (8)

- `Private/`: `ComposeSeparateTranslucency.usf`, `DistortAccumulatePS.usf`, `DistortAccumulateVS.usf`, `DistortApplyScreenPS.usf`, `DistortFiltering.usf`, `DistortionCommon.ush`, `SeparateTranslucency.ush`, `TranslucencyUpsampling.usf`

### A16 하늘 대기·높이 포그 (6)

- `Private/`: `HeightFogCommon.ush`, `HeightFogPixelShader.usf`, `HeightFogVertexShader.usf`, `ParticipatingMediaCommon.ush`, `SkyAtmosphere.usf`, `SkyAtmosphereCommon.ush`

### A17 포스트 프로세스 기본 (28)

- `Private/`: `DisplayMappingPixelShader.usf`, `EyeAdaptationCommon.ush`, `FXAAShader.usf`, `FXAAsource.tps`, `Fxaa3_11.ush`, `Histogram.usf`, `PostProcessBloom.usf`, `PostProcessCombineLUTs.usf`, `PostProcessCombineLUTsInner.usf`, `PostProcessCommon.ush`, `PostProcessDeviceEncodingOnly.usf`, `PostProcessDownsample.usf`, `PostProcessExposureMaterial.ush`, `PostProcessEyeAdaptation.usf`, `PostProcessEyeAdaptationUtils.usf`, `PostProcessHistogram.usf`, `PostProcessHistogramCommon.ush`, `PostProcessHistogramReduce.usf`, `PostProcessMaterialShaders.usf`, `PostProcessMitchellNetravali.usf`, `PostProcessTonemap.usf`, `PostProcessUpscale.usf`, `ScreenPass.usf`, `ScreenPass.ush`, `TemporalAA.tps`, `TemporalAA.usf`
- `Private/PostProcessing/`: `FilmGrainPackConstants.usf`, `FilmGrainReduce.usf`

### A18 유틸리티 (37)

- `Private/`: `ByteBuffer.usf`, `ByteBuffer.ush`, `ClearReplacementShaders.usf`, `ComputeGenerateMips.usf`, `CopyDepthTexture.usf`, `CopyDepthTextureCS.usf`, `CopyTextureShaders.usf`, `CopyUIntBuffer.usf`, `DownsampleDepthPixelShader.usf`, `FilterPixelShader.usf`, `FilterVertexShader.usf`, `HdrCustomResolveShaders.usf`, `MissingShaderPixelShader.usf`, `NullPixelShader.usf`, `OneColorShader.usf`, `PostProcessAlphaInvert.usf`, `PostProcessChannelMask.usf`, `PostProcessPassThrough.usf`, `RenderGraphUtilities.usf`, `RendererUtils.usf`, `ResolvePixelShader.usf`, `ResolveVertexShader.usf`, `ScreenPixelShader.usf`, `ScreenPixelShaderOES.usf`, `ScreenVertexShader.usf`, `SolidColorPixelShader.tps`, `SolidColorPixelShader.ush`, `TextureSampling.ush`, `UpdateTextureShaders.usf`, `WideCustomResolveShaders.usf`, `WideCustomResolve_Wide.ush`, `WideCustomResolve_Wider.ush`, `WideCustomResolve_Widest.ush`
- `Private/Tools/`: `DrawTexture.usf`, `DumpTexture.usf`, `FullscreenVertexShader.usf`, `SetupIndirectArgs.usf`

### A19 2D·UI·단순 요소 (13)

- `Private/`: `CompositeUIPixelShader.usf`, `SimpleElementColorChannelMaskPixelShader.usf`, `SimpleElementHitProxyPixelShader.usf`, `SimpleElementPixelShader.usf`, `SimpleElementTexture2DPreviewPixelShader.usf`, `SimpleElementVertexShader.usf`, `SimpleElementVolumeTexturePreviewPixelShader.usf`, `SlateElementPixelShader.usf`, `SlateMaskingShader.usf`, `SlatePostProcessColorDeficiencyPixelShader.usf`, `SlatePostProcessPixelShader.usf`, `SlateShaderCommon.ush`, `SlateVertexShader.usf`

### A20 디버그·시각화·편집 표시 (47)

- `Private/`: `DebugProbes.usf`, `DebugViewModeCommon.ush`, `DebugViewModePixelShader.usf`, `DebugViewModeVertexShader.usf`, `GPUMessaging.usf`, `GPUMessaging.ush`, `GPUSceneDebugRender.usf`, `HitProxyCopy.usf`, `HitProxyPixelShader.usf`, `HitProxyVertexShader.usf`, `LightMapDensityShader.usf`, `MeshEdges.usf`, `MeshPaintPixelShader.usf`, `MeshPaintTextureCommon.ush`, `MeshPaintVertexShader.usf`, `MiniFontCommon.ush`, `PostProcessCompositePrimitives.usf`, `PostProcessGBufferHints.usf`, `PostProcessSelectionOutline.usf`, `PostProcessTestImage.usf`, `PostProcessVisualizeBuffer.usf`, `PostProcessVisualizeDOF.usf`, `PostProcessVisualizeHDR.usf`, `PostProcessVisualizeLevelInstance.usf`, `PostProcessVisualizeLocalExposure.usf`, `PrintValue.tps`, `PrintValue.ush`, `QuadComplexityAccumulatePixelShader.usf`, `QuadOverdraw.ush`, `ShaderComplexityApplyPixelShader.usf`, `ShaderPrint.ush`, `ShaderPrintCommon.ush`, `ShaderPrintDraw.usf`, `ShaderPrintDrawPrimitive.usf`, `StationaryLightOverlapShaders.usf`, `Visualization.ush`, `VisualizeShadingModels.usf`, `VisualizeVolumetricLightmap.usf`, `meshpaintdilatepixelshader.usf`, `meshpaintdilatevertexshader.usf`
- `Private/Tools/`: `ColorGradingHistogram.usf`, `ColorGradingParade.usf`, `ColorGradingVectorScope.usf`, `DebugAlphaChannel.usf`, `GPUDebugCrashUtils.usf`, `VisualizeLightShape.usf`, `VisualizeTexture.usf`

### A21 컴퓨트 유틸 (15)

- `Private/`: `ASTCCompressionCommon.ush`, `BCCompressionCommon.ush`, `BinarySearch.ush`, `BitStreamReaderImplementation.ush`, `BitonicSort.usf`, `BlockCompressionCommon.ush`, `ETCCompressionCommon.ush`, `HashTable.ush`, `IntegerSequenceEncoding.ush`, `PixelQuadMessagePassing.ush`, `RadixSortShaders.usf`, `ReductionCommon.ush`, `ThreadGroupPrefixSum.ush`, `WorkDistribution.ush`, `WorkGroupLoadBalancer.ush`

### A22 스키닝·정점 변형 (17)

- `Private/`: `BoneTransform.ush`, `GpuSkinCacheComputeShader.usf`, `GpuSkinCommon.ush`, `GpuSkinVertexFactory.ush`, `MorphTargets.usf`, `RecomputeTangentsCommon.ush`, `RecomputeTangentsPerTrianglePass.usf`, `RecomputeTangentsPerVertexPass.usf`
- `Private/Skinning/`: `AnimBankEval.usf`, `AnimSequenceTransformProvider.usf`, `CopyPreviousTransforms.usf`, `ResolveAttachments.usf`, `SkinnedMeshDebug.usf`, `SkinningDataDecode.ush`, `SkinningSceneDataDecode.ush`, `TransformProviders.usf`
- `Shared/`: `SkinningDefinitions.h`

### A23 고급 셰이딩 모델 (19)

- `Private/`: `BurleyNormalizedSSSCommon.ush`, `PostProcessSubsurface.usf`, `PostProcessSubsurfaceCommon.ush`, `PostProcessSubsurfaceDiffuse.usf`, `PostProcessSubsurfaceDiffuse.ush`, `PostProcessSubsurfaceTile.usf`, `SeparableSSS.tps`, `SeparableSSS.ush`, `SingleLayerWaterCommon.ush`, `SingleLayerWaterComposite.usf`, `SingleLayerWaterRefractionCulling.usf`, `SingleLayerWaterShading.ush`, `SpecularProfile.usf`, `SpecularProfileCommon.ush`, `SubsurfaceBurleyNormalized.ush`, `SubsurfaceProfileCommon.ush`, `ThinTranslucentCommon.ush`, `TransmissionThickness.ush`
- `Shared/`: `SingleLayerWaterDefinitions.h`

### A24 포스트 프로세스 확장(후속) (85)

- `Private/Bloom/`: `BloomClampKernel.usf`, `BloomCommon.ush`, `BloomDownsampleKernel.usf`, `BloomFinalizeApplyConstants.usf`, `BloomFindKernelCenter.usf`, `BloomPackKernelConstants.usf`, `BloomReduceKernelSurvey.usf`, `BloomResizeKernel.usf`, `BloomSumScatterDispersionEnergy.usf`, `BloomSurveyKernelCenterEnergy.usf`, `BloomSurveyMaxScatterDispersion.usf`
- `Private/`: `CircleDOFCommon.ush`, `DepthOfFieldCommon.ush`, `GPUFastFourierTransform.usf`, `GPUFastFourierTransform2DCore.ush`, `GPUFastFourierTransformCore.ush`, `LensDistortion.ush`, `PaniniProjection.usf`, `PaniniProjection.ush`, `PostProcessDOF.usf`, `PostProcessLensFlares.usf`, `PostProcessLocalExposure.usf`, `PostProcessNoiseBlur.usf`
- `Private/DiaphragmDOF/`: `DOFBokehLUT.usf`, `DOFCocTileCommon.ush`, `DOFCocTileDilate.usf`, `DOFCocTileFlatten.usf`, `DOFCommon.ush`, `DOFDownsample.usf`, `DOFDownsample.ush`, `DOFGatherAccumulator.ush`, `DOFGatherCommon.ush`, `DOFGatherKernel.ush`, `DOFGatherPass.usf`, `DOFGatherTileSuggest.ush`, `DOFHybridScatterCommon.ush`, `DOFHybridScatterCompilation.usf`, `DOFHybridScatterCompilation.ush`, `DOFHybridScatterPixelShader.usf`, `DOFHybridScatterVertexShader.usf`, `DOFPostfiltering.usf`, `DOFRecombine.usf`, `DOFReduce.usf`, `DOFSetup.usf`, `DOFVignetteCommon.ush`
- `Private/MotionBlur/`: `MotionBlurApply.tps`, `MotionBlurApply.usf`, `MotionBlurCommon.ush`, `MotionBlurFilterTileClassify.usf`, `MotionBlurTileGather.usf`, `MotionBlurTileScatter.usf`, `MotionBlurVelocityFlatten.usf`, `MotionBlurVelocityFlatten.ush`, `MotionBlurVisualize.usf`, `VisualizeMotionVectors.usf`
- `Private/SMAA/`: `SMAA.ush`, `SMAABlendingWeightCalculationShader.usf`, `SMAACommon.ush`, `SMAADebugVisualization.usf`, `SMAAEdgeDetectionShader.usf`, `SMAANeighborhoodBlendingShader.usf`, `SubpixelMorphologicalAntialiasing.tps`
- `Private/TemporalSuperResolution/`: `TSRClearPrevTextures.usf`, `TSRClosestOccluder.ush`, `TSRColorSpace.ush`, `TSRCommon.ush`, `TSRConvolutionNetwork.ush`, `TSRConvolutionNetworkPass.ush`, `TSRDecimateHistory.usf`, `TSRDepthVelocityAnalysis.ush`, `TSRDetectThinGeometry.usf`, `TSRDilateVelocity.usf`, `TSRKernels.ush`, `TSRMeasureCoverage.usf`, `TSRMeasureFlickeringLuma.usf`, `TSRRejectShading.usf`, `TSRReprojectionField.ush`, `TSRResolveHistory.usf`, `TSRShadingAnalysis.ush`, `TSRSpatialAntiAliasing.usf`, `TSRSpatialAntiAliasing.ush`, `TSRThinGeometryCommon.ush`, `TSRUpdateHistory.usf`, `TSRVisualize.usf`, `TSRWeightRelaxation.usf`

### A25 볼륨 렌더링(후속) (60)

- `Private/`: `FogScreenSpaceScattering.ush`, `FogSeparateComposition.usf`, `FogSeparateComposition.ush`, `LightShaftShader.usf`, `TranslucencyVolumeCommon.ush`, `TranslucencyVolumeInjectionCommon.ush`, `TranslucencyVolumeVisualize.usf`, `TranslucentLightInjectionShaders.usf`, `TranslucentLightingShaders.usf`, `TranslucentShadowDepthShaders.usf`, `VolumeLightingCommon.ush`, `VolumeLightingCommonSampling.ush`, `VolumetricCloud.usf`, `VolumetricCloudCommon.ush`, `VolumetricCloudMaterialPixelCommon.ush`, `VolumetricFog.usf`, `VolumetricFogLightFunction.usf`, `VolumetricFogVoxelization.usf`, `VolumetricRenderTarget.usf`
- `Private/Froxel/`: `Froxel.ush`, `FroxelBuild.ush`
- `Private/HeterogeneousVolumes/`: `HeterogeneousVolumesAdaptiveVolumetricShadowMapSampler.ush`, `HeterogeneousVolumesAdaptiveVolumetricShadowMapSampling.ush`, `HeterogeneousVolumesAdaptiveVolumetricShadowMapUtils.ush`, `HeterogeneousVolumesAmbientOcclusionPipeline.usf`, `HeterogeneousVolumesBeerShadowMapUtils.ush`, `HeterogeneousVolumesComposite.usf`, `HeterogeneousVolumesFrustumVoxelGrid.usf`, `HeterogeneousVolumesFrustumVoxelGridUtils.ush`, `HeterogeneousVolumesHardwareRayTracing.usf`, `HeterogeneousVolumesLightingUtils.ush`, `HeterogeneousVolumesLiveShadingGlobalPipeline.usf`, `HeterogeneousVolumesLiveShadingPipeline.usf`, `HeterogeneousVolumesLiveShadingShadows.usf`, `HeterogeneousVolumesLiveShadingUtils.ush`, `HeterogeneousVolumesMaterialBakingPipeline.usf`, `HeterogeneousVolumesPreshadingPipeline.usf`, `HeterogeneousVolumesPreshadingUniformBufferUtils.ush`, `HeterogeneousVolumesPreshadingUtils.ush`, `HeterogeneousVolumesRayMarchingTypes.ush`, `HeterogeneousVolumesRayMarchingUtils.ush`, `HeterogeneousVolumesShadowMask.usf`, `HeterogeneousVolumesSparseVoxelPipeline.usf`, `HeterogeneousVolumesSparseVoxelUniformBufferUtils.ush`, `HeterogeneousVolumesSparseVoxelUtils.ush`, `HeterogeneousVolumesStochasticFiltering.ush`, `HeterogeneousVolumesTracingUtils.ush`, `HeterogeneousVolumesTransmittanceVolumeUtils.ush`, `HeterogeneousVolumesVisualizeCascades.usf`, `HeterogeneousVolumesVoxelGridPipeline.usf`, `HeterogeneousVolumesVoxelGridRendering.usf`, `HeterogeneousVolumesVoxelGridRenderingUtils.ush`, `HeterogeneousVolumesVoxelGridShadows.usf`, `HeterogeneousVolumesVoxelGridTraversal.ush`, `HeterogeneousVolumesVoxelGridTypes.ush`, `HeterogeneousVolumesVoxelGridUtils.ush`
- `Private/LocalFogVolumes/`: `LocalFogVolumeCommon.ush`, `LocalFogVolumeSplat.usf`, `LocalFogVolumeTiledCulling.usf`
- `Shared/`: `FroxelDefinitions.h`

### A26 Nanite(후속) (60)

- `Private/Nanite/`: `NaniteAttributeDecode.ush`, `NaniteClusterCulling.usf`, `NaniteClusterRayTracing.usf`, `NaniteClusterRayTracingStats.ush`, `NaniteCulling.ush`, `NaniteCullingCommon.ush`, `NaniteCurve.usf`, `NaniteCurveCommon.ush`, `NaniteDataDecode.ush`, `NaniteDebugViews.usf`, `NaniteDepthDecode.usf`, `NaniteDepthExport.usf`, `NaniteDice.ush`, `NaniteEditorCommon.ush`, `NaniteEmitShadow.usf`, `NaniteExportGBuffer.usf`, `NaniteFastClear.usf`, `NaniteHZBCull.ush`, `NaniteHierarchyTraversal.ush`, `NaniteHierarchyTraversalCommon.ush`, `NaniteInstanceCulling.usf`, `NaniteInstanceHierarchyCulling.usf`, `NanitePackedNaniteView.ush`, `NanitePrimitiveFilter.usf`, `NanitePrintStats.usf`, `NaniteRasterBinning.usf`, `NaniteRasterClear.usf`, `NaniteRasterizationCommon.ush`, `NaniteRasterizer.usf`, `NaniteRasterizer.ush`, `NaniteRayTrace.ush`, `NaniteRayTracing.usf`, `NaniteSVO.ush`, `NaniteScatterUpdates.usf`, `NaniteSceneCommon.ush`, `NaniteShadeBinning.usf`, `NaniteShadeCommon.ush`, `NaniteShadeDebug.usf`, `NaniteSkinningUpdateViewData.usf`, `NaniteSplit.usf`, `NaniteStreamOut.usf`, `NaniteStreaming.usf`, `NaniteStreaming.ush`, `NaniteTessellation.ush`, `NaniteTranscode.usf`, `NaniteTranslucency.usf`, `NaniteTranslucencyFactory.ush`, `NaniteVertexDeformation.ush`, `NaniteVertexFactory.ush`, `NaniteVertexFetch.ush`, `NaniteVisualize.usf`, `NaniteWritePixel.ush`
- `Private/Nanite/Voxel/`: `AutoVoxel.usf`, `Brick.ush`, `RasterizeBricks.usf`, `ScatterBricks.usf`, `TileBricks.usf`, `Voxel.ush`
- `Shared/`: `NaniteDefinitions.h`, `NaniteRayTracingDefinitions.h`

### A27 Lumen·거리장(후속) (127)

- `Private/`: `CapsuleShadowShaders.usf`, `DistanceFieldAOShared.ush`, `DistanceFieldDownsampling.usf`, `DistanceFieldLightingPost.usf`, `DistanceFieldLightingShared.ush`, `DistanceFieldObjectCulling.usf`, `DistanceFieldScreenGridLighting.usf`, `DistanceFieldShadowing.usf`, `DistanceFieldShadowingShared.ush`, `DistanceFieldStreaming.usf`, `DistanceFieldVisualization.usf`, `FrontLayerTranslucency.usf`, `FrontLayerTranslucencyShared.ush`, `HeightFieldAtlasManagement.usf`, `HeightfieldLightingShared.ush`, `HybridIndirectLighting.ush`, `MeshDistanceFieldCommon.ush`
- `Private/DistanceField/`: `GlobalDistanceField.usf`, `GlobalDistanceFieldCompositeObjects.usf`, `GlobalDistanceFieldDebug.usf`, `GlobalDistanceFieldHeightfields.usf`, `GlobalDistanceFieldMip.usf`, `GlobalDistanceFieldObjectGrid.ush`, `GlobalDistanceFieldShared.ush`, `GlobalDistanceFieldUpdate.ush`, `GlobalDistanceFieldUtils.ush`
- `Private/Lumen/`: `LumenBufferEncoding.ush`, `LumenCardBasePass.ush`, `LumenCardCommon.ush`, `LumenCardComputeShader.usf`, `LumenCardPixelShader.usf`, `LumenCardTile.ush`, `LumenCardTileShadowDownsampleFactor.ush`, `LumenCardVertexShader.usf`, `LumenCompositeBackgroundVisibility.usf`, `LumenDiffuseColorBoost.ush`, `LumenFloatQuantization.ush`, `LumenHairTracing.ush`, `LumenHardwareRayTracingCommon.ush`, `LumenHardwareRayTracingMaterials.usf`, `LumenHardwareRayTracingPayloadCommon.ush`, `LumenHardwareRayTracingPlatformCommon.ush`, `LumenIrradianceFieldGather.usf`, `LumenIrradianceFieldInterpolation.ush`, `LumenMaterial.ush`, `LumenMeshSDFCulling.usf`, `LumenPosition.ush`, `LumenRadianceCache.usf`, `LumenRadianceCacheCommon.ush`, `LumenRadianceCacheDebug.usf`, `LumenRadianceCacheHardwareRayTracing.usf`, `LumenRadianceCacheInterpolation.ush`, `LumenRadianceCacheMarkCommon.ush`, `LumenRadianceCacheTracingCommon.ush`, `LumenRadianceCacheUpdate.usf`, `LumenRadianceCacheUpdate.ush`, `LumenReSTIRGather.usf`, `LumenReflectionCommon.ush`, `LumenReflectionDenoiserClear.usf`, `LumenReflectionDenoiserCommon.ush`, `LumenReflectionDenoiserSpatial.usf`, `LumenReflectionDenoiserTemporal.usf`, `LumenReflectionHardwareRayTracing.usf`, `LumenReflectionResolve.usf`, `LumenReflectionTracing.usf`, `LumenReflections.usf`, `LumenReflectionsCombine.ush`, `LumenScene.usf`, `LumenSceneDebugHardwareRayTracing.usf`, `LumenSceneDirectLighting.usf`, `LumenSceneDirectLighting.ush`, `LumenSceneDirectLightingCulling.usf`, `LumenSceneDirectLightingHardwareRayTracing.usf`, `LumenSceneDirectLightingPerLightShadowCommon.ush`, `LumenSceneDirectLightingShadowMask.usf`, `LumenSceneDirectLightingSoftwareRayTracing.usf`, `LumenSceneDirectLightingStochastic.usf`, `LumenSceneDirectLightingStochastic.ush`, `LumenSceneLighting.usf`, `LumenSceneLighting.ush`, `LumenSceneLightingDebug.usf`, `LumenScreenProbeCommon.ush`, `LumenScreenProbeDebug.usf`, `LumenScreenProbeFiltering.usf`, `LumenScreenProbeGather.usf`, `LumenScreenProbeGatherScreenData.usf`, `LumenScreenProbeGatherTemporal.usf`, `LumenScreenProbeGatherTemporal.ush`, `LumenScreenProbeHardwareRayTracing.usf`, `LumenScreenProbeImportanceSampling.usf`, `LumenScreenProbeImportanceSamplingShared.ush`, `LumenScreenProbeTileClassication.ush`, `LumenScreenProbeTracing.usf`, `LumenScreenProbeTracingCommon.ush`, `LumenScreenSpaceBentNormal.usf`, `LumenScreenSpaceBentNormal.ush`, `LumenScreenTracing.ush`, `LumenShortRangeAOHardwareRayTracing.usf`, `LumenSoftwareRayTracing.ush`, `LumenTracingCommon.ush`, `LumenTranslucencyRadianceCacheMarkShaders.usf`, `LumenTranslucencyVolumeHardwareRayTracing.usf`, `LumenTranslucencyVolumeLighting.usf`, `LumenTranslucencyVolumeLightingShared.ush`, `LumenTranslucencyVolumeShared.ush`, `LumenUpsample.ush`, `LumenVirtualTextureCommon.ush`, `LumenVisualize.usf`, `LumenVisualize.ush`, `LumenVisualizeHardwareRayTracing.usf`, `LumenVisualizeRadianceCache.usf`, `LumenVisualizeTraces.ush`, `RayTracedTranslucency.usf`
- `Private/Lumen/Radiosity/`: `LumenRadiosity.usf`, `LumenRadiosity.ush`, `LumenRadiosityCulling.usf`, `LumenRadiosityHardwareRayTracing.usf`, `LumenVisualizeRadiosityProbes.usf`
- `Private/Lumen/SurfaceCache/`: `LumenSurfaceCache.usf`, `LumenSurfaceCache.ush`, `LumenSurfaceCacheSampling.ush`
- `Private/StochasticLighting/`: `StochasticLightingCommon.ush`, `StochasticLightingRayTracing.ush`, `StochasticLightingTileClassification.usf`, `StochasticLightingUpsample.ush`, `StochasticLightingVisualize.ush`
- `Shared/`: `LumenDefinitions.h`

### A28 레이 트레이싱·패스 트레이싱·디노이저(후속) (161)

- `Private/PathTracing/Light/`: `PathTracingCapsuleLight.ush`, `PathTracingDirectionalLight.ush`, `PathTracingLightCommon.ush`, `PathTracingLightGrid.ush`, `PathTracingLightSampling.ush`, `PathTracingPointLight.ush`, `PathTracingRectLight.ush`, `PathTracingSkyLight.ush`
- `Private/PathTracing/Material/`: `FirstBounceRayGuidingCommon.ush`, `PathTracingEnergyConservation.ush`, `PathTracingEye.ush`, `PathTracingFresnel.ush`, `PathTracingGlossy.ush`, `PathTracingHair.ush`, `PathTracingLambert.ush`, `PathTracingMaterialCommon.ush`, `PathTracingMaterialSampling.ush`, `PathTracingMedium.ush`, `PathTracingRadianceProbe.ush`, `PathTracingSubstrate.ush`, `PathTracingSubstrateCommon.ush`, `PathTracingSubstrateFoliage.ush`, `PathTracingSubstrateSolidGlass.ush`, `PathTracingSubstrateThinGlass.ush`
- `Private/PathTracing/`: `PathTracing.usf`, `PathTracingAdaptiveStart.usf`, `PathTracingAdaptiveUtils.ush`, `PathTracingBuildAdaptiveError.usf`, `PathTracingBuildAtmosphereLUT.usf`, `PathTracingBuildCloudAccelerationMap.usf`, `PathTracingBuildLightGrid.usf`, `PathTracingCommon.ush`, `PathTracingCompositingPixelShader.usf`, `PathTracingCopyDepth.usf`, `PathTracingCore.ush`, `PathTracingDebug.usf`, `PathTracingDecals.ush`, `PathTracingDefaultHitShader.usf`, `PathTracingGPULightmassMaterialHitShader.usf`, `PathTracingHeterogeneousVolumeMaterialShader.usf`, `PathTracingInitExtinctionCoefficient.usf`, `PathTracingLightingMissShader.usf`, `PathTracingMaterialHitShader.usf`, `PathTracingMissShader.usf`, `PathTracingShaderUtils.ush`, `PathTracingSkylightMISCompensation.usf`, `PathTracingSkylightPrepare.usf`, `PathTracingSpatialTemporalDenoising.usf`, `PathTracingSwizzleScanlines.usf`, `PathTracingVolumetricCloudMaterialShader.usf`
- `Private/PathTracing/Utilities/`: `PathTracingRIS.ush`, `PathTracingRandomSequence.ush`
- `Private/PathTracing/Volume/`: `PathTracingAtmosphere.ush`, `PathTracingAtmosphereCommon.ush`, `PathTracingClouds.ush`, `PathTracingCloudsCommon.ush`, `PathTracingCloudsMaterialCommon.ush`, `PathTracingFog.ush`, `PathTracingHeterogeneousVolumes.ush`, `PathTracingHeterogeneousVolumesForwardDecl.ush`, `PathTracingHeterogeneousVolumesMaterialCommon.ush`, `PathTracingHeterogeneousVolumesPreshadingCommon.ush`, `PathTracingHeterogeneousVolumesTrackingCommon.ush`, `PathTracingVolume.ush`, `PathTracingVolumeCommon.ush`, `PathTracingVolumeSampling.ush`
- `Private/RayTracing/`: `CompositeAmbientOcclusionPS.usf`, `CompositeSkyLightPS.usf`, `GenerateSkyLightVisibilityRaysCS.usf`, `MipTreeCommon.ush`, `RayGenUtils.ush`, `RayTracingAmbientOcclusionRGS.usf`, `RayTracingBarycentrics.usf`, `RayTracingBuildDecalGrid.usf`, `RayTracingBuildLightGrid.usf`, `RayTracingBuiltInShaders.usf`, `RayTracingCalcInterpolants.ush`, `RayTracingCapsuleLight.ush`, `RayTracingCommon.ush`, `RayTracingCommonMetal.ush`, `RayTracingDebug.usf`, `RayTracingDebugCHS.usf`, `RayTracingDebugHitStats.usf`, `RayTracingDebugHitStatsCHS.usf`, `RayTracingDebugHitStatsUtils.usf`, `RayTracingDebugInstanceOverlap.usf`, `RayTracingDebugMS.usf`, `RayTracingDebugPicking.usf`, `RayTracingDebugTraversal.usf`, `RayTracingDebugUtils.ush`, `RayTracingDecalGrid.ush`, `RayTracingDecalMaterialShader.usf`, `RayTracingDefaultDecalHitShader.usf`, `RayTracingDeferredMaterials.usf`, `RayTracingDeferredMaterials.ush`, `RayTracingDeferredShadingCommon.ush`, `RayTracingDirectionalLight.ush`, `RayTracingDispatchDesc.usf`, `RayTracingDynamicMesh.usf`, `RayTracingFeedback.usf`, `RayTracingGatherPoints.ush`, `RayTracingHitGroupCommon.ush`, `RayTracingInstanceBufferUtil.usf`, `RayTracingLightCullingCommon.ush`, `RayTracingLightFunctionCommon.ush`, `RayTracingLightingCommon.ush`, `RayTracingLightingMS.usf`, `RayTracingMaterialDefaultHitShaders.usf`, `RayTracingMaterialHitShaders.usf`, `RayTracingOcclusionRGS.usf`, `RayTracingPrimaryRays.usf`, `RayTracingRayCone.ush`, `RayTracingRectLight.ush`, `RayTracingReflectionEnvironment.ush`, `RayTracingShaderUtils.ush`, `RayTracingSkyLightCommon.ush`, `RayTracingSkyLightEvaluation.ush`, `RayTracingSkyLightRGS.usf`, `RayTracingSphereLight.ush`, `RayTracingTest.usf`, `RayTracingValidation.usf`, `SkyLightMipTreeCommon.ush`, `SkyLightVisibilityRaysData.ush`, `TraceRayInline.ush`, `TraceRayInlineCommon.ush`, `TraceRayInlineD3D12.ush`, `TraceRayInlineMetal.ush`, `TraceRayInlineStatistics.usf`, `TraceRayInlineVulkan.ush`, `VFXTraceRay.ush`
- `Private/ScreenSpaceDenoise/`: `DenoisingCommon.ush`, `SSDCommon.ush`, `SSDComposeHarmonics.usf`, `SSDCompressMetadata.usf`, `SSDDefinitions.ush`, `SSDInjest.usf`, `SSDMetadata.ush`, `SSDPublic.ush`, `SSDPublicBufferEncoding.ush`, `SSDPublicHarmonics.ush`, `SSDSignalAccumulator.ush`, `SSDSignalAccumulatorArray.ush`, `SSDSignalArray.ush`, `SSDSignalBufferEncoding.ush`, `SSDSignalCompression.ush`, `SSDSignalCore.ush`, `SSDSignalFramework.ush`, `SSDSpatialAccumulation.usf`, `SSDSpatialKernel.ush`, `SSDTemporalAccumulation.usf`
- `Private/`: `VisualizeRayBuffer.usf`
- `Shared/`: `PathTracingDefinitions.h`, `RayTracingBuiltInResources.h`, `RayTracingDebugTypes.h`, `RayTracingDefinitions.h`, `RayTracingPayloadType.h`, `RayTracingTypes.h`, `RaytracingDebugDefinitions.h`
- `Shared/ThirdParty/`: `FastMinimumStorageRayTriangleIntersection.tps`, `RayTriangleIntersection.h`, `WatertightRayTriangleIntersection.tps`

### A29 Substrate(후속) (31)

- `Private/Substrate/Glint/`: `GlintThirdParty.ush`, `real-time_geometric_glint.tps`
- `Private/Substrate/`: `Substrate.ush`, `SubstrateDBuffer.usf`, `SubstrateDeferredLighting.ush`, `SubstrateEvaluation.ush`, `SubstrateExport.ush`, `SubstrateExportCommon.ush`, `SubstrateForwardLighting.ush`, `SubstrateLegacyConversion.ush`, `SubstrateLightingCommon.ush`, `SubstrateMaterialClassification.usf`, `SubstrateMaterialSampling.usf`, `SubstrateMaterialSampling.ush`, `SubstrateMobileForwardLighting.ush`, `SubstrateRead.ush`, `SubstrateRoughRefraction.usf`, `SubstrateStatisticalOperators.ush`, `SubstrateSubsurface.ush`, `SubstrateTile.usf`, `SubstrateTile.ush`, `SubstrateToonBSDF.ush`, `SubstrateTree.ush`, `SubstrateVisualize.usf`, `SubstrateVisualizeCommon.ush`
- `Private/`: `ToonProfile.usf`, `ToonProfileCommon.ush`
- `Public/`: `SubstratePublic.ush`
- `Shared/`: `SubstrateDefinitions.h`, `SubstrateVisualizeDefinitions.h`, `ToonProfileDefinitions.h`

### A30 가상 그림자 맵(후속) (38)

- `Private/VirtualShadowMaps/`: `Desaturate.usf`, `VirtualShadowMapBuildPerPageDrawCommands.usf`, `VirtualShadowMapCacheGPUInvalidation.usf`, `VirtualShadowMapCacheInvalidation.ush`, `VirtualShadowMapCacheLoadBalancer.usf`, `VirtualShadowMapCompactViews.usf`, `VirtualShadowMapComputeExplicitChunkDrawsViewMask.usf`, `VirtualShadowMapCopyStats.usf`, `VirtualShadowMapDebug.usf`, `VirtualShadowMapHandle.ush`, `VirtualShadowMapLightGrid.ush`, `VirtualShadowMapMaskBitsCommon.ush`, `VirtualShadowMapPageAccessCommon.ush`, `VirtualShadowMapPageCacheCommon.ush`, `VirtualShadowMapPageManagement.usf`, `VirtualShadowMapPageMarking.usf`, `VirtualShadowMapPageMarking.ush`, `VirtualShadowMapPageOverlap.ush`, `VirtualShadowMapPerPageDispatch.ush`, `VirtualShadowMapPhysicalPageManagement.usf`, `VirtualShadowMapPrintStats.usf`, `VirtualShadowMapProjection.usf`, `VirtualShadowMapProjectionCommon.ush`, `VirtualShadowMapProjectionComposite.usf`, `VirtualShadowMapProjectionDirectional.ush`, `VirtualShadowMapProjectionFilter.ush`, `VirtualShadowMapProjectionSpot.ush`, `VirtualShadowMapProjectionStructs.ush`, `VirtualShadowMapSMRTCommon.ush`, `VirtualShadowMapSMRTTemplate.ush`, `VirtualShadowMapScreenRayTrace.ush`, `VirtualShadowMapShadowCasterBounds.usf`, `VirtualShadowMapShadowCasterColor.usf`, `VirtualShadowMapStats.ush`, `VirtualShadowMapThrottle.usf`, `VirtualShadowMapTransmissionCommon.ush`, `VirtualShadowMapVisualize.ush`
- `Shared/`: `VirtualShadowMapDefinitions.h`

### A31 MegaLights(후속) (33)

- `Private/MegaLights/`: `MegaLights.usf`, `MegaLights.ush`, `MegaLightsDebug.usf`, `MegaLightsDenoiser.ush`, `MegaLightsDenoiserSpatial.usf`, `MegaLightsDenoiserTemporal.usf`, `MegaLightsEstimateLight.ush`, `MegaLightsFilterVisibleLightHash.usf`, `MegaLightsHardwareRayTracing.usf`, `MegaLightsLightComplexity.usf`, `MegaLightsLightPowerDelta.usf`, `MegaLightsLightTargetPDF.ush`, `MegaLightsMaterial.ush`, `MegaLightsRayTracing.usf`, `MegaLightsRayTracing.ush`, `MegaLightsSampling.usf`, `MegaLightsSampling.ush`, `MegaLightsSamplingDefinitionsCommon.ush`, `MegaLightsShading.usf`, `MegaLightsShading.ush`, `MegaLightsTileClassification.ush`, `MegaLightsVSMMarking.usf`, `MegaLightsVSMTracing.usf`, `MegaLightsVisibility.ush`, `MegaLightsVisibleLightHash.usf`, `MegaLightsVisualize.usf`, `MegaLightsVisualizeHardwareRayTracing.usf`, `MegaLightsVolume.ush`, `MegaLightsVolumeHardwareRayTracing.usf`, `MegaLightsVolumeRayTracing.usf`, `MegaLightsVolumeSampling.usf`, `MegaLightsVolumeShading.usf`
- `Shared/`: `MegaLightsDefinitions.h`

### A32 HairStrands(후속) (83)

- `Private/`: `HairBsdf.ush`, `HairShadingCommon.ush`
- `Private/HairStrands/`: `HairBsdfSample.ush`, `HairCardsAttributeCommon.ush`, `HairCardsDeformation.usf`, `HairCardsGeneration.usf`, `HairCardsTextureCommon.ush`, `HairCardsVertexFactory.ush`, `HairStrandsAABB.usf`, `HairStrandsAABBCommon.ush`, `HairStrandsAttributeCommon.ush`, `HairStrandsAttributeTemplate.ush`, `HairStrandsBindingCommon.ush`, `HairStrandsClusterCommon.ush`, `HairStrandsClusterCulling.usf`, `HairStrandsCommon.ush`, `HairStrandsComposition.usf`, `HairStrandsCoverage.usf`, `HairStrandsDebug.usf`, `HairStrandsDebugCommonStruct.ush`, `HairStrandsDeepShadowAllocation.usf`, `HairStrandsDeepShadowCommon.ush`, `HairStrandsDeepShadowCommonStruct.ush`, `HairStrandsDeepShadowMask.usf`, `HairStrandsDeepShadowPS.usf`, `HairStrandsDeepShadowVS.usf`, `HairStrandsDeepTransmittanceCommon.ush`, `HairStrandsDeepTransmittanceDualScattering.ush`, `HairStrandsDeepTransmittanceMask.usf`, `HairStrandsEnvironmentAO.usf`, `HairStrandsEnvironmentLighting.usf`, `HairStrandsEnvironmentLightingCommon.ush`, `HairStrandsFollicleMask.usf`, `HairStrandsForwardCulling.usf`, `HairStrandsForwardRaster.usf`, `HairStrandsGBufferCommon.ush`, `HairStrandsGuideDeform.usf`, `HairStrandsHitProxy.usf`, `HairStrandsInterpolation.usf`, `HairStrandsLUT.usf`, `HairStrandsLightSample.usf`, `HairStrandsMaterialCommon.ush`, `HairStrandsMaterialPS.usf`, `HairStrandsMaterialVS.usf`, `HairStrandsMesh.usf`, `HairStrandsPack.ush`, `HairStrandsRaytracing.ush`, `HairStrandsRaytracingGeometry.usf`, `HairStrandsScreenTracing.ush`, `HairStrandsSkinUpdate.usf`, `HairStrandsTangent.usf`, `HairStrandsTexturesGeneration.usf`, `HairStrandsTileCommon.ush`, `HairStrandsTranscode.usf`, `HairStrandsVelocity.usf`, `HairStrandsVertexFactory.ush`, `HairStrandsVertexFactoryCommon.ush`, `HairStrandsVisibilityClearPS.usf`, `HairStrandsVisibilityCommon.ush`, `HairStrandsVisibilityCommonStruct.ush`, `HairStrandsVisibilityCompaction.usf`, `HairStrandsVisibilityCompactionComputeRaster.usf`, `HairStrandsVisibilityComputeSampleCoverage.usf`, `HairStrandsVisibilityCopyIndirectArg.usf`, `HairStrandsVisibilityDepthPS.usf`, `HairStrandsVisibilityFillOpaqueDepthPS.usf`, `HairStrandsVisibilityPS.usf`, `HairStrandsVisibilityRasterClassification.usf`, `HairStrandsVisibilityRasterCompute.usf`, `HairStrandsVisibilityRasterComputeNaive.usf`, `HairStrandsVisibilityRasterHW.usf`, `HairStrandsVisibilityTile.usf`, `HairStrandsVisibilityTransmittanceDepthPS.usf`, `HairStrandsVisibilityUtils.ush`, `HairStrandsVisibilityVS.usf`, `HairStrandsVoxelDebug.usf`, `HairStrandsVoxelPageCommon.ush`, `HairStrandsVoxelPageCommonStruct.ush`, `HairStrandsVoxelPageTraversal.ush`, `HairStrandsVoxelRasterCompute.usf`, `RenderCurveRaster.usf`
- `Shared/`: `HairStrandsDefinitions.h`, `RenderCurveDefinitions.h`

### A33 가상 텍스처·MaterialCache·SVT(후속) (26)

- `Private/`: `GPUFeedbackCompaction.usf`, `PageTableUpdate.usf`, `VTtest.usf`, `VirtualTextureAdapter.usf`, `VirtualTextureCommon.ush`, `VirtualTextureCompress.usf`, `VirtualTextureMaterial.usf`, `VirtualTextureMaterial.ush`, `VisualizeVirtualTexture.usf`
- `Private/MaterialCache/`: `MaterialCacheABufferBorderFillHardwarePages.usf`, `MaterialCacheABufferPages.usf`, `MaterialCacheCommon.ush`, `MaterialCacheMaterialCommon.ush`, `MaterialCacheShade.usf`, `MaterialCacheShadeCommon.ush`, `MaterialCacheUnwrapNaniteShade.usf`, `MaterialCacheUnwrapPixelShader.usf`, `MaterialCacheUnwrapVertexShader.usf`
- `Private/SparseVolumeTexture/`: `SparseVolumeTextureCommon.ush`, `UpdateSparseVolumeTexture.usf`, `VisualizeSparseVolumeTexture.usf`
- `Public/`: `IndirectVirtualTexture.ush`, `IndirectVirtualTextureCommon.ush`
- `Shared/`: `IndirectVirtualTextureDefinitions.h`, `MaterialCacheDefinitions.h`, `VirtualTextureDefinitions.h`

### A34 지형·식생·물·지오메트리 캐시(후속) (15)

- `Private/`: `GeometryCacheVertexFactory.ush`, `GeometryCollectionVertexFactory.ush`, `LandscapeGrassWeight.usf`, `LandscapeLayersPS.usf`, `LandscapePhysicalMaterial.usf`, `LandscapeVertexFactory.ush`, `SpeedTreeCommon.ush`, `WaterInfoTextureBlur.usf`, `WaterInfoTextureMerge.usf`
- `Private/Landscape/`: `LandscapeCommon.ush`, `LandscapeCulling.usf`, `LandscapeEditLayersHeightmaps.usf`, `LandscapeEditLayersUtils.usf`, `LandscapeEditLayersWeightmaps.usf`, `LandscapeMergeTexturesPS.usf`

### A35 XR·미디어·bindless·VRS·OIT·기타(후속) (25)

- `Private/`: `GPUBenchmark.usf`, `GameplayMediaEncoderShaders.usf`, `InstancedStereo.ush`, `MediaShaders.usf`, `NeuralPostProcessCommon.usf`, `OITCombine.usf`, `OITCommon.ush`, `OculusShaders.usf`, `PostProcessHMD.tps`, `PostProcessHMD.usf`, `ShaderBundleDispatch.usf`, `ShaderBundleWorkGraphCommon.ush`, `ShaderBundleWorkGraphDispatch.usf`, `StereoLayerShader.usf`, `UpdateDescriptorHandle.usf`, `XRShaders.usf`
- `Private/OIT/`: `OITSorting.usf`
- `Private/VariableRateShading/`: `VRSShadingRateCalculate.usf`, `VRSShadingRateCombine.usf`, `VRSShadingRateCommon.ush`, `VRSShadingRateFoveated.usf`, `VRSShadingRatePreview.usf`, `VRSShadingRateReproject.usf`
- `Public/`: `BindlessResources.ush`
- `Public/Platform/D3D/`: `PlatformBindlessResources.ush`

### X-bink 제외: Bink (1)

- `Private/`: `Bink.usf`

### X-build 제외: 빌드 규칙 (1)

- (루트): `Shaders.Build.cs`

### X-cascade 제외: Cascade 파티클 (14)

- `Private/`: `MeshParticleVertexFactory.ush`, `ParticleBeamTrailVertexFactory.ush`, `ParticleBoundsShader.usf`, `ParticleCurveInjectionShader.usf`, `ParticleGPUSpriteVertexFactory.ush`, `ParticleInjectionShader.usf`, `ParticleSimVisualizeShader.usf`, `ParticleSimulationCommon.ush`, `ParticleSimulationShader.usf`, `ParticleSortKeyGen.usf`, `ParticleSpriteVertexFactory.ush`, `ParticleVertexFactoryCommon.ush`, `VectorFieldCompositeShaders.usf`, `VectorFieldVisualizationVertexFactory.ush`

### X-chaos 제외: Chaos 물리 필드 (6)

- `Private/`: `PhysicsFieldBuilder.usf`, `PhysicsFieldEval.ush`, `PhysicsFieldReset.usf`, `PhysicsFieldSampler.ush`, `PhysicsFieldShared.ush`, `PhysicsFieldVisualizer.usf`

### X-console 제외: 콘솔 전용 (2)

- `Private/`: `HTileEncoding.ush`, `RTWriteMaskDecode.usf`

### X-legacy 제외: 폐기·제거 예정 (4)

- `Private/`: `ClusteredDeferredShadingPixelShader.usf`, `ClusteredDeferredShadingVertexShader.usf`, `SimpleElementNormalMapPixelShader.usf`
- `Private/SSRT/`: `SSRTDiffuseIndirect.usf`

### X-mobile 제외: UE 모바일 렌더러 전용 (18)

- `Private/`: `LateZDummyPS.usf`, `MobileBasePassCommon.ush`, `MobileBasePassPixelShader.usf`, `MobileBasePassVertexShader.usf`, `MobileDeferredShading.usf`, `MobileDeferredUtils.usf`, `MobileFog.usf`, `MobileGGX.ush`, `MobileLightingCommon.ush`, `MobileSSR.ush`, `OcclusionFeedbackShaders.usf`, `PostProcessMobile.usf`, `SSProfilePreIntegratedMobile.usf`, `SceneDataMobileLoader.ush`, `SceneDataMobileWriter.ush`
- `Public/Platform/GL/`: `GLSubpassSupport.ush`
- `Public/Platform/Metal/`: `MetalSubpassSupport.ush`
- `Public/Platform/Vulkan/`: `VulkanSubpassSupport.ush`

### X-standalone 제외: 단독 렌더러 (7)

- `StandaloneRenderer/D3D/`: `GammaCorrectionCommon.hlsl`, `SlateDefaultVertexShader.hlsl`, `SlateElementPixelShader.hlsl`
- `StandaloneRenderer/OpenGL/`: `SlateElementPixelShader.glsl`, `SlateVertexShader.glsl`, `SplashFragmentShader.glsl`, `SplashVertexShader.glsl`

### X-unused 제외: 미사용 (2)

- `Private/`: `SimpleF32PixelShader.ush`, `SimpleF32VertexShader.ush`

### X-vendor 제외: 벤더 확장 (12)

- `Public/`: `NvidiaExtensionsCommon.ush`
- `Shared/ThirdParty/AMD/`: `Agslib.tps`, `ags_shader_intrinsics_dx11.h`, `ags_shader_intrinsics_dx12.h`
- `Shared/ThirdParty/Intel/`: `IntelExtensions.h`, `IntelExtensions12.h`, `IntelExtensionsFramework.tps`
- `Shared/ThirdParty/NVIDIA/`: `NVAPI_vR570.tps`, `Nvapi.tps`, `nvHLSLExtns.h`, `nvHLSLExtnsInternal.h`, `nvShaderExtnEnums.h`

합계 1227개.
