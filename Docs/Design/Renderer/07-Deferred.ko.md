# 렌더러(Renderer) 설계 — 7. Deferred

[목차](README.ko.md)

## 7. Deferred

### 7.1 GBuffer layout

**UE 5.8 근거.** `r.GBufferFormat` 기본 1(`R/SceneTextures.cpp:60–69`)의 배치(`RC/Private/GBufferInfo.cpp:247–553`). Substrate는 엔진 기본 꺼짐(`RU:2068–2072`)이고, 새 프로젝트는 Substrate를 켜되 일반 템플릿은 Blendable GBuffer(`r.Substrate.ProjectGBufferFormat=0`), 고급 템플릿은 Adaptive GBuffer로 만든다(`Editor/GameProjectGeneration/Private/GameProjectUtils.cpp:199–212`, 설명 `RU:2074–2083`). Blendable은 아래 배치를 그대로 쓰고 closure를 1개로 강제한다(`RU:2088, 2226–2233`). 배치를 바꾸는 것은 Adaptive 모드(`R32_UINT` 텍스처 배열, `RC/Private/GBufferInfo.cpp:265–283`, `R/Substrate/Substrate.cpp:646–660`)뿐이다. Substrate식 closure 체계는 계획 14.4절의 연구 gate이므로 Rocks는 Blendable과 같은 고정 배치로 시작한다.

| 타깃 | Rocks 형식(PixelFormat 5.2절) | 내용 | UE 5.8 | 조건 |
|---|---|---|---|---|
| MRT0 SceneColor | `RGBA16Float` | emissive·정적 간접광(base pass), 직접광 누적(조명 pass) | `PF_FloatRGBA`(`r.SceneColorFormat` 4, `Core/Private/HAL/ConsoleManager.cpp:4141–4150`) | 항상 |
| MRT1 GBufferA | `RGB10A2Unorm` | RGB = world 법선 `N·0.5+0.5`, A = 인스턴스 표시 2 bit | `:344–345, 363, 424–430`, 인코딩 `SH/DeferredShadingCommon.ush:137–147, 994–1005` | 항상 |
| MRT2 GBufferB | `RGBA8Unorm` | R metallic, G specular, B roughness, A = shading model 4 bit + 선택 출력 표시 4 bit | `:358, 364, 433–458`, 인코딩 `SH/DeferredShadingCommon.ush:295–309` | 항상 |
| MRT3 GBufferC | `RGBA8UnormSrgb` | RGB base color, A 머티리얼 AO | `:366–367, 464–467, 499–502` | 항상 |
| MRT4(아래 번호 규칙) GBufferD | `RGBA8Unorm` | shading model 전용 값(clear coat 등) | `:373, 536–541` | 고급 shading model 사용 profile(RS9) |
| MRT4–5(아래 번호 규칙) GBufferE | `RGBA8Unorm` | 정적 그림자 mask(Mixed 광원 4채널) | `:375–379, 517–524`, 조건 `EN/Private/ShaderCompiler/ShaderGenerationUtil.cpp:2307` | `StaticLighting::Baked` + shadow mask profile(RS7) |
| 깊이 | `D32FloatS8Uint` | reversed-Z 깊이 + 스텐실(shading model 분류·decal 수신·광원 채널) | `PF_DepthStencil`(PixelFormat 10절) | 항상 |
| velocity | `RG16Float`(별도 텍스처, MRT 아님) | 현재 표본 → 이전 위치, 출력 정규화(계획 12.2절) | `PF_G16R16`, OpenGL은 `R16G16_UINT`(`R/VelocityRendering.cpp:786–799`) | 이동 물체가 있을 때 |

- **D·E 번호.** D와 E는 켜진 것만 MRT4부터 차례로 붙는다(D만 → D=4, E만 → E=4, 둘 다 → D=4·E=5). GBuffer profile은 {기본, +D, +E, +D+E} 넷이다. UE 기본 layout은 D를 늘 4에, E를 5에 둔다(`RC/Private/GBufferInfo.cpp:369–379`). Rocks는 고급 shading model이 없는 장면에서 D를 만들지 않으려고 나눈다.
- **GBufferF(UE).** UE deferred는 tangent·이방성용 GBufferF를 데이터 기반 layout 밖에서 따로 만든다(`R/SceneTextures.cpp:791–799`). Rocks는 이방성(14.1절 `tangent`·`anisotropy`)을 후속으로 두므로 1차 layout에 넣지 않는다.
- **MRT 수.** 기본은 4개(MRT0–3)다. D·E를 쓰는 profile은 5–6개이므로 형식 정의 `renderTargets` ≥ 8인 형식에서만 켠다(13절, 결정 RN3·RN4). UE 모바일 deferred도 기본 4개다(`RC/Private/GBufferInfo.cpp:556–584`).
- **velocity를 MRT에 넣지 않는다.** UE 기본도 velocity를 depth pass에서 쓴다(`r.VelocityOutputPass` 0, `R/VelocityRendering.cpp:30–36`). Rocks도 F4에서 이동 물체만 velocity를 쓰고, 정지 물체의 카메라 이동분은 소비자가 깊이와 이전 view 행렬로 계산한다. UE가 정지 물체 velocity를 같은 방식으로 다루는지는 확인하지 못했다(**미확인**).
- **velocity 형식.** UE는 `RG16Unorm` 인코딩과 OpenGL용 정수 형식을 쓴다. Rocks는 모든 backend에서 `RG16Float` 하나다: `RG16Unorm`은 GLES에서 확장(`GL_EXT_texture_norm16`) 조건이고(PixelFormat 8.6절) `RG16Float`는 ES 3.2 core에서 렌더·필터가 된다(같은 표). 결정 RN6.
- **법선 인코딩.** UE desktop은 `N·0.5+0.5`를 10:10:10에 담고 octahedral은 모바일에서만 쓴다(`SH/DeferredShadingCommon.ush:137–147, 610–612, 721–724`). Rocks는 desktop과 같은 방식을 모든 backend에 쓴다(결정 RN2).
- **shading model 번호.** 4 bit(최대 16)다. UE도 16 이하를 정적 검사한다(`EN/Classes/Engine/EngineTypes.h:730`, `SH/ShadingCommon.ush:20–43`). 번호표는 14.2절.
- **장면 텍스처 묶음.** 위 타깃과 AO·HZB를 `SceneTextureSet`으로 묶어 공유 상수 slot `SceneTextures`로 셰이더에 준다(15절).

### 7.2 Deferred base pass

- 깊이 `Equal`·깊이 쓰기 없음(F4 prepass가 완전하므로). UE도 prepass가 완전하면 base pass 깊이를 읽기 전용으로 둔다(`DSR:2105–2114`, `R/RendererScene.cpp:4408–4419`).
- 머티리얼 `SurfaceAttributes`를 받아 GBuffer로 인코딩하는 함수는 렌더러가 소유한다(14절). emissive와 정적 간접광(라이트맵·프로브, 12.1절)은 MRT0에 바로 쓴다. UE도 base pass가 정적 간접광을 scene color에 쓴다(`SH/BasePassPixelShader.usf:554, 1359`).
- 스텐실: decal 수신 bit를 쓴다(GBuffer decal이 쓴다). 조명 pass는 스텐실을 쓰지 않는다(7.3절).

### 7.3 Deferred 조명 pass

**UE 5.8 방식.**

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 정렬 | 광원을 sort key(종류·IES·광원 함수·그림자·batch 가능·MegaLights)로 정렬하고 구간 경계를 계산 | `R/LightSceneInfo.h:80–166`, `R/LightRendering.cpp:1409, 1552–1622` |
| batch 가능 조건 | 그림자 없음·atlas 호환 광원 함수·directional 아님·contact shadow 없음 | `R/LightRendering.cpp:1484–1499` |
| 기본 경로 | batch 가능한 광원도 광원별 그리기, 그림자 광원은 screen shadow mask(`PF_B8G8R8A8`)를 채운 뒤 광원별 그리기 | `R/LightRendering.cpp:1699–1757, 1844, 1895–1908, 2354, 2436–2439` |
| 광원 기하 | directional은 전체 화면, point·rect는 구, spot은 원뿔. 카메라가 볼륨 안이면 컬 뒤집기, 선택 depth bounds | `R/LightRendering.cpp:2666–2679, 3031–3042, 3064–3093` |
| grid 기반 deferred | 있으나 기본 꺼짐·SM6·VSM 플랫폼 조건, "low utility and thus use"로 제거 예정 | `R/ClusteredDeferredShadingPass.cpp:36–65` |
| tiled deferred | 5.8에 없다(셰이더도 clustered만) | `R/` 전체 grep, `R/LightRendering.cpp:1487` 주석만 |
| MegaLights | RT 기반, 프로젝트 기본 꺼짐 | `R/MegaLights/MegaLights.cpp:26–31, 541–549, 2428`, `DSR:3472` |

**Rocks 방식(결정 RN1 권장안).** 전체 화면 pass 하나가 모든 직접광을 평가한다.

```text
DeferredLightingPass (전체 화면 PS, 렌더 타깃은 SceneColor 하나·가산 블렌드, 깊이 attachment 없음)
  depth = 깊이 SRV(픽셀); 원평면(0)이면 종료
  surface = DecodeGBuffer(픽셀); Unlit이면 종료
  for 각 directional 광원(목록, 최대 4):  EvaluateLight(surface, light, CSM 표본)
  cell = LightGridCell(픽셀 위치, view 깊이)
  for 각 local 광원 in cell:             EvaluateLight(surface, light, atlas 표본)
```

- `EvaluateLight`·`EvaluateDirectLighting`(BRDF)·그림자 표본 함수는 Forward+ base pass(8.3절)와 **같은 HLSL 함수**다. 계획 API02("동일 material/mesh의 Deferred/Forward 기준 영상")가 직접광에서 같은 결과를 내게 하는 장치다.
- **깊이·스텐실을 attachment로 붙이지 않는다.** 깊이를 SRV로 읽으면서 같은 텍스처를 attachment로 붙이면 GLES에서는 rendering feedback loop가 되고, 깊이와 스텐실을 동시에 읽을 수도 없다(PixelFormat 10절). 그래서 Unlit·하늘 픽셀은 스텐실 대신 셰이더 분기로 건너뛴다. D3D·Vulkan에서도 같은 셰이더를 쓴다.
- tiled compute deferred는 쓰지 않는다. compute로 SceneColor(`RGBA16Float`, GLES image 가능 형식)를 쓰는 것은 가능하지만, 타일 기반 GPU에서 전체 화면 PS가 대역폭에 유리하고 조명 함수를 Forward+ PS와 그대로 나눠 쓸 수 있기 때문이다. UE 5.8에도 tiled deferred가 없다.
- **광원별 pass를 두지 않는 이유와 위험.** UE 기본(광원별 pass)은 광원마다 GBuffer를 다시 읽는다. B2(광원 100개, 계획 29.2절)와 GLES 대역폭에서 불리하다고 보고 grid 한 번 순회를 고른다. 위험은 셰이더 레지스터 압력과 분기 발산이다. 측정 **MR1**(B2 장면에서 광원별 pass 시제품과 GPU 시간·대역폭 대조)로 확정한다. UE가 grid 기반 deferred를 지우는 이유("low utility")가 성능 때문인지는 원문에 없다(**미확인**).
- **batch되지 않는 광원.** 광원 함수(머티리얼)는 광원 함수 atlas(RS9)로 같은 pass에 넣는다. atlas 전에는 광원 함수가 있는 광원을 `Unsupported`로 보고하고 광원 함수 없이 그리지 않는다(지원표에 "광원 함수 미지원" 표시, 몰래 끄지 않는다).
- **SRV 예산.** 7.4절.

### 7.4 단계당 SRV 예산

**제약(결정 RN13, 2026-11-18 사용자 결정으로 개정).** 셰이더 원문은 모든 형식에 하나다. 단계당 SRV 텍스처(typed buffer 포함) 상한은 **형식 정의**(`sampledTextures`, 셰이더 시스템 12.1절)가 정한다. D3D12·Vulkan·Metal 형식은 넓게, GLES·D3D11 SM5 형식은 보장치 16으로 둔다. 렌더러 pass 셰이더는 GLES에서도 쓰므로 **렌더러 자원은 16 안**에 둔다(아래 표). 머티리얼 텍스처 상한은 (pass, 퍼뮤테이션, 형식)마다 `형식 상한 − 렌더러 합계`다. 그래서 같은 머티리얼이 데스크톱 형식에서는 쿠킹되고 GLES·D3D11 형식에서는 상한을 넘을 수 있으며, 그때는 **그 형식을 쿠킹할 때 명시 오류**다(UE 모바일 머티리얼의 샘플러 제약과 같은 성격, `Engine/Source/Developer/ShaderFormatOpenGL/Private/OpenGLShaderCompiler.cpp:868–877`). 이 검사는 셰이더 시스템 7.2절 4항의 프로필 공통 상한(형식들의 최소값)이 아니라 job 형식의 상한으로 한다(RQ22).

**샘플러.** 텍스처·샘플러 1:1 짝 규칙(셰이더 시스템 7.2절 9항)에서는 자기 샘플러를 쓰는 텍스처 수가 형식의 `samplers` 상한에 묶인다. 지금 형식 정의의 `samplers`는 모든 형식이 16이고, Metal은 장치 상한이 16이다(UE `ML_MaxSamplers = 16`, `Apple/MetalRHI/Private/MetalPipeline.h:47`). 그래서 넓은 형식이 실제로 16장을 넘으려면 짝 규칙의 예외인 **공유 샘플러**(UE `SSM_Wrap_WorldGroupSettings`·`SSM_Clamp_WorldGroupSettings`, `EN/Classes/Engine/EngineTypes.h:283–292`, 생성 `EN/Private/Materials/HLSLMaterialTranslator.cpp:7364–7388`)가 필요하다. 셰이더 시스템에서 후속이던 것을 요구로 올렸고(RQ23), 셰이더 시스템 개정 13이 SH2로 앞당겨 규칙을 정했다(셰이더 시스템 7.2절 9항: `SharedWrapSampler`·`SharedClampSampler`, 텍스처마다 출처 하나). 그 전에는 모든 형식에서 짝 규칙대로 `samplers − 렌더러 샘플러 수`가 실제 상한이다.

**줄이는 규칙.** grid의 셀 개수와 광원 index를 한 `R32Uint` 버퍼(앞부분 셀 머리, 뒷부분 목록)로 합친다(8.1절). CSM 단과 local 광원 그림자를 **shadow atlas 하나**에 둔다(9.2절). 광원 함수·IES는 atlas 하나(RS9). 정적 조명은 정책 퍼뮤테이션으로 라이트맵 또는 프로브 하나만 묶는다.

| 자원 | `DeferredLightingPass` | `ForwardOpaque`·`Translucent` |
|---|---|---|
| 깊이 | 1 | 0(contact shadow 퍼뮤테이션은 깊이 복사본 1) |
| GBuffer A·B·C(+D·E) | 3(+2) | 0 |
| AO | 1 | 1 |
| 광원 데이터 `RGBA32Float` | 1 | 1 |
| grid `R32Uint` | 1 | 1 |
| shadow atlas | 1 | 1 |
| 반사 capture cube array | 1 | 1 |
| BRDF LUT | 1 | 1 |
| 정적 조명(라이트맵 또는 프로브) | 0(base pass에서 처리) | 0–2 |
| 동적 GI cache | 0–1 | 0–1 |
| 광원 함수·IES atlas(RS9) | 0–1 | 0–1 |
| DBuffer(Forward+ decal, 퍼뮤테이션) | 0 | 0–3 |
| height fog·aerial perspective LUT | 0 | 0–1(Translucent) |
| **렌더러 합계** | 10–14 | 6–15 |
| 머티리얼 텍스처에 남는 수 | — | 형식 상한 − 렌더러 합계 |

- `GBuffer` pass의 렌더러 합계는 정적 조명(라이트맵 또는 프로브) 0–2뿐이고, depth 계열 pass(`Depth`·`DepthVelocity`·`DepthAux`·`ShadowDepth`·`Velocity`)는 0이다.
- 머티리얼 텍스처가 형식 상한을 넘으면 셰이더 워커의 형식별 상한 검사가 그 (머티리얼, pass, 퍼뮤테이션, 형식)의 쿠킹 오류를 낸다. 머티리얼 컴파일러는 번역 때 프로필 형식마다 이 값을 계산해 편집기·쿠커 진단에 보여 준다(머티리얼 시스템 5.4절 V9, RQ18).
- GLES·D3D11 형식에서는 선택 자원을 모두 켠 조합(합계 15)에 머티리얼 텍스처가 1개만 남고, 정적 조명 2·GI·atlas·DBuffer만 켜도(합계 13) 3개다. 이런 조합의 실제 영향은 머티리얼 시스템 측정 MM5로 본다. 데스크톱 형식은 형식 상한이 넓어 같은 조합에서도 남는다(공유 샘플러 전에는 위 샘플러 문단의 상한).
- bindless(셰이더 시스템 7.2절 10항)는 이 결정 밖의 후속이다.
