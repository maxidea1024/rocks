# 다층 표면(UE Substrate 전체 대응) 후속 계획

상태: 계획(개정 1, 2026-11-27). 구현은 아래 6절 선행이 끝난 뒤다. 담당은 렌더 세션(셰이더·렌더러)과 머티리얼 세션(그래프·번역기)이다.

## 1. 목적과 지금 범위

UE 5.8 Substrate의 기능 전부를 Rocks가 지원한다. 2026-11-27 사용자 지시는 "후속으로 계획은 잡아둬. 나중에라도 지원해야함"이다. 그래서 이 문서는 지금 범위를 넘어서는 부분을 **확정된 후속 범위**로 적는다. 연구 결과에 따라 넣을지 말지를 정하는 선택 항목으로 두지 않는다.

지금 범위는 머티리얼 시스템 결정 MD1의 권장안 (나)다(2026-11-18).

| 항목 | 지금(MD1 (나)) | 이 문서가 더하는 것(MD1 (다)) |
|---|---|---|
| 머티리얼 그래프 | Substrate식 BSDF 그래프(`SurfaceSlab`, shading preset 노드) | 같음. 모든 BSDF 노드와 연산을 연다 |
| 렌더러에 넘기는 값 | closure 하나로 접어 `SurfaceAttributes`(고전 GBuffer 칸 + shading model 번호). UE Blendable GBuffer와 같은 방식 | 화소마다 closure 여럿, 연산 트리 그대로 평가 |
| GBuffer 저장 | 고전 배치(Renderer 14.1절) | Adaptive 저장(top layer + material 배열)을 고를 수 있음 |
| 셰이더 BRDF | Substrate 단일 slab 평가(EngineShaders SLD3) | closure 여럿 평가, 층 사이 투과·감쇠 |

이에 따라 다음 문구를 "연구 gate"에서 "필수 후속"으로 읽는다.
- 머티리얼 시스템 SF4: 여러 closure 저장, glint, rough refraction.
- 계획 14.4절: Substrate 유사 임의 closure/layer 체계.

## 2. UE 5.8 조사 요약

근거 경로는 UE 5.8.4 로컬 클론 기준이다. 약자는 `SH` = `Engine/Shaders`, `RR` = `Engine/Source/Runtime/Renderer`, `RC` = `Engine/Source/Runtime/RenderCore`, `EN` = `Engine/Source/Runtime/Engine`이다.

**기본값.**
- 엔진 CVar `r.Substrate`는 기본 0이고 ReadOnly다(`RC/Private/RenderUtils.cpp:2069`).
- 새 프로젝트는 Substrate를 켜고 Blendable GBuffer(`r.Substrate.ProjectGBufferFormat=0`)를 쓴다. 고급 템플릿은 Adaptive다(머티리얼 시스템 10.2절).

**머티리얼 그래프.**
- BSDF 노드는 Slab, Hair, Eye, Single Layer Water, Unlit, Volumetric Fog Cloud, Light Function, PostProcess, UI, Toon(Experimental)이다.
- 연산은 Horizontal Blend, Vertical Layer, Add, Coverage Weight, Select다.
- 한도는 closure 8, 연산 15, 트리 깊이 48이다.
- 예산을 넘으면 연산을 깊이 순으로 parameter blending하고 slab을 단순화한다.
- 고전 shading model → Substrate 변환 노드가 있다.
- 출처는 머티리얼 시스템 10.2절에 있다.

**저장.**
- `r.Substrate.BytesPerPixel`(기본 80), `r.Substrate.ClosuresPerPixel`(기본 1, 플랫폼마다 설정, 상한 `SUBSTRATE_MAX_CLOSURE_COUNT`)을 쓴다. 그 밖에 `r.Substrate.ProjectClosuresPerPixel`, `r.Substrate.MaterialQualityLevelsClosuresPerPixel`, `r.Substrate.ProjectClosuresPerPixelOverride`가 있다(`RC/Private/RenderUtils.cpp:2093–2117`).
- Adaptive 저장은 top layer(노멀 22·거칠기 8·종류 2비트)와 material 배열이다(`RC/Private/GBufferInfo.cpp:265–328`, `SH/Private/Substrate/Substrate.ush`).

**타일 분류.**
- 타일 종류는 Simple, Single, Complex, ComplexSpecial, OpaqueRoughRefraction, OpaqueRoughRefractionSSSWithout, DecalSimple, DecalSingle, DecalComplex다(`EN/Public/Rendering/SubstrateMaterialShared.h:68–80`).
- 분류 셰이더는 `SH/Private/Substrate/SubstrateMaterialClassification.usf`·`SubstrateTile.usf`이고, 렌더러 쪽은 `RR/Private/Substrate/Substrate.cpp`다. 비동기 분류(`r.Substrate.AsyncClassification`), 타일 좌표 8비트, stencil 단계 같은 CVar가 있다(같은 파일 25–96행).

**조명.**
- 셰이더는 `SubstrateDeferredLighting.ush`, `SubstrateForwardLighting.ush`, `SubstrateMobileForwardLighting.ush`, `SubstrateLightingCommon.ush`, `SubstrateEvaluation.ush`다.
- 확률적 조명 CVar는 `r.Substrate.StochasticLighting`이고, 단순 타일 조명 pass는 `r.Substrate.SimpleTiledLightingPasses`다(`RC/Private/RenderUtils.cpp:2196–2202`).

**부가 기능.**
- DBuffer 데칼(`SubstrateDBuffer.usf`, `r.Substrate.DBufferPass`)
- 불투명 rough refraction(`SubstrateRoughRefraction.usf`, `RR/Private/Substrate/SubstrateRoughRefraction.cpp`, `r.Substrate.OpaqueMaterialRoughRefraction`)
- 표면 산란(`SubstrateSubsurface.ush`)
- glint(`SH/Private/Substrate/Glint/`, `r.Substrate.Glints*`). `GlintThirdParty.ush`는 제3자 코드다(`real-time_geometric_glint.tps`).
- specular profile(`EN/Classes/Engine/SpecularProfile.h`, `r.Substrate.SpecularProfile`)
- rough diffuse(`r.Substrate.RoughDiffuse`)
- 층 지원 스위치(`r.Substrate.EnableLayerSupport`)
- 셰이딩 품질(`r.Substrate.ShadingQuality`)
- 통계 연산(`SubstrateStatisticalOperators.ush`)
- Toon BSDF(`SubstrateToonBSDF.ush`)
- 시각화(`SubstrateVisualize.usf`, `RR/Private/Substrate/SubstrateVisualize.cpp`)

**다른 기능과의 연결.**
- Lumen: `RR/Private/Lumen/Lumen.cpp`, `LumenHardwareRayTracingCommon.cpp`, `LumenRadianceCache.cpp`, `LumenReSTIRGather.cpp`
- 레이 트레이싱: `SH/Private/RayTracing/RayTracingCommon.ush`, `RayTracingLightingCommon.ush`
- 패스 트레이싱
- 위 셋 모두 Substrate 데이터를 읽는다.

## 3. 목표 범위(전부 필수)

| ID | 영역 | 내용 | UE 대응 |
|---|---|---|---|
| LS-G1 | 그래프 | 모든 BSDF 노드. Slab 전체 입력(SSS MFP, SecondRoughness, Fuzz, Glint, Specular Profile, Haziness 포함), Hair, Eye, Single Layer Water, Unlit, Volumetric Fog Cloud, Light Function, PostProcess, UI | `MaterialExpressionSubstrate.h` BSDF 노드 |
| LS-G2 | 그래프 | 연산 다섯(Horizontal Blend, Vertical Layer, Add, Coverage Weight, Select)과 한도(closure 8, 연산 15, 깊이 48) | 같음 |
| LS-G3 | 번역기 | 예산을 넘었을 때 parameter blending 단순화. 화소당 closure 상한을 플랫폼·품질별로 적용 | `SubstrateTranslatorCommon.cpp` |
| LS-S1 | 저장 | Adaptive GBuffer: top layer와 가변 material 배열. Blendable은 지금 경로가 맡음 | `GBufferInfo.cpp`, `Substrate.ush` |
| LS-S2 | 저장 | 화소당 byte·closure 설정을 프로젝트·플랫폼·품질 등급별로. Rocks CVar와 기기 프로필로 연결 | `r.Substrate.BytesPerPixel`·`ClosuresPerPixel` 계열 |
| LS-R1 | 렌더러 | 타일 분류(Simple, Single, Complex, ComplexSpecial)와 타일별 조명 dispatch. 비동기 분류는 RG4 async compute 위에서 | `SubstrateMaterialClassification.usf`, `SubstrateTile.usf` |
| LS-R2 | 렌더러 | closure별 deferred 조명, Forward+ 조명, 모바일 forward 조명 | `SubstrateDeferredLighting.ush` 등 |
| LS-R3 | 렌더러 | 확률적 조명(closure 표본)과 단순 타일 조명 pass | `r.Substrate.StochasticLighting` |
| LS-R4 | 렌더러 | DBuffer 데칼 타일과 데칼 pass | `SubstrateDBuffer.usf` |
| LS-R5 | 렌더러 | 불투명 rough refraction과 표면 산란 연결 | `SubstrateRoughRefraction.*`, `SubstrateSubsurface.ush` |
| LS-F1 | 기능 | glint(LUT·level bias·min). 제3자 코드는 라이선스 확인 뒤 Rocks 구현으로 | `Glint/`, `r.Substrate.Glints*` |
| LS-F2 | 기능 | specular profile 에셋과 표본 | `SpecularProfile.h` |
| LS-F3 | 기능 | rough diffuse, haziness, fuzz, 셰이딩 품질 단계 | 해당 CVar |
| LS-F4 | 기능 | Toon BSDF(UE에서 Experimental) | `SubstrateToonBSDF.ush` |
| LS-V1 | 도구 | 시각화(closure 수, 타일 종류, byte 사용량, 층 분리 debug peel) | `SubstrateVisualize.*` |
| LS-X1 | 연결 | DynamicGI(Lumen 대응)와 레이 트레이싱, 패스 트레이싱이 closure 데이터를 읽기 | Lumen·RT 파일 |
| LS-X2 | 연결 | 반투명의 closure 여럿(forward 평가), TranslucentColoredTransmittance(dual source blending) | `EngineTypes.h:254` |

빼는 것은 UE가 deprecated나 제거 예정으로 표시한 경로뿐이다(Rocks 원칙 — UE 레거시 제외). 조사 시점에 Substrate 안에서 그런 표시가 있는 기능은 없다. 구현을 시작할 때 다시 확인한다.

## 4. 단계(제안)

| 단계 | 범위 | 선행 |
|---|---|---|
| LS0 | 설계 확정: 저장 형식(Adaptive byte 배치), 타일 종류, closure 상한 기본값, Rocks 이름(UE 이름은 대응표에만) | 6절 선행, 머티리얼 SF3 |
| LS1 | 그래프·번역기: LS-G1–G3. 출력은 아직 closure 하나로 접는 경로 | MS SF3 |
| LS2 | Adaptive 저장 + 타일 분류 + closure별 deferred 조명(LS-S1·S2·R1·R2 deferred) | Renderer RS2(Deferred)·RS9 |
| LS3 | Forward+·모바일 forward, 반투명 closure 여럿(LS-R2 나머지·X2) | Renderer RS1·RS5 |
| LS4 | 확률적 조명, DBuffer, rough refraction, 표면 산란(LS-R3–R5) | Renderer 데칼·SSS 단계 |
| LS5 | glint, specular profile, rough diffuse·haziness·fuzz, Toon(LS-F1–F4) | LS2 |
| LS6 | 시각화(LS-V1) | LS2 |
| LS7 | DynamicGI·RT·패스 트레이싱 연결(LS-X1) | 그 기능들의 단계(큰 기능 후속) |

순서는 계획 고정 순서를 따른다. 화면 마일스톤 뒤 렌더러 RS 단계와 머티리얼 SF1–SF3가 먼저다. 이 문서는 그 뒤에 이어지는 후속 단계를 확보해 두는 것이다.

## 5. 결정(미확정, 착수 때 정함 — 권장안)

| ID | 질문 | 권장안 |
|---|---|---|
| LSD1 | 지금 경로(Blendable 대응)를 유지할지 | 유지한다. UE처럼 프로젝트 설정으로 Blendable과 Adaptive를 고른다. 모바일과 낮은 등급은 Blendable이다(UE 모바일도 Blendable) |
| LSD2 | 화소당 closure 기본값 | UE와 같이 1. 플랫폼·품질 등급별로 덮어쓸 수 있게 한다 |
| LSD3 | Adaptive 저장 기능 등급 | UE가 요구하는 등급을 따른다. 머티리얼 SF4의 "SM6 형식만" 문구는 착수 때 UE 5.8 플랫폼 요구로 다시 확인한다 |
| LSD4 | glint 제3자 코드 | 원문을 복사하지 않는다. 논문을 근거로 Rocks 구현을 만든다. 근거 논문은 Deliot·Belcour, *Real-Time Rendering of Glinty Appearances using Distributed Binomial Laws on Anisotropic Grids*(HPG 2023)이고, 층 연산은 Belcour, *Efficient Rendering of Layered Materials using an Atomic Decomposition with Statistical Operators*(SIGGRAPH 2018)이다(EngineShaders 4.24절 A29). 라이선스는 착수 전에 확인한다 |
| LSD5 | Toon(Experimental) | 들인다. UE에서 Experimental이지 deprecated가 아니다. 표시만 실험 기능으로 둔다 |

## 6. 선행

- 머티리얼: SF1–SF3(그래프·번역기·표면 연산 parameter blending), MS 번역기 IR.
- 렌더러: RS0(공유 slot·View 상수), RS1(Forward+), RS2(Deferred·GBuffer), RS5(반투명), RS9(고급 shading model), 데칼·SSS 단계.
- 셰이더 시스템·엔진 셰이더: SL3(BRDF LUT·에너지 보존), A04 단일 slab 평가(완료), A29 Substrate 영역(EngineShaders 4.24절).
- RenderGraph: RG4 async compute(비동기 분류).
- 큰 기능 후속: DynamicGI·RT·패스 트레이싱(LS7만).

## 7. 다른 문서 개정 요청

- **머티리얼 시스템(머티리얼 세션 담당):** 결정 MD1 행과 SF4 행에 이 문서를 잇는다. SF4의 "계획 14.4절 연구 gate"를 "필수 후속 — LayeredSurface.ko.md"로 바꾼다.
- **계획 14.4절:** "별도 연구 gate에서 검증한다"를 "필수 후속으로 지원한다(LayeredSurface.ko.md). 비용 측정은 LS0에서 한다"로 바꿨다(이번 개정).
- **EngineShaders 4.24절 A29 행:** 이 문서를 잇는다(이번 개정).
- **Renderer:** GBuffer 절(14.1절)에 Adaptive 저장 후속을 잇는다. RS 단계가 진행될 때 렌더 세션이 함께 고친다.

## 8. 개정 기록

- 개정 1(2026-11-27): 사용자 지시("후속으로 계획은 잡아둬. 나중에라도 지원해야함")에 따라 처음 썼다. 내용은 UE 5.8 Substrate 조사 요약, 목표 범위 LS-G1–X2, 단계 LS0–LS7, 결정 LSD1–LSD5(미확정, 권장안), 선행, 다른 문서 개정 요청이다. 계획 14.4절 문구와 EngineShaders 4.24절 A29 행을 이 문서에 맞춰 고쳤다.
