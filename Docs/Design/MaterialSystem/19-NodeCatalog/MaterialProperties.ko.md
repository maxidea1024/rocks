# 머티리얼 시스템(MaterialSystem) 설계

## 19. 노드 목록과 머티리얼 체계 대응 — 머티리얼 속성

규칙·상태는 [19장](README.ko.md)이다. 이 파일은 UE 머티리얼 에셋 속성을 Rocks 에셋 필드(4.2절)에 대응시킨다. 상태는 다음과 같다.

| 상태 | 뜻 |
|---|---|
| 필드 | 에셋 필드가 있고 번역·쿠킹이 쓴다(MS3) |
| 필드(효과는 Renderer) | 에셋 필드가 있고 쿠킹 데이터(렌더 상태·셰이더 특성)로 넘긴다. 그리는 효과는 비고의 Renderer 단계가 구현한다 |
| 필드 + TODO | 에셋 필드는 지금 둔다. 값을 쓰려면 비고의 선행(19.4.1절 MPQ)이 필요하고, 그 전에 기본값이 아닌 값을 넣으면 쿠킹 진단 `PrerequisiteMissing`이다 |
| 에디터 데이터 | EditorOnly 필드(쿠킹에서 빠짐) |
| 다른 곳 | 에셋 필드로 두지 않는다(비고의 이유) |
| 제외 | deprecated |

필드 이름은 camelCase이고 bool에 `b` 접두를 쓰지 않는다. `traits`는 `MaterialTraitsDesc`(4.2절)다. 렌더러 전용 묶음은 `traits.translucency`·`traits.forward`·`traits.postProcess` 하위 struct로 둔다.

### 19.9 `UMaterial` 속성(`RE/Public/Materials/Material.h`)

| UE `UMaterial` 속성 | 줄 | Rocks | 상태 | 비고 |
|---|---|---|---|---|
| `PhysMaterial` | 462 | `physicalMaterial : SoftAssetRef` | 필드 + TODO | MPQ-PhysicalMaterial(PhysicsModule 물리 재질 에셋) |
| `PhysMaterialMask` | 466 | `physicalMaterialMask : SoftAssetRef` | 필드 + TODO | MPQ-PhysicalMaterial |
| `RenderTracePhysicalMaterialOutputs` | 470 | `renderTracePhysicalMaterials` | 필드 + TODO | MPQ-PhysicalMaterial, RenderTrace 플러그인 출력 노드와 짝 |
| `MaterialDomain` | 481 | `traits.domain` | 필드 | Renderer 14.2 영역 번호표 |
| `BlendMode` | 485 | `traits.blendMode` | 필드 | TranslucentColoredTransmittance·AlphaHoldout은 TODO(19.4.1 MPQ-ColoredTransmittance·MPQ-AlphaHoldout) |
| `DecalBlendMode` | 488 | — | 제외 | deprecated |
| `MaterialDecalResponse` | 495 | `traits.decalResponse` | 필드(효과는 Renderer) | Renderer RS7 |
| `NaniteOverrideMaterial` | 499 | `virtualizedGeometryOverride` | 필드 + TODO | MPQ-VirtualizedGeometry |
| `DisplacementScaling` | 502 | `traits.displacementScaling` | 필드 + TODO | MPQ-VirtualizedGeometry(displacement를 쓰는 pass) |
| `DisplacementFadeRange` | 505 | `traits.displacementFadeRange` | 필드 + TODO | MPQ-VirtualizedGeometry |
| `ShadingModel` | 510 | — | 다른 곳 | 고전 단일 shading model 값. Rocks는 Surface 그래프가 정한다(MD1). UE 변환은 SurfaceFromShadingModel로 옮긴다 |
| `bCastDynamicShadowAsMasked` | 515 | `traits.castDynamicShadowAsMasked` | 필드(효과는 Renderer) | Renderer 그림자 pass |
| `ShadingModels` | 519 | — | 다른 곳 | 번역 결과(MATERIAL_SHADING_MODELS)로 계산 |
| `UsedShadingModels` | 524 | — | 다른 곳 | 에디터 표시용 계산값 |
| `OpacityMaskClipValue` | 535 | `traits.opacityMaskClipValue` | 필드 | MATERIAL_OPACITY_MASK_CLIP |
| `bEnableSeparateTranslucency_DEPRECATED` | 540 | — | 제외 | deprecated |
| `bEnableResponsiveAA` | 548 | `traits.responsiveAntialiasing` | 필드(효과는 Renderer) | Renderer TAA |
| `bScreenSpaceReflections` | 552 | `traits.screenSpaceReflections` | 필드(효과는 Renderer) | Renderer 반투명 SSR |
| `bContactShadows` | 556 | `traits.contactShadows` | 필드(효과는 Renderer) | Renderer 반투명 |
| `TwoSided` | 560 | `traits.twoSided` | 필드 | MATERIAL_TWO_SIDED |
| `bIsThinSurface` | 564 | `traits.thinSurface` | 필드 | Substrate thin 표면(10절) |
| `DitheredLODTransition` | 568 | `traits.ditheredLodTransition` | 필드(효과는 Renderer) | Renderer LOD dither |
| `DitherOpacityMask` | 572 | `traits.ditherOpacityMask` | 필드 | Masked + TAA dither |
| `bAllowNegativeEmissiveColor` | 576 | `traits.allowNegativeEmissive` | 필드 | 번역이 emissive를 max(0)로 자르지 않는다 |
| `bHasPixelAnimation` | 582 | `traits.hasPixelAnimation` | 필드(효과는 Renderer) | Renderer velocity |
| `bEnableTessellation` | 586 | `traits.enableTessellation` | 필드 + TODO | MPQ-VirtualizedGeometry |
| `bEnableDisplacementFade` | 590 | `traits.enableDisplacementFade` | 필드 + TODO | MPQ-VirtualizedGeometry |
| `TranslucencyPass` | 597 | `traits.translucencyPass` | 필드(효과는 Renderer) | Renderer RS5 |
| `TranslucencyLightingMode` | 601 | `traits.translucencyLightingMode` | 필드(효과는 Renderer) | Renderer RS5 |
| `bEnableMobileSeparateTranslucency` | 605 | `traits.mobileSeparateTranslucency` | 필드(효과는 Renderer) | Renderer RS5 |
| `NumCustomizedUVs` | 609 | `traits.customizedUvCount` | 필드 | MATERIAL_NUM_TEXCOORDS·보간 예산(V9) |
| `TranslucencyDirectionalLightingIntensity` | 616 | `traits.translucency.directionalLightingIntensity` | 필드(효과는 Renderer) | Renderer RS5 |
| `TranslucentShadowDensityScale` | 620 | `traits.translucency.shadowDensityScale` | 필드(효과는 Renderer) | Renderer RS5 |
| `TranslucentSelfShadowDensityScale` | 627 | `traits.translucency.selfShadowDensityScale` | 필드(효과는 Renderer) | Renderer RS5 |
| `TranslucentSelfShadowSecondDensityScale` | 631 | `traits.translucency.selfShadowSecondDensityScale` | 필드(효과는 Renderer) | Renderer RS5 |
| `TranslucentSelfShadowSecondOpacity` | 635 | `traits.translucency.selfShadowSecondOpacity` | 필드(효과는 Renderer) | Renderer RS5 |
| `TranslucentBackscatteringExponent` | 643 | `traits.translucency.backscatteringExponent` | 필드(효과는 Renderer) | Renderer RS5 |
| `TranslucentMultipleScatteringExtinction` | 650 | `traits.translucency.multipleScatteringExtinction` | 필드(효과는 Renderer) | Renderer RS5 |
| `TranslucentShadowStartOffset` | 654 | `traits.translucency.shadowStartOffset` | 필드(효과는 Renderer) | Renderer RS5 |
| `bDisableDepthTest` | 658 | `traits.disableDepthTest` | 필드(효과는 Renderer) | Renderer RS5 |
| `bWriteOnlyAlpha` | 662 | `traits.writeOnlyAlpha` | 필드(효과는 Renderer) | Renderer RS5 |
| `bGenerateSphericalParticleNormals` | 666 | `traits.sphericalParticleNormals` | 필드(효과는 Renderer) | 공급 particle |
| `bTangentSpaceNormal` | 673 | `traits.tangentSpaceNormal` | 필드 | 노멀 공간 해석(6.4 공간 분석) |
| `bUseEmissiveForDynamicAreaLighting` | 679 | `traits.emissiveForAreaLighting` | 필드(효과는 Renderer) | 조명 굽기·GI |
| `bUsedAsSpecialEngineMaterial` | 688 | `traits.engineDefaultMaterial` | 필드 | 기본 머티리얼(11.3) — 모든 사용 대상 쿠킹 |
| `bSubstrateRoughnessTracking` | 694 | `traits.roughnessTracking` | 필드 | 표면 접기(10절) |
| `bUsesDistortion` | 809 | `traits.usesDistortion` | 필드(효과는 Renderer) | Renderer 굴절 pass(RS5) |
| `bRootNodeOverridesDefaultDistortion_DEPRECATED` | 815 | — | 제외 | deprecated |
| `bUsedWithUI_DEPRECATED` | 917 | — | 제외 | deprecated |
| `bForceCompatibleWithLightFunctionAtlas` | 926 | `traits.lightFunctionAtlasCompatible` | 필드(효과는 Renderer) | Renderer light function |
| `bAutomaticallySetUsageInEditor` | 933 | — | 다른 곳 | 사용 대상은 선언 + 쿠킹 유도(MD6). 에디터 자동 설정은 하지 않는다 |
| `bFullyRough` | 937 | `traits.fullyRough` | 필드 | 번역이 거칠기 1로 고정 |
| `bUseFullPrecision_DEPRECATED` | 946 | — | 제외 | deprecated |
| `FloatPrecisionMode` | 959 | — | 다른 곳 | half 없음(셰이더 시스템 7.2절 5항) — 모든 형식 full |
| `bUseLightmapDirectionality` | 963 | `traits.lightmapDirectionality` | 필드(효과는 Renderer) | 조명 굽기 데이터(공급 lightmap) |
| `bUseAlphaToCoverage` | 967 | `traits.alphaToCoverage` | 필드(효과는 Renderer) | Renderer 렌더 상태 |
| `bForwardRenderUsePreintegratedGFForSimpleIBL` | 971 | `traits.forward.preintegratedSimpleIbl` | 필드(효과는 Renderer) | Renderer Forward+ |
| `bUseHQForwardReflections` | 977 | `traits.forward.highQualityReflections` | 필드(효과는 Renderer) | Renderer Forward+ |
| `bForwardBlendsSkyLightCubemaps` | 983 | `traits.forward.blendSkyLightCubemaps` | 필드(효과는 Renderer) | Renderer Forward+ |
| `bUsePlanarForwardReflections` | 987 | `traits.forward.planarReflections` | 필드(효과는 Renderer) | Renderer Forward+ |
| `bNormalCurvatureToRoughness` | 991 | `traits.normalCurvatureToRoughness` | 필드 | 번역(미분 사용, 픽셀) |
| `AllowTranslucentCustomDepthWrites` | 995 | `traits.translucentCustomDepthWrites` | 필드(효과는 Renderer) | Renderer custom depth |
| `bAllowFrontLayerTranslucency` | 999 | `traits.frontLayerTranslucency` | 필드(효과는 Renderer) | Renderer GI |
| `bCompatibleWithLumenCardSharing` | 1003 | `traits.surfaceCacheSharing` | 필드(효과는 Renderer) | GI 표면 캐시(이름은 기능 이름) |
| `Wireframe` | 1007 | `traits.wireframe` | 필드(효과는 Renderer) | Renderer 렌더 상태 |
| `bAllowTranslucentLocalLightShadow` | 1011 | `traits.translucency.localLightShadow` | 필드(효과는 Renderer) | Renderer RS5 |
| `TranslucentLocalLightShadowQuality` | 1015 | `traits.translucency.localLightShadowQuality` | 필드(효과는 Renderer) | Renderer RS5 |
| `TranslucentDirectionalLightShadowQuality` | 1019 | `traits.translucency.directionalLightShadowQuality` | 필드(효과는 Renderer) | Renderer RS5 |
| `ShadingRate` | 1027 | `traits.shadingRate` | 필드(효과는 Renderer) | Renderer VRS |
| `bAllowVariableRateShading` | 1031 | `traits.allowVariableRateShading` | 필드(효과는 Renderer) | Renderer VRS |
| `EditorX` | 1035 | — | 에디터 데이터 | 에디터 배치(GraphDocumentLayout) |
| `EditorY` | 1038 | — | 에디터 데이터 | 에디터 배치 |
| `EditorPitch` | 1041 | `preview.camera` | 에디터 데이터 | 미리보기 설정(EditorOnly) |
| `EditorYaw` | 1044 | `preview.camera` | 에디터 데이터 | 미리보기 설정(EditorOnly) |
| `bCanMaskedBeAssumedOpaque` | 1049 | `traits.maskedAssumedOpaque` | 필드 | 마스크 진입점 생략 판정 |
| `bIsMasked_DEPRECATED` | 1053 | — | 제외 | deprecated |
| `bIsPreviewMaterial` | 1057 | — | 다른 곳 | PreviewSession 임시 머티리얼 표시(11.4), 에셋 필드 아님 |
| `bIsFunctionPreviewMaterial` | 1061 | — | 다른 곳 | 같음 |
| `bUseMaterialAttributes` | 1065 | `traits.useMaterialAttributes` | 필드 | 출력 노드 Attributes 핀(5.6) |
| `bEnableExecWire` | 1069 | — | 제외 | deprecated(UE 5.8 Material.h:1068–1070) |
| `bEnableNewHLSLGenerator` | 1073 | — | 다른 곳 | UE 번역기 선택. Rocks 번역기는 하나(MD2) |
| `bCastRayTracedShadows` | 1077 | `traits.castRayTracedShadows` | 필드(효과는 Renderer) | ray tracing(M8) |
| `bUseTranslucencyVertexFog` | 1081 | `traits.translucencyVertexFog` | 필드(효과는 Renderer) | Renderer 안개 |
| `bApplyCloudFogging` | 1085 | `traits.applyCloudFogging` | 필드(효과는 Renderer) | Renderer 대기 |
| `bIsSky` | 1089 | `traits.isSky` | 필드(효과는 Renderer) | Renderer 하늘 |
| `bComputeFogPerPixel` | 1093 | `traits.fogPerPixel` | 필드(효과는 Renderer) | Renderer 안개 |
| `bOutputTranslucentVelocity` | 1097 | `traits.outputTranslucentVelocity` | 필드(효과는 Renderer) | Renderer velocity |
| `bIsTranslucencyVelocityFromDepth` | 1104 | `traits.translucencyVelocityFromDepth` | 필드(효과는 Renderer) | Renderer velocity |
| `bAllowDevelopmentShaderCompile` | 1108 | — | 다른 곳 | UE 에디터 셰이더 개발 옵션. Rocks는 미리보기 job 우선순위(MSR7) |
| `bIsMaterialEditorStatsMaterial` | 1112 | — | 다른 곳 | 에디터 통계용 임시 머티리얼 |
| `bRelaxRuntimeVirtualTextureRestrictions` | 1121 | `traits.relaxVirtualTextureRestrictions` | 필드 + TODO | MPQ-VirtualTexture |
| `BlendableLocation` | 1126 | `traits.postProcess.location` | 필드(효과는 Renderer) | PostProcess 영역 삽입 위치(계획 26절) |
| `BlendableOutputAlpha` | 1130 | `traits.postProcess.outputAlpha` | 필드(효과는 Renderer) | 같음 |
| `UserSceneTexture` | 1134 | `traits.postProcess.userSceneTexture` | 필드(효과는 Renderer) | 같음, 공급 scene |
| `UserTextureDivisor` | 1138 | `traits.postProcess.userTextureDivisor` | 필드(효과는 Renderer) | 같음 |
| `ResolutionRelativeToInput` | 1142 | `traits.postProcess.resolutionRelativeToInput` | 필드(효과는 Renderer) | 같음 |
| `bDisablePreExposureScale` | 1151 | `traits.postProcess.disablePreExposureScale` | 필드(효과는 Renderer) | 같음 |
| `bEnableStencilTest` | 1168 | `traits.postProcess.stencilTest` | 필드(효과는 Renderer) | 같음 |
| `StencilCompare` | 1171 | `traits.postProcess.stencilCompare` | 필드(효과는 Renderer) | 같음 |
| `StencilRefValue` | 1174 | `traits.postProcess.stencilRef` | 필드(효과는 Renderer) | 같음 |
| `NeuralProfileId` | 1180 | `neuralProfile : SoftAssetRef` | 필드 + TODO | MPQ-NeuralProfile |
| `RefractionMode_DEPRECATED` | 1183 | — | 제외 | deprecated |
| `RefractionMethod` | 1187 | `traits.refractionMethod` | 필드(효과는 Renderer) | Renderer RS5 |
| `RefractionCoverageMode` | 1191 | `traits.refractionCoverageMode` | 필드(효과는 Renderer) | Renderer RS5 |
| `PixelDepthOffsetMode` | 1195 | `traits.pixelDepthOffsetMode` | 필드 | 번역(PDO 방향 해석) |
| `BlendablePriority` | 1199 | `traits.postProcess.priority` | 필드(효과는 Renderer) | PostProcess |
| `bIsBlendable` | 1203 | `traits.postProcess.blendable` | 필드(효과는 Renderer) | PostProcess |
| `PreshaderGap` | 1207 | — | 다른 곳 | UE preshader 버퍼 내부 값(9.4 상수 식은 형식이 다름) |
| `RefractionDepthBias` | 1211 | `traits.refractionDepthBias` | 필드(효과는 Renderer) | Renderer RS5 |
| `MaxWorldPositionOffsetDisplacement` | 1219 | `traits.maxWorldPositionOffset` | 필드(효과는 Renderer) | Renderer bounds·culling |
| `bAlwaysEvaluateWorldPositionOffset` | 1226 | `traits.alwaysEvaluateWorldPositionOffset` | 필드(효과는 Renderer) | Renderer WPO 거리 비활성 |
| `StateId` | 1238 | — | 다른 곳 | Rocks는 semantic hash(5.5)·번역 키(12.2) |
| `bSavedCachedExpressionData_DEPRECATED` | 1273 | — | 제외 | deprecated |
| `ReferencedTextureGuids` | 1278 | — | 다른 곳 | 텍스처 참조는 Hard 참조 보고(4.4) |
| `EditorComments_DEPRECATED` | 2141 | — | 제외 | deprecated |
| `Expressions_DEPRECATED` | 2144 | — | 제외 | deprecated |
| `ParameterGroupData_DEPRECATED` | 2147 | — | 제외 | deprecated |
| 고전 입력 22개(DiffuseColor·SpecularColor·BaseColor·Metallic·Specular·Roughness·Anisotropy·Normal·Tangent·EmissiveColor·Opacity·OpacityMask·WorldPositionOffset·SubsurfaceColor·ClearCoat·ClearCoatRoughness·AmbientOcclusion·Refraction·MaterialAttributes·PixelDepthOffset·ShadingModelFromMaterialExpression·FrontMaterial(_DEPRECATED)) | 2150–2216 | — | 제외 | deprecated. 그래프 출력 노드·Surface로 옮겨졌다(UE도 같다) |

수치: 대응 117행 + 고전 입력 묶음 22 + 사용 플래그 28(19.10절) = 167(UE 리플렉션 속성 전부).

### 19.10 사용 플래그

UE는 정점 입력 타입마다 `bUsedWith*`를 둔다(`Material.h:703–911`, `EMaterialUsage` `RE/Public/Materials/MaterialInterface.h:76`). 에디터가 이 플래그를 자동으로 켠다(`bAutomaticallySetUsageInEditor`). Rocks는 선언 + 쿠킹 유도로 정한다(8.4절, 결정 MD6).
- 필드: `usage : VertexInputUsage`, 정점 입력 타입 이름의 집합.
- 아래 표의 대상 가운데 정점 입력 타입이 아직 없는 것은 TODO다. 그 정점 입력 타입이 생기면 이름을 등록하는 것으로 끝나고 머티리얼 쪽 형식은 바뀌지 않는다.

| UE 사용 플래그 | 줄 | Rocks 사용 대상(8.4절) | 상태 |
|---|---|---|---|
| `bUsedWithSkeletalMesh` | 702 | SkinnedMesh 정점 입력 | 구현(정점 입력 타입 있음 또는 MeshAnimationAssets MR4) |
| `bUsedWithMeshDeformer` | 710 | MeshDeformer 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithEditorCompositing` | 718 | 에디터 합성 pass | 다른 곳(에디터 pass) |
| `bUsedWithParticleSprites` | 726 | 파티클 sprite 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithBeamTrails` | 734 | 파티클 beam·trail 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithMeshParticles` | 742 | 메시 파티클 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithNiagaraSprites` | 750 | VFX sprite 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithNiagaraRibbons` | 754 | VFX ribbon 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithNiagaraMeshParticles` | 758 | VFX 메시 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithGeometryCache` | 762 | GeometryCache 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithStaticLighting` | 770 | 조명 굽기 데이터(lightmap) 변형 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithMorphTargets` | 778 | morph 특성(MeshAnimationAssets MR4) | 구현(정점 입력 타입 있음 또는 MeshAnimationAssets MR4) |
| `bUsedWithSplineMeshes` | 786 | SplineMesh 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithInstancedStaticMeshes` | 794 | Instanced 정적 메시 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithGeometryCollections` | 802 | GeometryCollection 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithClothing` | 823 | Cloth 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithWater` | 831 | 물 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithHairStrands` | 839 | Hair strand 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithLidarPointCloud` | 846 | 점 구름 정점 입력(플러그인) | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithVirtualHeightfieldMesh` | 854 | VHM 정점 입력(플러그인) | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithNanite` | 862 | 가상화 geometry | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithVoxels` | 870 | voxel 렌더링 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithCurves` | 878 | 곡선(hair card 등) 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithVolumetricCloud` | 886 | Volume 영역 cloud pass | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithHeterogeneousVolumes` | 894 | heterogeneous volume pass | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithInstancedSkinnedMesh` | 902 | Instanced skinned 정점 입력 | TODO(그 정점 입력 타입·pass가 생기면 등록) |
| `bUsedWithStaticMesh` | 910 | StaticMesh 정점 입력 | 구현(정점 입력 타입 있음 또는 MeshAnimationAssets MR4) |
| `bUsedWithNeuralNetworks` | 1161 | 신경망 후처리 pass | TODO(그 정점 입력 타입·pass가 생기면 등록) |

### 19.11 `UMaterialInterface`·`UMaterialInstance`

**`MaterialBase`(UE `UMaterialInterface`, `RE/Public/Materials/MaterialInterface.h:351–430`).**

| UE 속성 | 줄 | Rocks | 상태 | 비고 |
|---|---|---|---|---|
| `EditorOnlyData` | 360 | — | 다른 곳 | Rocks는 EditorOnly 필드 표지로 나눈다(AssetSystem 6.2절) |
| `SubsurfaceProfile`·`SubsurfaceProfiles` | 379·385 | `subsurfaceProfiles` | 필드 + TODO | MPQ-SubsurfaceProfile. 노드 속성에서 모아 번역이 채운다 |
| `SpecularProfiles` | 389 | `specularProfiles` | 필드 + TODO | MPQ-SpecularProfile |
| `ToonProfiles` | 393 | `toonProfiles` | 필드 + TODO | MPQ-ToonShading |
| `NeuralProfile` | 397 | `neuralProfile` | 필드 + TODO | MPQ-NeuralProfile |
| `LightmassSettings` | 410 | `lightBakeSettings` | 필드 + TODO | 조명 굽기 도구(19.12절) |
| `bTextureStreamingDataSorted`·`TextureStreamingDataVersion`·`TextureStreamingData` | 415–422 | `textureSamplingScales` | 필드 | 12.5절(TextureStreaming T4) |
| `ReferencedDefaultTextures` | 426 | — | 다른 곳 | 텍스처 Hard 참조 보고(4.4절) |
| `AssetUserData` | 430 | — | 다른 곳 | 에셋 공통 기능(AssetSystem) |

**`MaterialInstance`(UE `UMaterialInstance`, `RE/Public/Materials/MaterialInstance.h:638–860`).** 파라미터 override는 9.2절 키(`ParameterKey`)로 저장한다. UE의 이름 + 연관 + index와는 다르다.

| UE 속성 | Rocks | 상태 |
|---|---|---|
| `Parent` | `parent : ObjectRef<MaterialBase>`(Hard + Build) | 필드 |
| `ScalarParameterValues`·`VectorParameterValues`·`DoubleVectorParameterValues`·`TextureParameterValues` | `scalarOverrides`·`vectorOverrides`·`doubleVectorOverrides`·`textureOverrides` | 필드 |
| `ParameterCollectionParameterValues` | `collectionOverrides` | 필드 |
| `TextureCollectionParameterValues` | `textureCollectionOverrides` | 필드 + TODO(MPQ-TextureCollection) |
| `RuntimeVirtualTextureParameterValues` | `virtualTextureOverrides` | 필드 + TODO(MPQ-VirtualTexture) |
| `SparseVolumeTextureParameterValues` | `sparseVolumeTextureOverrides` | 필드 + TODO(MPQ-SparseVolumeTexture) |
| `FontParameterValues` | `fontOverrides` | 필드 + TODO(MPQ-Font) |
| `UserSceneTextureOverrides` | `userSceneTextureOverrides` | 필드(효과는 Renderer 후처리) |
| `EnumerationObjects` | — | 다른 곳(스칼라 파라미터의 열거 표시는 파라미터 노드 속성) |
| `StaticParametersRuntime`·`bHasStaticPermutationResource` | `staticOverrides`, static 변형 유무는 계산 | 필드 |
| `BasePropertyOverrides` | `traitOverrides : MaterialTraitOverrides`(아래) | 필드 |
| `PhysMaterial`·`bOverridePhysMaterial`, MIC `PhysMaterialMask` | `physicalMaterialOverride`·`physicalMaterialMaskOverride` | 필드 + TODO(MPQ-PhysicalMaterial) |
| `NaniteOverrideMaterial` | `virtualizedGeometryOverride` | 필드 + TODO(MPQ-VirtualizedGeometry) |
| `bOverrideSubsurfaceProfile`·`SpecularProfileOverride`·`ToonProfileOverride`와 bool들 | `profileOverrides` | 필드 + TODO(profile MPQ) |
| `bOverrideBlendableLocation`·`BlendableLocationOverride`·`bOverrideBlendablePriority`·`BlendablePriorityOverride` | `traitOverrides.postProcess` | 필드(효과는 Renderer) |
| `ReferencedTextureGuids`, MIC `ParameterStateId` | — | 다른 곳(Hard 참조 보고, 번역 키) |
| `bOverrideBaseProperties_DEPRECATED`·`StaticParameters_DEPRECATED`·`bSavedCachedData_DEPRECATED` | — | 제외 |

**base property override**(UE `FMaterialInstanceBasePropertyOverrides`, `MaterialInstanceBasePropertyOverrides.h:12–141`). 값 16개와 그 `bOverride_*` 16개다. Rocks는 `MaterialTraitOverrides`의 `Optional<값>` 필드 16개로 둔다. UE의 bool 짝을 Optional 하나로 합친 것이다.
- 대상 값: opacityMaskClipValue·blendMode·shadingModel(Rocks에서는 static 변형 → Surface preset 값)·ditheredLodTransition·castDynamicShadowAsMasked·twoSided·thinSurface·outputTranslucentVelocity·hasPixelAnimation·enableTessellation(TODO)·displacementScaling(TODO)·enableDisplacementFade(TODO)·displacementFadeRange(TODO)·maxWorldPositionOffset·surfaceCacheSharing·usage.
- 값을 덮으면 static 변형이 생긴다. 실행 중 인스턴스(DynamicMaterialInstance)는 덮을 수 없다(4.2절, `UnsupportedSetting`).

### 19.12 custom attribute·플랫폼 품질 설정·조명 굽기 설정

**custom attribute.** UE는 `FMaterialAttributeDefinitionMap::AddCustomAttribute`로 머티리얼 속성 집합에 속성을 더한다(`RE/Public/Materials/MaterialAttributeDefinitionMap.h:159`). Rocks는 MaterialSchema에 `RegisterMaterialAttribute`를 둔다(MS1a).
- 받는 것: 속성 Guid, 이름, 타입, 기본값, 빈도(정점·픽셀), module lease.
- 내장 속성(표면·출력 값)도 같은 표에 Guid로 등록한다. 값은 UE Guid와 같게 해서(`MaterialAttributeDefinitionMap.cpp:401–423`) UE 변환이 Guid로 잇게 한다.
- Make·Break·Get·Set·Blend Attributes와 레이어 함수가 이 Guid를 쓴다.

**플랫폼 품질 override.** UE `UShaderPlatformQualitySettings`·`FMaterialQualityOverrides`(`Engine/Source/Runtime/MaterialShaderQualitySettings/Classes/ShaderPlatformQualitySettings.h:31–78`)는 플랫폼·품질별로 번역 기능을 끈다(예: 모바일 저품질에서 lightmap·포인트 라이트). Rocks는 이것을 TargetProfile의 품질 수준별 번역 설정 `MaterialQualityOverrides`로 둔다.
- 번역 키에 든다.
- static 해석(19.2.1절)의 입력이다.
- 항목은 UE와 같다. 렌더러 기능에 걸린 항목은 그 기능이 생길 때 효과가 난다.

**조명 굽기 설정.** UE `FLightmassMaterialInterfaceSettings`(MaterialInterface `LightmassSettings`), `bUseLightmapDirectionality`, `bUsedWithStaticLighting`은 정적 조명 굽기 도구의 입력이다. Rocks는 `MaterialBase.lightBakeSettings` 필드로 두고 TODO로 둔다. 조명 굽기 도구 스펙이 생기면 그 도구가 읽는다.
