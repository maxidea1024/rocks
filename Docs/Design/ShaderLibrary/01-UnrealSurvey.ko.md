# 셰이더 라이브러리 — 1. UE 구조 조사

[목차](README.ko.md)

## 1. UE 구조 조사

### 1.1 폴더와 파일 수

UE 5.8.4 `Engine/Shaders` 전체를 2026-11-23에 열거했다(`find -type f`). 파일 1,227개다.

| 루트 | 파일 | 확장자 | 성격 |
|---|---|---|---|
| `Public` | 22 | `.ush` 22 | 플랫폼 머리와 형식별 보충(`Public/Platform/{D3D,GL,Metal,Vulkan}`), 웨이브·FP16·bindless·루트 상수 |
| `Private` | 1,146 | `.usf` 579, `.ush` 545, `.tps` 22 | 엔트리 셰이더와 라이브러리. 하위 폴더 33개(아래 표) |
| `Shared` | 51 | `.h` 45, `.tps` 6 | C++와 HLSL이 함께 include하는 정의(번호표·구조체 배치·상수)와 벤더 확장 |
| `StandaloneRenderer` | 7 | `.hlsl` 3, `.glsl` 4 | RHI 없는 단독 Slate 렌더러용 원문 |
| (루트) | 1 | `.cs` | `Shaders.Build.cs`(UBT 모듈 규칙, 셰이더 아님) |

- `.usf`는 엔트리가 있는 파일, `.ush`는 include 전용, `.h`는 `Shared`에서만 쓰는 C++·HLSL 공용 헤더다. Rocks 대응은 `.rsf`·`.rsh`·`.h`(SS 결정 1, SC2)다.
- `.tps`(28개)는 UE가 원문에 넣은 서드파티 코드의 기록이다. 해당 셰이더: FXAA 3.11(NVIDIA), SMAA, ACES 1.3·2.0(Academy), Bend Studio 화면 공간 그림자, ShaderFastLibs(빠른 근사 수학), 색각 이상 모의(Machado 2009), 움직임 흐림 재구성 필터(McGuire 2012), 숫자 출력, Perlin·TEA·"Efficient computation of noise in GLSL" 잡음, 색 지도, 모바일 half GGX, 분리형 SSS, Oculus 왜곡, 광선–삼각형 교차 두 종, 실시간 glint, AMD AGS·Intel·NVAPI 확장. Rocks는 이 코드도 옮기지 않고 공개 문헌으로 직접 구현한다(결정 SLD8).

`Private` 하위 폴더(파일 수):

| 폴더 | 수 | 폴더 | 수 | 폴더 | 수 |
|---|---|---|---|---|---|
| ACES | 8 | HeterogeneousVolumes | 35 | RayTracing | 64 |
| Bloom | 11 | InstanceCulling | 7 | SMAA | 7 |
| DiaphragmDOF | 22 | Landscape | 6 | SSRT | 6 |
| DistanceField | 9 | LightFunctionAtlas | 2 | SceneCulling | 3 |
| Froxel | 2 | LocalFogVolumes | 3 | ScreenSpaceDenoise | 20 |
| GPUScene | 2 | Lumen(+Radiosity 5, SurfaceCache 3) | 95 | Skinning | 8 |
| HairStrands | 79 | MaterialCache | 9 | SparseVolumeTexture | 3 |
| MegaLights | 32 | MotionBlur | 10 | StochasticLighting | 5 |
| Nanite(+Voxel 6) | 58 | OIT | 1 | Substrate(+Glint 2) | 25 |
| PathTracing(+Light 8, Material 16, Utilities 2, Volume 14) | 66 | PostProcessing | 2 | TemporalSuperResolution | 23 |
| Tools | 11 | VariableRateShading | 6 | VirtualShadowMaps | 37 |

나머지 `Private` 최상위 파일은 469개다(엔트리·라이브러리 혼재).

### 1.2 구조의 특징

UE 셰이더 트리에서 Rocks 설계에 영향을 주는 구조는 아래와 같다(이름만 적는다).

| 구조 | UE | Rocks 대응 |
|---|---|---|
| 플랫폼 머리 | `Public/Platform.ush`가 형식별 `Public/Platform/<API>/<API>Common.ush`를 고르고 컴파일러 기능 매크로를 정한다 | `Public/Platform.rsh`(SH1a 구현). 형식 기능 정의는 SS SC10 정의 조립이 넣는다. 형식별 보충은 3.2절 |
| 공통 머리 | 거의 모든 셰이더가 `Private/Common.ush`를 먼저 include한다. 수학 상수·view 상수·장면 텍스처·보조 함수가 한 파일에 몰려 있다 | 한 파일에 몰지 않는다. `Common/ShaderPrelude.rsh`는 작은 기본 묶음만 include하고 view·장면 텍스처는 쓰는 pass가 따로 include한다(결정 SLD2) |
| 생성 include | `/Engine/Generated/Material.ush`(머티리얼 번역 결과), `/Engine/Generated/VertexFactory.ush`(정점 팩토리), `/Engine/Generated/GeneratedUniformBuffers.ush`(파라미터 구조체 HLSL) | MS 7.2절 `/Engine/Generated/Material.rsh`, RB 10.2절 `/Engine/Generated/VertexInput.rsh`, SS 13절 P7 생성 구조체 |
| 머티리얼 템플릿 | `MaterialTemplate.ush`(약 5,000줄)에 번역 결과를 텍스트 치환으로 넣는다 | 치환하지 않는다. 정적 `Material/MaterialCommon.rsh`와 생성 파일 계약(MS 7.2·7.3절) |
| 공용 정의 | `Shared/*.h`에 번호표·구조체 배치를 두고 C++와 HLSL이 같은 파일을 include한다(`HLSLTypeAliases.h`가 C++ 쪽 `float4` 등 별명을 준다) | 같다. `Shared/*.h`(3.3절). 번호표는 RN 결정 RN5 |
| 디버그 출력 | `ShaderPrint`(GPU에서 글자·선 그리기), `GPUMessaging`(GPU → CPU 메시지), `AssertionMacros` | `Debug/ShaderDebugPrint.rsh`·`Debug/GpuMessages.rsh`·`Common/ShaderAssert.rsh`(A20, SL2) |
| 모바일 렌더러 | `Mobile*` 파일과 `SceneDataMobile*`, subpass 확장이 별도 렌더러를 이룬다 | 렌더러 하나(RN 1절)이므로 대응 파일이 없다. GLES는 같은 원문을 형식 사실로 거른다(결정 SLD11) |
| Substrate 전환 | 5.7부터 고전 shading model 함수에 `UE_DEPRECATED 5.7 - Deprecated by Substrate` 표시가 붙었다(`SH/Private/ShadingModels.ush`·`DeferredShadingCommon.ush`·`DeferredLightingCommon.ush`·`RectLightIntegrate.ush`·`CapsuleLightIntegrate.ush`·`ShadingModelsSampling.ush`) | 고전 함수를 근거로 삼지 않는다. 단일 closure 평가(Substrate Blendable의 slab 하나)를 근거로 Rocks가 `SurfaceAttributes` 평가를 만든다(결정 SLD3, 4.4절) |
| 큰 월드 | `DoubleFloat.ush`(float 둘 확장 정밀도)와 `LargeWorldCoordinates.ush`(tile·offset)를 `WorldSpaceMath.ush`가 고른다 | 카메라 상대 float + 확장 정밀도 하나만(결정 SLD6) |

### 1.3 영역 분류 결과

전수 분류 스크립트로 모든 파일을 영역 하나에 넣었다(미분류 0). 영역별 UE 파일 목록은 12장, 영역 내용은 4장이다.

| 영역 | 파일 | 영역 | 파일 | 영역 | 파일 |
|---|---|---|---|---|---|
| A01 플랫폼·공통 | 26 | A13 데칼 | 5 | A25 볼륨 렌더링 | 60 |
| A02 수학·인코딩·난수 | 35 | A14 정적 조명 표본 | 4 | A26 Nanite | 60 |
| A03 색·전달 함수 | 17 | A15 반투명·굴절 | 8 | A27 Lumen·거리장 | 127 |
| A04 BRDF·셰이딩 모델 | 21 | A16 하늘 대기·높이 포그 | 6 | A28 RT·PT·디노이저 | 161 |
| A05 머티리얼·정점 입력 | 13 | A17 포스트 기본 | 28 | A29 Substrate | 31 |
| A06 장면·view·컬링 | 31 | A18 유틸리티 | 37 | A30 가상 그림자 맵 | 38 |
| A07 깊이·베이스 패스 | 15 | A19 2D·UI | 13 | A31 MegaLights | 33 |
| A08 광원 데이터·grid | 17 | A20 디버그·시각화 | 47 | A32 HairStrands | 83 |
| A09 직접·디퍼드 조명 | 5 | A21 컴퓨트 유틸 | 15 | A33 VT·MaterialCache·SVT | 26 |
| A10 그림자 | 16 | A22 스키닝·변형 | 17 | A34 지형·식생·물 | 15 |
| A11 반사·캡처·SSR | 19 | A23 고급 셰이딩 모델 | 19 | A35 XR·미디어·기타 | 25 |
| A12 AO | 2 | 제외(1.4절) | 67 | 합계 | 1,227 |

기본 영역(A01–A23)이 416개, 후속 영역(A24–A35)이 744개, 제외가 67개다. 후속 쪽 파일이 많은 것은 Lumen·RT·Nanite·Hair가 파일 수가 많은 대형 기능이기 때문이며, 기능 수로는 기본 영역이 UE 렌더러의 대부분이다.

### 1.4 제외 목록과 사유

| 종류 | 파일(수) | 사유 |
|---|---|---|
| 폐기·제거 예정 | `ClusteredDeferredShadingPixelShader.usf`·`ClusteredDeferredShadingVertexShader.usf`, `SSRT/SSRTDiffuseIndirect.usf`, `SimpleElementNormalMapPixelShader.usf`(4) | clustered deferred는 제거 예정(RN 머리말 `R/ClusteredDeferredShadingPass.cpp:36–46`), SSGI는 5.8부터 deprecated(RN 머리말), 노멀 맵 단순 요소는 파일 머리에 "Deprecated, do not use" |
| 미사용 | `SimpleF32PixelShader.ush`·`SimpleF32VertexShader.ush`(2) | `SRC/`·`SH/`·플러그인 어디서도 참조하지 않는다(`grep` 0건). SM3 시절 `sampler2D`·`tex2D` 원문 |
| Cascade 파티클 | `Particle*`(11), `MeshParticleVertexFactory`(1), `VectorField*`(2) — 14 | CC 결정 CD5·CD16(Cascade 제외, VFX는 Niagara 구조의 `EffectComponent`). Niagara 셰이더는 플러그인 폴더라 범위 밖(1.5절) |
| UE 모바일 렌더러 전용 | `Mobile*`(9), `SceneDataMobileLoader`·`Writer`, `PostProcessMobile`, `SSProfilePreIntegratedMobile`, `OcclusionFeedbackShaders`, `LateZDummyPS`, `Public/Platform/{GL,Metal,Vulkan}/*SubpassSupport`(3) — 18 | 폐기된 것은 아니다. Rocks는 모바일 전용 렌더러를 두지 않고 GLES도 같은 렌더러·원문을 쓴다(RN 1절, 결정 SLD11). 모바일 GGX half 근사(`MobileGGX`)는 GLES 품질 등급을 정할 때 참고만 한다(9장) |
| 콘솔 전용 | `HTileEncoding.ush`, `RTWriteMaskDecode.usf`(2) | 깊이·색 압축 메타데이터를 직접 읽는 기능으로, 그것을 켜는 플랫폼 정의(`PLATFORM_SUPPORTS_HTILE_LOOKUP` 등)가 공개 데스크톱·모바일 형식에 없다. 콘솔은 Rocks 후속(EIP 2.2절) |
| 벤더 확장 | `Public/NvidiaExtensionsCommon.ush`, `Shared/ThirdParty/{AMD,Intel,NVIDIA}/*`(12) | 벤더 SDK 헤더와 라이선스가 따른다. Rocks는 SM 6 표준 웨이브 연산으로 같은 일을 한다(결정 SLD13) |
| 단독 렌더러 | `StandaloneRenderer/*`(7) | RHI 없는 도구용 Slate 렌더러의 D3D·데스크톱 GL 원문이다. Rocks는 HLSL 하나·쿠킹 경로뿐이고(SS 1절) 데스크톱 GL을 제외했다 |
| Chaos 물리 필드 | `PhysicsField*`(6) | Rocks 물리는 Jolt다(EIP 16절). 물리 필드를 들일 때 별도 설계 |
| Bink | `Bink.usf`(1) | 상용 영상 코덱(RAD) 전용. 미디어 재생은 A35 후속 |
| 빌드 규칙 | `Shaders.Build.cs`(1) | 셰이더가 아니다. Rocks 셰이더 트리는 SS SC2 mount로 들어간다 |

UE 5.8에서 deprecated 표시가 파일 일부에만 있는 경우(예: `HeightFogCommon.ush`의 `ComputeVolumeUV_DEPRECATED`, `MaterialTemplate.ush`의 옛 노드 함수, 위 Substrate 전환 함수)는 파일을 제외하지 않고, 그 함수만 근거에서 뺀다.

### 1.5 범위 밖

- **엔진 플러그인 셰이더.** `Engine/Plugins/*/Shaders`에 Niagara 141, TextureGraph 86, NNERuntimeRDG 30, HairStrands 29, PCG 28, DeformerGraph 22, nDisplay 18, GPULightmass 14, Water 11 등 40여 플러그인의 셰이더가 있다. 플러그인은 CC 9절과 각 기능 문서가 들일 때 정한다. Niagara(VFX)는 사용자 결정으로 후속이다.
- **플랫폼 폴더.** `Engine/Platforms`에는 Android·IOS·VisionOS·Windows만 있고 `Shaders` 하위 폴더가 없다(이 클론에는 콘솔 플랫폼 폴더가 없다). 콘솔 셰이더는 공개 클론에 없으므로 조사 대상이 아니다.
- **셰이더 시스템 자체.** 전처리·컴파일·퍼뮤테이션·바인딩 규약은 SS가 정한다. 이 문서는 그 위에 올라가는 HLSL 원문의 구성만 정한다.
