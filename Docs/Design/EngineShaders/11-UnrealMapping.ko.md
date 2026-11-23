# 엔진 셰이더 — 11. UE 대응표(Rocks 파일 → UE 파일)

[목차](README.ko.md)

## 11. UE 대응표

Rocks 파일(가상 경로, `/Engine/` 생략)마다 역할이 대응하는 UE 5.8 파일이다. 대응은 역할 기준이며 내용이 같다는 뜻이 아니다(UE 원문을 옮기지 않는다 — 2.2절). "—"는 UE에 같은 단위 파일이 없고 여러 파일에 흩어져 있거나 Rocks가 새로 둔 것이다. 경로는 `SH/Private/` 기준, `Public/`·`Shared/`는 그 루트다.

### 11.1 기본 영역

| Rocks | UE 5.8 |
|---|---|
| `Public/Platform.rsh` | `Public/Platform.ush` |
| `Public/ShaderFeatures.rsh` | `Public/Platform/{D3D,GL,Metal,Vulkan}/*Common.ush`의 기능 매크로, `Definitions.usf` |
| `Public/Backend/*.rsh` | `Public/Platform/D3D/D3DCommon.ush`·`GL/GLCommon.ush`·`Metal/MetalCommon.ush`·`Vulkan/VulkanCommon.ush` |
| `Common/ShaderPrelude.rsh` | `Common.ush`(일부) |
| `Common/ShaderAssert.rsh` | `AssertionMacros.ush` |
| `Common/StageOutputs.rsh` | `PixelShaderOutputCommon.ush`, `ShaderOutputCommon.ush`, `ComputeShaderOutputCommon.ush` |
| `Common/ComputeDispatch.rsh`(CS 전용, SL1) | `ComputeShaderUtils.ush` |
| `Common/WaveOperations.rsh`(CS 전용, SL1) | `WaveOpUtil.ush`, `Public/WaveBroadcastIntrinsics.ush` |
| `Common/ScalarArrays.rsh` | `GeneratedUniformBufferTypes.ush` |
| `Common/RootConstants.rsh` | `Public/RootConstants.ush`, `Public/Platform/D3D/D3DRootConstants.ush` |
| `Shared/ShaderTypeAliases.h` | `Shared/HLSLTypeAliases.h`, `Shared/HLSLMathAliases.h` |
| `Shared/ShaderStaticAssert.h` | `Shared/HLSLStaticAssert.h` |
| `Shared/ReservedBindingSpaces.h` | `Shared/HLSLReservedSpaces.h` |
| `Math/MathConstants.rsh`·`ScalarMath.rsh` | `Common.ush`(상수·보조 함수 부분) |
| `Math/VectorMath.rsh` | `Common.ush`·`MonteCarlo.ush`(기저 생성 부분) |
| `Math/MatrixMath.rsh` | `Matrices.ush` |
| `Math/Quaternion.rsh` | `Quaternion.ush` |
| `Math/FastApproximations.rsh` | `FastMath.ush`, `FastMathThirdParty.ush` |
| `Math/ExtendedPrecision.rsh` | `DoubleFloat.ush`, `DoubleFloatOperations.ush`, `DoubleFloatVectorDefinition.ush`, `WorldSpaceMath.ush`(확장 정밀도 분기) |
| `Math/DualNumbers.rsh` | `DualNumber.ush` |
| `Math/BitPacking.rsh` | `BitPacking.ush`, `PackUnpack.ush` |
| `Math/UnitVectorEncoding.rsh` | `OctahedralCommon.ush` |
| `Math/DitheredQuantization.rsh` | `Quantization.ush` |
| `Math/MortonOrder.rsh` | `MortonCode.ush` |
| `Math/IntegerHash.rsh` | `Hash.ush`, `RandomPCG.ush`(해시 부분) |
| `Math/RandomNumbers.rsh` | `Random.ush`, `RandomPCG.ush` |
| `Math/ScreenNoise.rsh` | `RandomInterleavedGradientNoise.ush`, `BlueNoise.ush`, `BlueNoiseCommon.ush` |
| `Math/LowDiscrepancy.rsh` | `SobolRandom.ush`, `HaltonUtilities.ush`, `MonteCarlo.ush`(Hammersley) |
| `Math/SampleWarping.rsh` | `MonteCarlo.ush` |
| `Math/ProceduralNoise.rsh` | `Random.ush`(잡음 부분) |
| `Math/SphericalHarmonics.rsh` | `SHCommon.ush` |
| `Math/SphericalGaussian.rsh` | `SphericalGaussian.ush` |
| `Math/RayIntersection.rsh` | `IntersectionUtils.ush` |
| `Math/Barycentrics.rsh` | `Barycentrics.ush` |
| `Color/ColorSpaces.rsh` | `ColorSpace.ush`, `ColorUtils.ush` |
| `Color/ChromaticAdaptation.rsh` | `TonemapCommon.ush`(white balance 부분), `ColorSpace.ush` |
| `Color/TransferFunctions.rsh` | `GammaCorrectionCommon.ush`, `TonemapCommon.ush`(부호화 부분) |
| `Color/FilmicToneCurve.rsh` | `TonemapCommon.ush`(filmic 곡선) |
| `Color/ColorGrading.rsh` | `TonemapCommon.ush`·`PostProcessCombineLUTs.usf`(색 보정 부분) |
| `Color/LutCoordinates.rsh` | `TonemapCommon.ush`·`PostProcessTonemap.usf`(LUT 좌표) |
| `Color/DisplayEncoding.rsh` | `TonemapCommon.ush`(출력 장치 부호화) |
| `Color/AcesOutputTransform.rsh`(후속) | `ACES/ACESCommon.ush`, `ACESOutputTransform.ush`, `ACESTonescale.ush`, `ACESDisplayEncoding.ush`, `ACESUtilities.ush` |
| `Color/ColorMaps.rsh` | `ColorMap.ush`, `Visualization.ush` |
| `Color/ColorVisionDeficiency.rsh` | `ColorDeficiency.ush` |
| `Color/ColorDifference.rsh` | `ColorDifference.ush` |
| `Shading/MicrofacetTerms.rsh`·`DiffuseTerms.rsh` | `BRDF.ush` |
| `Shading/EnvironmentBrdf.rsh` | `BRDF.ush`(환경 BRDF 부분) |
| `Shading/EnergyConservation.rsh` | `ShadingEnergyConservation.ush`, `ShadingEnergyConservationTemplate.ush` |
| `Shading/BuildEnergyTables.rsf` | `ShadingEnergyConservationTable.usf` |
| `Shading/SurfaceAttributes.rsh` | — (UE는 머티리얼 픽셀 입력 구조체 `MaterialTemplate.ush`와 GBuffer 데이터 구조체 `DeferredShadingCommon.ush`에 나뉘어 있다. Rocks는 RN 14.1절 계약 하나) |
| `Shading/SurfaceShading.rsh`·`DirectLighting.rsh` | `ShadingCommon.ush`, `ShadingModelsMaterial.ush`, Substrate 단일 slab 평가(역할만, 결정 SLD3) |
| `Shading/LightAccumulation.rsh` | `LightAccumulator.ush` |
| `Shading/SphereLightIntegration.rsh` | `CapsuleLight.ush`, `CapsuleLightSampling.ush`, `AreaLightCommon.ush` |
| `Shading/RectLightIntegration.rsh` | `RectLight.ush`, `AreaLightCommon.ush` |
| `/Plugin/EngineShaderTests/Shading/ShadingTestKernels.rsf`(시험 루트) | `ShadingFurnaceTest.usf` |
| `Shading/ClearCoatShading.rsh` | `ClearCoatCommon.ush` |
| `Shading/SubsurfaceShading.rsh` | `TransmissionCommon.ush` |
| `Shading/TransmissionThickness.rsh` | `TransmissionThickness.ush` |
| `Shading/ClothShading.rsh`·`EyeShading.rsh` | — (`BRDF.ush`·고전 모델 함수에 흩어짐) |
| `Shading/ThinTranslucentShading.rsh` | `ThinTranslucentCommon.ush` |
| `Shading/SpecularProfile.rsh`·`BuildSpecularProfileAtlas.rsf` | `SpecularProfileCommon.ush`, `SpecularProfile.usf` |
| `Subsurface/SubsurfaceProfiles.rsh`·`BuildSubsurfaceProfileAtlas.rsf` | `SubsurfaceProfileCommon.ush`, `BurleyNormalizedSSSCommon.ush` |
| `Subsurface/ScreenSpaceSubsurface.rsf` | `PostProcessSubsurface.usf`, `PostProcessSubsurfaceCommon.ush`, `PostProcessSubsurfaceDiffuse.usf`·`.ush`, `PostProcessSubsurfaceTile.usf`, `SubsurfaceBurleyNormalized.ush`, `SeparableSSS.ush` |
| `Water/SingleLayerWaterShading.rsh` | `SingleLayerWaterCommon.ush`, `SingleLayerWaterShading.ush` |
| `Water/SingleLayerWaterComposite.rsf` | `SingleLayerWaterComposite.usf`, `SingleLayerWaterRefractionCulling.usf` |
| `Material/MaterialCommon.rsh`·`MaterialNodeLibrary.rsh` | `MaterialTemplate.ush` |
| `Material/MaterialTextureSampling.rsh` | `MaterialTexture.ush` |
| `VertexInput/VertexInputInterface.rsh` | `VertexFactoryDefaultInterface.ush`, `VertexFactoryDefaultFwd.ush` |
| `VertexInput/VertexInputCommon.rsh` | `VertexFactoryCommon.ush` |
| `VertexInput/BuiltinVertexInput.rsh` | — (Rocks 엔진 기본 정점 입력, RB 10.2절) |
| `VertexInput/LocalMeshVertexInput.rsh` | `LocalVertexFactory.ush`, `LocalVertexFactoryCommon.ush` |
| `VertexInput/SplineMeshDeform.rsh` | `SplineMeshCommon.ush` |
| `VertexInput/SkinnedMeshVertexInput.rsh` | `GpuSkinVertexFactory.ush`, `GpuSkinCommon.ush` |
| `Scene/ViewConstants.rsh` | `CommonViewUniformBuffer.ush`, `ViewData.ush` |
| `Scene/DepthConversion.rsh` | `PositionReconstructionCommon.ush`, `Common.ush`(깊이 변환 부분) |
| `Scene/SceneTextures.rsh` | `SceneTexturesCommon.ush`, `SceneTextureParameters.ush` |
| `Scene/GpuSceneRead.rsh` | `SceneData.ush` |
| `Scene/GpuSceneUpload.rsf` | `GPUScene/GPUSceneDataManagement.usf`, `GPUScene/GPUSceneWriter.ush` |
| `Scene/BuildHzb.rsf` | `HZB.usf`, `HZB.ush` |
| `Scene/HzbOcclusion.rsh` | `HZBOcclusion.usf` |
| `Scene/HzbScreenTrace.rsh` | `HZBTracing.ush` |
| `Scene/InstanceCulling.rsf` | `InstanceCulling/BuildInstanceDrawCommands.usf`, `CompactVisibleInstances.usf`, `InstanceCompactionCommon.ush`, `InstanceCullingCommon.ush`, `InstanceCullingLoadBalancer.ush`, `InstanceCullingSetup.ush` |
| `Scene/LodDitherTransition.rsf`·`.rsh` | `DitheredTransitionStencil.usf` |
| `BasePass/MeshPassCommon.rsh` | `BasePassVertexCommon.ush`, `BasePassCommon.ush`, `BasePassCommonFunctions.ush` |
| `BasePass/DepthPasses.rsf` | `DepthOnlyVertexShader.usf`, `DepthOnlyPixelShader.usf`, `PositionOnlyDepthVertexShader.usf` |
| `BasePass/VelocityEncoding.rsh`·`VelocityPass.rsf` | `VelocityCommon.ush`, `VelocityShader.usf`, `VelocityUpdate.usf` |
| `BasePass/ForwardOpaquePass.rsf`·`GBufferPass.rsf` | `BasePassVertexShader.usf`, `BasePassPixelShader.usf` |
| `BasePass/GBufferEncoding.rsh` | `DeferredShadingCommon.ush`(Blendable 배치), `GBufferHelpers.ush` |
| `Shared/SurfaceEnums.h` | `ShadingCommon.ush`(번호 정의) |
| `Shared/ViewLayout.h`·`GpuSceneLayout.h`·`InstanceCullingLayout.h`·`EnvironmentFlags.h` | `Shared/ViewDefinitions.h`, `SceneDefinitions.h`, `InstanceCullingDefinitions.h`·`SceneCullingDefinitions.h`, `EnvironmentComponentsFlags.h` |
| `Lighting/LightRecord.rsh` | `LightData.ush`, `LightDataUniforms.ush`, `LightShaderParameters.ush` |
| `Lighting/LightAttenuation.rsh` | `DynamicLightingCommon.ush` |
| `Lighting/LightGridLookup.rsh` | `LightGridCommon.ush` |
| `Lighting/BuildLightGrid.rsf` | `LightGridInjection.usf` |
| `Lighting/IesProfile.rsh`·`BuildIesAtlas.rsf` | `IESLightProfilesCommon.ush`, `IESAtlas.usf` |
| `Lighting/RectLightTextureAtlas.rsf` | `RectLightAtlas.usf` |
| `Lighting/LightFunctionAtlas.rsf`·`.rsh` | `LightFunctionAtlas/LightFunctionAtlasCommon.usf`·`LightFunctionAtlasRender.usf`, `LightFunctionCommon.ush` |
| `Lighting/ForwardLighting.rsh` | `ForwardLightingCommon.ush`, `DeferredLightingCommon.ush`(현재 경로 부분) |
| `Lighting/DeferredLightingPass.rsf` | `DeferredLightPixelShaders.usf` |
| `Lighting/LightingDebugCounters.rsh` | `LightAccumulator.ush`의 광원 컬링 시각화 부분 |
| `Shared/LightRecordLayout.h`·`LightGridLayout.h` | `Shared/LightDefinitions.h`·`LightSceneData.h`·`LightViewData.h`, `LightGridDefinitions.h` |
| `Shadow/ShadowDepthPass.rsf` | `ShadowDepthVertexShader.usf`, `ShadowDepthPixelShader.usf`, `ShadowDepthCommon.ush` |
| `Shadow/ShadowAtlas.rsh`·`CascadeSelection.rsh`·`ShadowSampling.rsh` | `ForwardShadowingCommon.ush`, `ShadowProjectionCommon.ush` |
| `Shadow/ShadowFiltering.rsh` | `ShadowFilteringCommon.ush`, `ShadowPercentageCloserFiltering.ush` |
| `Shadow/ContactShadows.rsh` | `ScreenSpaceShadowRayCast.ush` |
| `Reflection/CubemapSampling.rsh` | `CubemapCommon.ush` |
| `Reflection/SkyLighting.rsh` | `SkyLightingShared.ush`, `SkyLightingDiffuseShared.ush` |
| `Reflection/ReflectionCaptures.rsh` | `ReflectionEnvironmentShared.ush`, `ReflectionEnvironmentComposite.ush` |
| `Reflection/FilterEnvironmentCube.rsf` | `ReflectionEnvironmentShaders.usf` |
| `Reflection/IndirectComposite.rsf` | `DiffuseIndirectComposite.usf`, `ReflectionEnvironmentPixelShader.usf` |
| `Reflection/SceneCaptureOutput.rsf` | `SceneCapturePixelShader.usf` |
| `Reflection/PlanarReflection.rsf`·`.rsh` | `PlanarReflectionShaders.usf`, `PlanarReflectionShared.ush` |
| `Reflection/ScreenSpaceReflections.rsf`·`.rsh` | `SSRT/SSRTReflections.usf`, `SSRTRayCast.ush`, `SSRTTileClassification.usf`, `SSRTTileClassificationBuffer.ush`, `SSRTPrevFrameReduction.usf`, `DefaultSSRTiles.usf`, `ScreenSpaceReflectionTileCommons.ush` |
| `Occlusion/AmbientOcclusionCommon.rsh` | `PostProcessAmbientOcclusionCommon.ush` |
| `Occlusion/ScreenSpaceAo.rsf`·`GroundTruthAo.rsf` | `PostProcessAmbientOcclusion.usf`(SSAO·GTAO 경로) |
| `Decal/DecalCommon.rsh` | `DecalCommon.ush` |
| `Decal/DeferredDecalPass.rsf` | `DeferredDecal.usf` |
| `Decal/DBufferDecalPass.rsf`·`DBufferRead.rsh` | `DBufferDecalShared.ush`, `DBufferNormalReprojection.ush` |
| `Decal/MeshDecalPass.rsf` | `MeshDecals.usf` |
| `StaticLighting/LightmapSampling.rsh` | `LightmapCommon.ush`, `LightmapData.ush` |
| `StaticLighting/VolumeLightmapSampling.rsh`·`UpdateVolumeLightmap.rsf` | `VolumetricLightmapShared.ush`, `VolumetricLightmapStreaming.usf` |
| `Translucency/TranslucentPass.rsf`·`BlendModeOutput.rsh` | `BasePassPixelShader.usf`(투명 분기) |
| `Translucency/ComposeTranslucency.rsf` | `ComposeSeparateTranslucency.usf`, `SeparateTranslucency.ush`, `TranslucencyUpsampling.usf` |
| `Translucency/DistortionAccumulate.rsf`·`DistortionApply.rsf` | `DistortAccumulateVS.usf`·`PS.usf`, `DistortApplyScreenPS.usf`, `DistortFiltering.usf`, `DistortionCommon.ush` |
| `Atmosphere/ParticipatingMedia.rsh` | `ParticipatingMediaCommon.ush` |
| `Atmosphere/AtmosphereModel.rsh`·`BuildAtmosphereLuts.rsf`·`SkyAtmospherePass.rsf` | `SkyAtmosphereCommon.ush`, `SkyAtmosphere.usf` |
| `Atmosphere/HeightFog.rsh`·`HeightFogPass.rsf` | `HeightFogCommon.ush`, `HeightFogVertexShader.usf`, `HeightFogPixelShader.usf` |
| `PostProcess/ScreenPassCommon.rsh` | `ScreenPass.ush`, `PostProcessCommon.ush` |
| `PostProcess/Downsample.rsf` | `PostProcessDownsample.usf` |
| `PostProcess/ExposureCommon.rsh` | `EyeAdaptationCommon.ush`, `PostProcessExposureMaterial.ush` |
| `PostProcess/LuminanceHistogram.rsf` | `PostProcessHistogram.usf`, `PostProcessHistogramReduce.usf`, `PostProcessHistogramCommon.ush`, `Histogram.usf` |
| `PostProcess/AutoExposure.rsf` | `PostProcessEyeAdaptation.usf`, `PostProcessEyeAdaptationUtils.usf` |
| `PostProcess/BloomGaussian.rsf` | `PostProcessBloom.usf` |
| `PostProcess/BuildColorLut.rsf` | `PostProcessCombineLUTs.usf`, `PostProcessCombineLUTsInner.usf` |
| `PostProcess/TonemapPass.rsf` | `PostProcessTonemap.usf`, `PostProcessDeviceEncodingOnly.usf` |
| `PostProcess/FilmGrain.rsf` | `PostProcessing/FilmGrainReduce.usf`, `FilmGrainPackConstants.usf` |
| `PostProcess/Fxaa.rsf` | `FXAAShader.usf`, `Fxaa3_11.ush` |
| `PostProcess/TemporalAntiAliasing.rsf` | `TemporalAA.usf` |
| `PostProcess/SpatialUpscale.rsf` | `PostProcessUpscale.usf`, `PostProcessMitchellNetravali.usf` |
| `PostProcess/PostProcessMaterialPass.rsf` | `PostProcessMaterialShaders.usf` |
| `Utility/FullscreenTriangle.rsf` | `ScreenVertexShader.usf`, `Tools/FullscreenVertexShader.usf`, `FilterVertexShader.usf`, `ResolveVertexShader.usf` |
| `Utility/CopyTexture.rsf` | `CopyTextureShaders.usf`, `ScreenPixelShader.usf`, `ScreenPass.usf`, `PostProcessPassThrough.usf`, `PostProcessChannelMask.usf`, `PostProcessAlphaInvert.usf`, `Tools/DrawTexture.usf` |
| `Utility/CopyDepth.rsf` | `CopyDepthTexture.usf`, `CopyDepthTextureCS.usf` |
| `Utility/ClearResources.rsf` | `ClearReplacementShaders.usf`, `OneColorShader.usf`, `RenderGraphUtilities.usf`, `RendererUtils.usf` |
| `Utility/GenerateMips.rsf` | `ComputeGenerateMips.usf` |
| `Utility/GaussianFilter.rsf` | `FilterPixelShader.usf` |
| `Utility/MsaaResolve.rsf` | `ResolvePixelShader.usf`, `HdrCustomResolveShaders.usf` |
| `Utility/DownsampleDepth.rsf` | `DownsampleDepthPixelShader.usf` |
| `Utility/BufferOperations.rsf` | `ByteBuffer.usf`, `ByteBuffer.ush`, `CopyUIntBuffer.usf` |
| `Utility/IndirectArgs.rsf` | `Tools/SetupIndirectArgs.usf`, `RendererUtils.usf` |
| `Utility/TextureSamplingHelpers.rsh` | `TextureSampling.ush` |
| `Utility/TextureReadback.rsf` | `Tools/DumpTexture.usf` |
| `Utility/PlaceholderShaders.rsf` | `NullPixelShader.usf`, `SolidColorPixelShader.ush`, `MissingShaderPixelShader.usf` |
| `Ui/UiCommon.rsh`·`UiElements.rsf`·`GlyphSampling.rsh` | `SlateShaderCommon.ush`, `SlateVertexShader.usf`, `SlateElementPixelShader.usf` |
| `Ui/UiClipMask.rsf` | `SlateMaskingShader.usf` |
| `Ui/UiPostEffects.rsf` | `SlatePostProcessPixelShader.usf`, `SlatePostProcessColorDeficiencyPixelShader.usf` |
| `Ui/UiComposite.rsf` | `CompositeUIPixelShader.usf` |
| `Ui/SimpleElements.rsf` | `SimpleElementVertexShader.usf`, `SimpleElementPixelShader.usf`, `SimpleElementHitProxyPixelShader.usf`, `SimpleElementColorChannelMaskPixelShader.usf`, `SimpleElementTexture2DPreviewPixelShader.usf`, `SimpleElementVolumeTexturePreviewPixelShader.usf` |
| `Debug/TextureInspector.rsf` | `Tools/VisualizeTexture.usf`, `Tools/DebugAlphaChannel.usf` |
| `Debug/GpuMessages.rsh`·`.rsf` | `GPUMessaging.usf`, `GPUMessaging.ush` |
| `Debug/ShaderDebugPrint.rsh`·`DebugPrintDraw.rsf` | `ShaderPrint.ush`, `ShaderPrintCommon.ush`, `ShaderPrintDraw.usf`, `ShaderPrintDrawPrimitive.usf`, `MiniFontCommon.ush`, `PrintValue.ush` |
| `Debug/VisualizeBuffers.rsf` | `PostProcessVisualizeBuffer.usf`, `PostProcessVisualizeHDR.usf`, `PostProcessGBufferHints.usf`, `PostProcessTestImage.usf` |
| `Debug/DebugViewModes.rsf`·`.rsh` | `DebugViewModeCommon.ush`, `DebugViewModeVertexShader.usf`, `DebugViewModePixelShader.usf`, `ShaderComplexityApplyPixelShader.usf`, `QuadOverdraw.ush`, `QuadComplexityAccumulatePixelShader.usf`, `LightMapDensityShader.usf` |
| `Debug/VisualizeLighting.rsf` | `VisualizeShadingModels.usf`, `StationaryLightOverlapShaders.usf`, `VisualizeVolumetricLightmap.usf`, `Tools/VisualizeLightShape.usf` |
| `Debug/DebugLines.rsf`·`IconSprites.rsf` | — (UE는 동적 메시·단순 요소로 그린다) |
| `Debug/EditorComposite.rsf` | `PostProcessCompositePrimitives.usf`, `PostProcessSelectionOutline.usf`, `MeshEdges.usf` |
| `Debug/HitProxy.rsf` | `HitProxyVertexShader.usf`, `HitProxyPixelShader.usf`, `HitProxyCopy.usf` |
| `Debug/ColorScopes.rsf`(후속) | `Tools/ColorGradingHistogram.usf`, `ColorGradingParade.usf`, `ColorGradingVectorScope.usf` |
| `Debug/MeshPainting.rsf`(후속) | `MeshPaintVertexShader.usf`, `MeshPaintPixelShader.usf`, `MeshPaintTextureCommon.ush`, `meshpaintdilatevertexshader.usf`, `meshpaintdilatepixelshader.usf` |
| `Compute/GroupPrefixSum.rsh`·`GlobalPrefixSum.rsf` | `ThreadGroupPrefixSum.ush` |
| `Compute/GroupReduction.rsh` | `ReductionCommon.ush` |
| `Compute/WorkDistribution.rsh` | `WorkDistribution.ush`, `WorkGroupLoadBalancer.ush` |
| `Compute/BinarySearch.rsh` | `BinarySearch.ush` |
| `Compute/BitonicSort.rsf` | `BitonicSort.usf` |
| `Compute/RadixSort.rsf` | `RadixSortShaders.usf` |
| `Compute/GpuHashTable.rsh`(후속) | `HashTable.ush` |
| `Compute/BitStreamReader.rsh`(후속) | `BitStreamReaderImplementation.ush` |
| `Compute/QuadMessaging.rsh` | `PixelQuadMessagePassing.ush` |
| `Compute/BlockCompressionBc.rsh`·`BlockCompressionEtc.rsh`·`BlockCompressionAstc.rsh`(후속) | `BlockCompressionCommon.ush`, `BCCompressionCommon.ush`, `ETCCompressionCommon.ush`, `ASTCCompressionCommon.ush`, `IntegerSequenceEncoding.ush` |
| `Skinning/BoneTransforms.rsh` | `BoneTransform.ush`, `Skinning/SkinningDataDecode.ush`, `SkinningSceneDataDecode.ush` |
| `Skinning/SkinningCache.rsf` | `GpuSkinCacheComputeShader.usf`, `Skinning/CopyPreviousTransforms.usf` |
| `Skinning/ApplyMorphTargets.rsf` | `MorphTargets.usf` |
| `Skinning/RecomputeTangents.rsf` | `RecomputeTangentsCommon.ush`, `RecomputeTangentsPerTrianglePass.usf`, `RecomputeTangentsPerVertexPass.usf` |
| `Shared/SkinningLayout.h`·`SplineMeshLayout.h`·`WaterLayout.h` | `Shared/SkinningDefinitions.h`, `SplineMeshShaderParams.h`, `SingleLayerWaterDefinitions.h` |

### 11.2 기본 영역 안 UE 파일 가운데 Rocks 파일이 없는 것

각 영역 "들이지 않는 것"에서 사유를 적은 파일이다: `Public/ShaderVersion.ush`, `Public/OverloadMacros.ush`, `Public/FP16Math.ush`, `Public/LaneVectorization.ush`, `Public/DualPixelVectorization.ush`, `LargeWorldCoordinates.ush`, `LWCOperations.ush`, `MinMaterialTexCoords.ush`, `SplineMeshSceneTexture.usf`, `DynamicMeshBounds.usf`·`.ush`, `OcclusionQueryVertexShader.usf`·`PixelShader.usf`, `InstanceCulling/InstanceCullingOcclusionQuery.usf`, `SceneCulling/*`(3), `GenerateConservativeDepth.usf`, `AnisotropyPassShader.usf`, `FirstPersonSelfShadow.usf`, `ShadingModels.ush`(deprecated 평가 함수), `ShadingModelsSampling.ush`, `RectLightIntegrate.ush`·`CapsuleLightIntegrate.ush`(deprecated 적분), `ThinFilmBSDF.ush`, `SGGX.ush`, `LightFunctionVertexShader.usf`·`LightFunctionPixelShader.usf`, `DeferredLightVertexShaders.usf`, `ShadowProjectionVertexShader.usf`·`PixelShader.usf`, `ShadowFactorsUpsampleCommon.ush`, `CopyShadowMaps.usf`, `ScreenSpaceShadows.usf`, `bend_sss_gpu.ush`, `LateZDummyPS.usf`, `AmbientCubemapComposite.usf`, `DisplayMappingPixelShader.usf`, `WideCustomResolveShaders.usf`·`WideCustomResolve_*.ush`(3), `ScreenPixelShaderOES.usf`, `UpdateTextureShaders.usf`, `PostProcessVisualizeDOF.usf`·`VisualizeLocalExposure.usf`·`VisualizeLevelInstance.usf`, `GPUSceneDebugRender.usf`, `DebugProbes.usf`, `Tools/GPUDebugCrashUtils.usf`, `Skinning/AnimBankEval.usf`·`AnimSequenceTransformProvider.usf`·`TransformProviders.usf`·`ResolveAttachments.usf`·`SkinnedMeshDebug.usf`, `ACES/ACES_v1.3.ush`. 이 가운데 기능 자체가 후속인 것은 9.2절 후속 표에 있다.

### 11.3 후속 영역

A24–A35는 4.24절 표의 "Rocks 폴더" 열이 대응이다. 파일 단위 대응은 그 기능 스펙이 정한다.
