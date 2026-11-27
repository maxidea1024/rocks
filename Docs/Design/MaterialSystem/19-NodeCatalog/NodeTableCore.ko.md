# 머티리얼 시스템(MaterialSystem) 설계

## 19. 노드 목록 — 노드 표 1

값·수학·문맥 입력·분기·파라미터·텍스처 노드다. 규칙·상태·키는 [19장](README.ko.md)이다. 이 표는 생성 원본 `MaterialNodeCatalog.json`(19.2.2절)과 같아야 한다.

### 19.6.1 상수·상수형 문맥 값(17)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionConstant` | `Material.Constant` | 구현 | MS2a | — |  |
| `UMaterialExpressionConstant2Vector` | `Material.Constant2` | 구현 | MS2a | — |  |
| `UMaterialExpressionConstant3Vector` | `Material.Constant3` | 구현 | MS2a | — |  |
| `UMaterialExpressionConstant4Vector` | `Material.Constant4` | 구현 | MS2a | — |  |
| `UMaterialExpressionConstantDouble` | `Material.ConstantDouble` | 구현 | MS2a | — | Double1(LWC scalar, 5.3절). UE도 scalar double(MaterialExpressionGenericConstant.h:29–36) |
| `UMaterialExpressionStaticBool` | `Material.StaticBool` | 구현 | MS2b | — |  |
| `UMaterialExpressionTime` | `Material.Time` | 구현 | MS2b | `view` |  |
| `UMaterialExpressionDeltaTime` | `Material.DeltaTime` | 구현 | MS2b | `view` |  |
| `UMaterialExpressionViewProperty` | `Material.ViewProperty` | 구현 | MS2b | `view` |  |
| `UMaterialExpressionIsOrthographic` | `Material.IsOrthographic` | 구현 | MS2b | `view` |  |
| `UMaterialExpressionTwoSidedSign` | `Material.TwoSidedSign` | 구현 | MS2b | `vertex` |  |
| `UMaterialExpressionVertexColor` | `Material.VertexColor` | 구현 | MS2b | `vertex` |  |
| `UMaterialExpressionPerInstanceRandom` | `Material.PerInstanceRandom` | 구현 | MS2b | `primitive` |  |
| `UMaterialExpressionPerInstanceFadeAmount` | `Material.PerInstanceFadeAmount` | 구현 | MS2b | `primitive` |  |
| `UMaterialExpressionDistanceCullFade` | `Material.DistanceCullFade` | 구현 | MS2b | `primitive` |  |
| `UMaterialExpressionPrecomputedAOMask` | `Material.PrecomputedAoMask` | 구현 | MS2b | `lightmap` |  |
| `UMaterialExpressionIsFirstPerson` | `Material.IsFirstPerson` | 구현 | MS2b | `firstperson` |  |

### 19.6.2 수학(62)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionIfThenElse` | `Material.DynamicBranch` | 구현 | MS2a | — | 값 입력 Condition·True·False의 실행 중 분기(지연 평가, UE IR Em.Branch — MaterialExpressionsToMIR.cpp:1189–1222). deprecated인 것은 머티리얼 속성 bEnableExecWire다 |
| `UMaterialExpressionAbs` | `Material.Abs` | 구현 | MS2a | — |  |
| `UMaterialExpressionAdd` | `Material.Add` | 구현 | MS2a | — |  |
| `UMaterialExpressionSubtract` | `Material.Subtract` | 구현 | MS2a | — |  |
| `UMaterialExpressionMultiply` | `Material.Multiply` | 구현 | MS2a | — |  |
| `UMaterialExpressionDivide` | `Material.Divide` | 구현 | MS2a | — |  |
| `UMaterialExpressionMin` | `Material.Min` | 구현 | MS2a | — |  |
| `UMaterialExpressionMax` | `Material.Max` | 구현 | MS2a | — |  |
| `UMaterialExpressionClamp` | `Material.Clamp` | 구현 | MS2a | — |  |
| `UMaterialExpressionSaturate` | `Material.Saturate` | 구현 | MS2a | — |  |
| `UMaterialExpressionOneMinus` | `Material.OneMinus` | 구현 | MS2a | — |  |
| `UMaterialExpressionFloor` | `Material.Floor` | 구현 | MS2a | — |  |
| `UMaterialExpressionCeil` | `Material.Ceil` | 구현 | MS2a | — |  |
| `UMaterialExpressionRound` | `Material.Round` | 구현 | MS2a | — |  |
| `UMaterialExpressionTruncate` | `Material.Truncate` | 구현 | MS2a | — |  |
| `UMaterialExpressionFrac` | `Material.Frac` | 구현 | MS2a | — |  |
| `UMaterialExpressionFmod` | `Material.Fmod` | 구현 | MS2a | — |  |
| `UMaterialExpressionModulo` | `Material.Modulo` | 구현 | MS2a | — |  |
| `UMaterialExpressionSign` | `Material.Sign` | 구현 | MS2a | — |  |
| `UMaterialExpressionStep` | `Material.Step` | 구현 | MS2a | — |  |
| `UMaterialExpressionSmoothStep` | `Material.SmoothStep` | 구현 | MS2a | — |  |
| `UMaterialExpressionPower` | `Material.Power` | 구현 | MS2a | — |  |
| `UMaterialExpressionSquareRoot` | `Material.SquareRoot` | 구현 | MS2a | — |  |
| `UMaterialExpressionExponential` | `Material.Exponential` | 구현 | MS2a | — |  |
| `UMaterialExpressionExponential2` | `Material.Exponential2` | 구현 | MS2a | — |  |
| `UMaterialExpressionLogarithm` | `Material.Logarithm` | 구현 | MS2a | — |  |
| `UMaterialExpressionLogarithm2` | `Material.Logarithm2` | 구현 | MS2a | — |  |
| `UMaterialExpressionLogarithm10` | `Material.Logarithm10` | 구현 | MS2a | — |  |
| `UMaterialExpressionSine` | `Material.Sine` | 구현 | MS2a | — |  |
| `UMaterialExpressionCosine` | `Material.Cosine` | 구현 | MS2a | — |  |
| `UMaterialExpressionTangent` | `Material.Tangent` | 구현 | MS2a | — |  |
| `UMaterialExpressionArcsine` | `Material.Arcsine` | 구현 | MS2a | — |  |
| `UMaterialExpressionArccosine` | `Material.Arccosine` | 구현 | MS2a | — |  |
| `UMaterialExpressionArctangent` | `Material.Arctangent` | 구현 | MS2a | — |  |
| `UMaterialExpressionArctangent2` | `Material.Arctangent2` | 구현 | MS2a | — |  |
| `UMaterialExpressionArcsineFast` | `Material.ArcsineFast` | 구현 | MS2a | — |  |
| `UMaterialExpressionArccosineFast` | `Material.ArccosineFast` | 구현 | MS2a | — |  |
| `UMaterialExpressionArctangentFast` | `Material.ArctangentFast` | 구현 | MS2a | — |  |
| `UMaterialExpressionArctangent2Fast` | `Material.Arctangent2Fast` | 구현 | MS2a | — |  |
| `UMaterialExpressionLinearInterpolate` | `Material.LinearInterpolate` | 구현 | MS2a | — |  |
| `UMaterialExpressionInverseLinearInterpolate` | `Material.InverseLinearInterpolate` | 구현 | MS2a | — |  |
| `UMaterialExpressionDotProduct` | `Material.DotProduct` | 구현 | MS2a | — |  |
| `UMaterialExpressionCrossProduct` | `Material.CrossProduct` | 구현 | MS2a | — |  |
| `UMaterialExpressionNormalize` | `Material.Normalize` | 구현 | MS2a | — |  |
| `UMaterialExpressionLength` | `Material.Length` | 구현 | MS2a | — |  |
| `UMaterialExpressionDistance` | `Material.Distance` | 구현 | MS2a | — |  |
| `UMaterialExpressionAppendVector` | `Material.Append` | 구현 | MS2a | — |  |
| `UMaterialExpressionComponentMask` | `Material.ComponentMask` | 구현 | MS2a | — |  |
| `UMaterialExpressionIf` | `Material.If` | 구현 | MS2a | — |  |
| `UMaterialExpressionSwitch` | `Material.Switch` | 구현 | MS2a | — | 정수 선택값의 실행 중 선택(입력 개수는 속성) |
| `UMaterialExpressionFloatToUInt` | `Material.FloatToUInt` | 구현 | MS2a | — |  |
| `UMaterialExpressionUIntToFloat` | `Material.UIntToFloat` | 구현 | MS2a | — |  |
| `UMaterialExpressionConstantBiasScale` | `Material.ConstantBiasScale` | 구현 | MS2a | — |  |
| `UMaterialExpressionConvert` | `Material.Convert` | 구현 | MS2a | — | 여러 입력 성분을 여러 출력 성분으로 잇는 범용 변환. UE 메뉴 범주는 없지만 팔레트 제외 조건에는 걸리지 않는다 |
| `UMaterialExpressionOperator` | `Material.Operator` | 구현 | MS2a | — | UE IR 번역기 전용(Operator.h:112). 연산 전부(비트·shift·IsNan/IsInf·쌍곡·Transpose·행렬곱·Rsqrt 등 Operator.h:14–100)를 IR 연산으로 둔다(6.2절, 결정 MD10) |
| `UMaterialExpressionDDX` | `Material.Ddx` | 구현 | MS2b | — | 픽셀 전용(V4) |
| `UMaterialExpressionDDY` | `Material.Ddy` | 구현 | MS2b | — | 픽셀 전용(V4) |
| `UMaterialExpressionRotateAboutAxis` | `Material.RotateAboutAxis` | 구현 | MS2f | — |  |
| `UMaterialExpressionSphereMask` | `Material.SphereMask` | 구현 | MS2f | — |  |
| `UMaterialExpressionDeriveNormalZ` | `Material.DeriveNormalZ` | 구현 | MS2f | — |  |
| `UMaterialExpressionFresnel` | `Material.Fresnel` | 구현 | MS2f | `vertex` |  |
| `UMaterialExpressionBumpOffset` | `Material.BumpOffset` | 구현 | MS2f | `vertex` |  |

### 19.6.3 색(6)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionDesaturation` | `Material.Desaturation` | 구현 | MS2f | — |  |
| `UMaterialExpressionHsvToRgb` | `Material.HsvToRgb` | 구현 | MS2f | — |  |
| `UMaterialExpressionRgbToHsv` | `Material.RgbToHsv` | 구현 | MS2f | — |  |
| `UMaterialExpressionBlackBody` | `Material.BlackBody` | 구현 | MS2f | — |  |
| `UMaterialExpressionColorRamp` | `Material.ColorRamp` | 구현 | MS2f | `src:Curve` | UE와 같게: 구간별 선형식을 saturate해 더한다(보간 모드·접선 무시), key 둘이면 saturate 없는 lerp, Input이 연결되지 않으면 번역 때 커브 평가식(조정 포함) 상수 — UE 기존 번역기 기준(MaterialExpressions.cpp:4899–5014, 6.13절 MS2f4) |
| `UMaterialExpressionSRGBColorToWorkingColorSpace` | `Material.SrgbToWorkingColorSpace` | 구현 | MS2f | — | 작업 색공간이 sRGB/Rec.709 고정이면 항등(색공간 설정은 후속) |

### 19.6.4 좌표(13)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionTextureCoordinate` | `Material.TextureCoordinate` | 구현 | MS2b | `vertex` |  |
| `UMaterialExpressionWorldPosition` | `Material.WorldPosition` | 구현 | MS2b | `vertex` |  |
| `UMaterialExpressionLocalPosition` | `Material.LocalPosition` | 구현 | MS2b | `vertex` |  |
| `UMaterialExpressionScreenPosition` | `Material.ScreenPosition` | 구현 | MS2b | `view` |  |
| `UMaterialExpressionViewSize` | `Material.ViewSize` | 구현 | MS2b | `view` |  |
| `UMaterialExpressionSceneTexelSize` | `Material.SceneTexelSize` | 구현 | MS2b | `view` |  |
| `UMaterialExpressionPixelDepth` | `Material.PixelDepth` | 구현 | MS2b | `view` |  |
| `UMaterialExpressionPanner` | `Material.Panner` | 구현 | MS2b | `view` |  |
| `UMaterialExpressionRotator` | `Material.Rotator` | 구현 | MS2b | `view` |  |
| `UMaterialExpressionTruncateLWC` | `Material.TruncateWorldPosition` | 구현 | MS2b | — |  |
| `UMaterialExpressionLightmapUVs` | `Material.LightmapUvs` | 구현 | MS2b | `lightmap` |  |
| `UMaterialExpressionObjectRadius` | `Material.ObjectRadius` | 구현 | MS2b | `primitive` |  |
| `UMaterialExpressionMapARPassthroughCameraUV` | `Material.XrPassthroughCameraUv` | 구현 | MS2b | `xr` |  |

### 19.6.5 벡터·공간(18)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionCameraPositionWS` | `Material.CameraPosition` | 구현 | MS2b | `view` |  |
| `UMaterialExpressionCameraVectorWS` | `Material.CameraVector` | 구현 | MS2b | `view` |  |
| `UMaterialExpressionObjectPositionWS` | `Material.ObjectPosition` | 구현 | MS2b | `primitive` |  |
| `UMaterialExpressionActorPositionWS` | `Material.ActorPosition` | 구현 | MS2b | `primitive` |  |
| `UMaterialExpressionObjectOrientation` | `Material.ObjectOrientation` | 구현 | MS2b | `primitive` |  |
| `UMaterialExpressionObjectBounds` | `Material.ObjectBounds` | 구현 | MS2b | `primitive` |  |
| `UMaterialExpressionObjectLocalBounds` | `Material.ObjectLocalBounds` | 구현 | MS2b | `primitive` |  |
| `UMaterialExpressionBounds` | `Material.Bounds` | 구현 | MS2b | `primitive` |  |
| `UMaterialExpressionPreSkinnedLocalBounds` | `Material.PreSkinnedLocalBounds` | 구현 | MS2b | `skin` |  |
| `UMaterialExpressionPreSkinnedPosition` | `Material.PreSkinnedPosition` | 구현 | MS2b | `skin` |  |
| `UMaterialExpressionPreSkinnedNormal` | `Material.PreSkinnedNormal` | 구현 | MS2b | `skin` |  |
| `UMaterialExpressionVertexNormalWS` | `Material.VertexNormal` | 구현 | MS2b | `vertex` |  |
| `UMaterialExpressionVertexTangentWS` | `Material.VertexTangent` | 구현 | MS2b | `vertex` |  |
| `UMaterialExpressionPixelNormalWS` | `Material.PixelNormal` | 구현 | MS2b | `vertex` | 대표 노멀(19.3.3절)을 읽는다. 대표 노멀·Tangent 경로(BSDF Normal·Tangent 핀, ClearCoatNormal·Tangent 출력)에서 쓰면 순환 오류(UE HLSLMaterialTranslator.cpp:11784–11787과 같은 규칙) |
| `UMaterialExpressionReflectionVectorWS` | `Material.ReflectionVector` | 구현 | MS2b | `vertex` |  |
| `UMaterialExpressionLightVector` | `Material.LightVector` | 구현 | MS2b | `light` |  |
| `UMaterialExpressionTransform` | `Material.TransformVector` | 구현 | MS2b | `vertex` | 공간: tangent·local·world·view·camera·instance. UE의 particle 공간은 deprecated라 뺀다 |
| `UMaterialExpressionTransformPosition` | `Material.TransformPosition` | 구현 | MS2b | `vertex` | particle 공간 제외(위와 같음) |

### 19.6.6 임의 HLSL·인스턴스 데이터(3)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionCustom` | `Material.CustomHlsl` | 구현 | MS2f | — | 7.5절 규칙(V13) |
| `UMaterialExpressionPerInstanceCustomData` | `Material.PerInstanceCustomData` | 구현 | MS2b | `primitive` |  |
| `UMaterialExpressionPerInstanceCustomData3Vector` | `Material.PerInstanceCustomData3` | 구현 | MS2b | `primitive` |  |

### 19.6.7 보조(3)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionNamedRerouteDeclaration` | `Material.NamedRerouteDeclaration` | 구현 | MS2a | — | 이름 있는 재경유. 의미 비교·IR에서 선언 입력으로 접힘 |
| `UMaterialExpressionNamedRerouteUsage` | `Material.NamedRerouteUsage` | 구현 | MS2a | — | 선언 NodeId를 속성으로 가진다(이름 아님) |
| `UMaterialExpressionVertexInterpolator` | `Material.VertexInterpolator` | 구현 | MS2b | — |  |

### 19.6.8 분기·스위치(22)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionStaticSwitch` | `Material.StaticSwitch` | 구현 | MS2b | — |  |
| `UMaterialExpressionQualitySwitch` | `Material.QualitySwitch` | 구현 | MS2b | — |  |
| `UMaterialExpressionShadingPathSwitch` | `Material.RenderPathSwitch` | 구현 | MS2b | — | Deferred·ForwardPlus·Default(6.3절 3항, V15) |
| `UMaterialExpressionFeatureLevelSwitch` | `Material.CapabilityTierSwitch` | 구현 | MS2b | — | Mobile·SM5·SM6(MSR9). 품질 Low에서는 Mobile 입력(UE MaterialExpressions.cpp:9224–9229) |
| `UMaterialExpressionShaderStageSwitch` | `Material.StageSwitch` | 구현 | MS2b | — |  |
| `UMaterialExpressionDataDrivenShaderPlatformInfoSwitch` | `Material.FormatFactSwitch` | 구현 | MS2b | — | 형식 사실 조건. 형식 등급별 정적 해석(19.3.4절, UE도 플랫폼별 번역 때 — MaterialExpressions.cpp:9405–9416) |
| `UMaterialExpressionRequiredSamplersSwitch` | `Material.SamplerBudgetSwitch` | 구현 | MS2b | — | 형식 샘플러 상한 조건(MD4). 정적 해석(19.3.4절, MaterialExpressions.cpp:9784–9797) |
| `UMaterialExpressionBindlessSwitch` | `Material.BindlessFeatureSwitch` | 구현 | MS2b | — | 형식의 bindless 기능 조건, 정적 해석(19.3.4절). 지금 모든 형식 false |
| `UMaterialExpressionPreviousFrameSwitch` | `Material.PreviousFrameSwitch` | 구현 | MS2b | — | EvaluateMaterialVertexPrevious에서 PreviousFrame 입력 |
| `UMaterialExpressionReflectionCapturePassSwitch` | `Material.ReflectionCaptureSwitch` | 구현 | MS2b | — | 실행 중 view 값의 동적 분기(공급 view, UE View.RenderingReflectionCaptureMask — Common.ush:2306–2309) |
| `UMaterialExpressionShadowReplace` | `Material.ShadowPassSwitch` | 구현 | MS2b | — | pass 조건 값(19.3.4절) |
| `UMaterialExpressionRayTracingQualitySwitch` | `Material.RayTracingQualitySwitch` | 구현 | MS2b | — | pass 조건 값: ray tracing pass와 GI 표면 캐시 capture pass에서 RayTraced(UE Common.ush:2171–2177). 지금 그 pass가 없음 → Normal |
| `UMaterialExpressionPathTracingQualitySwitch` | `Material.PathTracingQualitySwitch` | 구현 | MS2b | — | pass 조건 값(path tracer 없음 → Normal) |
| `UMaterialExpressionPathTracingRayTypeSwitch` | `Material.PathTracingRayTypeSwitch` | 구현 | MS2b | — | pass 조건 값(path tracer 없음 → Main) |
| `UMaterialExpressionGIReplace` | `Material.GlobalIlluminationPassSwitch` | 구현 | MS2b | — | pass 조건 값(GI pass 정의는 Renderer) |
| `UMaterialExpressionLightmassReplace` | `Material.LightBakePassSwitch` | 구현 | MS2b | — | pass 조건 값(조명 굽기 도구가 생기면 그 pass) |
| `UMaterialExpressionMaterialProxyReplace` | `Material.ProxyBakePassSwitch` | 구현 | MS2b | — | pass 조건 값(HLOD·머티리얼 굽기 도구) |
| `UMaterialExpressionNaniteReplace` | `Material.VirtualizedGeometryPassSwitch` | 구현 | MS2b | — | pass 조건 값(가상화 geometry 경로 없음 → Default) |
| `UMaterialExpressionDistanceFieldsRenderingSwitch` | `Material.DistanceFieldFeatureSwitch` | 구현 | MS2b | — | 프로필 기능 조건, 정적 해석(19.3.4절 — 번역 키에 든다). distance field 없음 → No |
| `UMaterialExpressionVirtualTextureFeatureSwitch` | `Material.VirtualTextureFeatureSwitch` | 구현 | MS2b | — | 프로필 기능 조건, 정적 해석(19.3.4절). 가상 텍스처 없음 → No |
| `UMaterialExpressionRuntimeVirtualTextureReplace` | `Material.RuntimeVirtualTexturePassSwitch` | 구현 | MS2b | — | pass 조건 값(RVT 쓰기 pass) |
| `UMaterialExpressionMeshPaintTextureReplace` | `Material.MeshPaintTextureSwitch` | 구현 | MS2b | — | primitive별 실행 중 분기(공급 meshpaint, UE HLSLMaterialTranslator.cpp:9142–9144) |

### 19.6.9 파라미터(10)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionScalarParameter` | `Material.ScalarParameter` | 구현 | MS2c | — |  |
| `UMaterialExpressionVectorParameter` | `Material.VectorParameter` | 구현 | MS2c | — |  |
| `UMaterialExpressionDoubleVectorParameter` | `Material.DoubleVectorParameter` | 구현 | MS2c | — | Double4(LWC). uniform은 double의 high·low 두 slot, 셰이더는 타일+오프셋(UE MaterialUniformExpressions.cpp:1344–1365, HLSLMaterialTranslator.cpp:2539) |
| `UMaterialExpressionChannelMaskParameter` | `Material.ChannelMaskParameter` | 구현 | MS2c | — |  |
| `UMaterialExpressionStaticBoolParameter` | `Material.StaticBoolParameter` | 구현 | MS2c | — |  |
| `UMaterialExpressionStaticSwitchParameter` | `Material.StaticSwitchParameter` | 구현 | MS2c | — |  |
| `UMaterialExpressionStaticComponentMaskParameter` | `Material.StaticComponentMaskParameter` | 구현 | MS2c | — |  |
| `UMaterialExpressionCollectionParameter` | `Material.ParameterCollectionValue` | 구현 | MS2c | `src:Collection` |  |
| `UMaterialExpressionCollectionTransform` | `Material.ParameterCollectionTransform` | 구현 | MS2c | `src:Collection` |  |
| `UMaterialExpressionCurveAtlasRowParameter` | `Material.CurveAtlasRowParameter` | 구현 | MS2c | — | 커브 atlas 에셋(MS2c5 — 선행이던 커브 atlas 에셋은 CurveAssets CA6로 생겨 19.4.1절 표에서 지움) |

### 19.6.10 텍스처(24)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionTextureObject` | `Material.TextureObject` | 구현 | MS2c | — |  |
| `UMaterialExpressionTextureObjectParameter` | `Material.TextureObjectParameter` | 구현 | MS2c | — |  |
| `UMaterialExpressionTextureSample` | `Material.TextureSample` | 구현 | MS2c | — |  |
| `UMaterialExpressionTextureSampleParameter2D` | `Material.TextureSampleParameter2D` | 구현 | MS2c | — |  |
| `UMaterialExpressionTextureSampleParameter2DArray` | `Material.TextureSampleParameter2DArray` | 구현 | MS2c | — |  |
| `UMaterialExpressionTextureSampleParameterCube` | `Material.TextureSampleParameterCube` | 구현 | MS2c | — |  |
| `UMaterialExpressionTextureSampleParameterCubeArray` | `Material.TextureSampleParameterCubeArray` | 구현 | MS2c | — |  |
| `UMaterialExpressionTextureSampleParameterVolume` | `Material.TextureSampleParameterVolume` | 구현 | MS2c | — |  |
| `UMaterialExpressionTextureSampleParameterSubUV` | `Material.TextureSampleParameterSubUv` | 구현 | MS2c | `particle` |  |
| `UMaterialExpressionTextureProperty` | `Material.TextureProperty` | 구현 | MS2c | — | 크기·texel 크기를 상수 식으로 |
| `UMaterialExpressionAntialiasedTextureMask` | `Material.AntialiasedTextureMask` | 구현 | MS2c | — |  |
| `UMaterialExpressionRecordTextureStreamingInfo` | `Material.RecordTextureStreamingInfo` | 구현 | MS2c | — | TextureStreaming T4 sampling scale 기록(12.5절) |
| `UMaterialExpressionTextureCollection` | `Material.TextureCollection` | TODO | MS2c | `MPQ-TextureCollection` |  |
| `UMaterialExpressionTextureCollectionParameter` | `Material.TextureCollectionParameter` | TODO | MS2c | `MPQ-TextureCollection` |  |
| `UMaterialExpressionTextureObjectFromCollection` | `Material.TextureObjectFromCollection` | TODO | MS2c | `MPQ-TextureCollection` |  |
| `UMaterialExpressionParticleSubUV` | `Material.ParticleSubUv` | 구현 | MS2c | `particle` |  |
| `UMaterialExpressionSceneTexture` | `Material.SceneTexture` | 구현 | MS2c | `scene` |  |
| `UMaterialExpressionSceneColor` | `Material.SceneColor` | 구현 | MS2c | `scene` |  |
| `UMaterialExpressionUserSceneTexture` | `Material.UserSceneTexture` | 구현 | MS2c | `scene` |  |
| `UMaterialExpressionSparseVolumeTextureObject` | `Material.SparseVolumeTextureObject` | TODO | MS2c | `MPQ-SparseVolumeTexture` |  |
| `UMaterialExpressionSparseVolumeTextureObjectParameter` | `Material.SparseVolumeTextureObjectParameter` | TODO | MS2c | `MPQ-SparseVolumeTexture` |  |
| `UMaterialExpressionSparseVolumeTextureSample` | `Material.SparseVolumeTextureSample` | TODO | MS2c | `MPQ-SparseVolumeTexture` |  |
| `UMaterialExpressionSparseVolumeTextureSampleParameter` | `Material.SparseVolumeTextureSampleParameter` | TODO | MS2c | `MPQ-SparseVolumeTexture` |  |
| `UMaterialExpressionPathTracingBufferTexture` | `Material.PathTracingBufferTexture` | 구현 | MS2c | `pathtracing` |  |
