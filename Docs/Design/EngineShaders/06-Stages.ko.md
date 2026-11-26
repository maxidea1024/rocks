# 엔진 셰이더 — 6. 단계표

[목차](README.ko.md)

## 6. 단계표

- 단계 ID는 SL이다(개정 1 초안 이름 "Shader Library"에서 온 접두로, 문서 이름을 바꾼 뒤에도 그대로 둔다). SL1–SL10은 RB 18.1절 화면 마일스톤과 RN 17.2절 RS 단계에 붙어 간다. 엔진 셰이더만 따로 앞서 가지 않는다(고정 순서·"화면 출력 앞당기기 제안 금지" 규칙). 예외는 SL0이다: 렌더러 없이 쓸 수 있는 순수 함수 라이브러리라 SS SH2와 함께 할 수 있다(7장).
- 각 단계의 완료 기준은 그 RS 단계의 완료 기준(RN 17.2절)에 이 장의 "엔진 셰이더 시험"(8장)을 더한 것이다.
- SL10은 서로 독립인 하위 단계 묶음이다. 각 하위 단계는 자기 RS 선행만 기다린다(예: RS13 디버그 표시는 RS0·RS6 뒤 바로 할 수 있다).
- RN RS11(GI·RT 연결점: `IndirectLightingInputs`·`RayScene` 자리·PathTracing 골격)에는 SL 단계가 없다. 연결점은 C++ 계약이고, 그 셰이더(DynamicGI·RT·PT)는 후속 영역(A27·A28)이기 때문이다. RS11이 만드는 셰이더 쪽 자리(간접광 입력 표본 함수 서명)는 A11 `Reflection/IndirectComposite.rsf`의 입력으로 받는다.

| 단계 | 붙는 단계 | 영역·파일 | 선행 | 엔진 셰이더 완료 기준(8장 시험 종류) |
|---|---|---|---|---|
| **SL0** | SS SH2와 함께(앞당김 가능) | A01에서 CS 전용 API(`ComputeDispatch`·`WaveOperations`)를 뺀 것, A02 1차(7.3절), A03 1차(색 공간·순응·전달 함수·filmic 곡선), A04 1차(BRDF 항·환경 BRDF 해석 근사·`SurfaceAttributes`·셰이딩 파라미터 골격), `Shared` 기반 셋, 시험 PS probe 셰이더(시험 루트). A18 전체 화면 VS는 RB 11.4절 2항이 먼저 넣었고 `TextureReadback.rsf`는 SL1로 옮겼다(개정 3, 결정 SLD21) | SS SH2(쿠킹된 셰이더·파이프라인·VS·PS 그리기 — compute는 아직 없다), A0-G(`RGBA32Float`·`R32Uint` 렌더 타깃·readback) | (C) 컴파일 행렬: 형식 정의 전부(SS SSD24 뒤 엔진 컴파일 프로필이 없으므로 `Engine/Config/ShaderFormats`의 형식 전부, 도구 없는 형식은 SKIP)에서 컴파일·워커 규칙 통과, (N) CPU 기준 구현 대조(PS probe), (G) 정수·비트 함수 golden, (S) 통계 시험(난수·표본, 2D 배치) |
| **SL1** | 화면 마일스톤(RB 18.1절) | A01 CS 전용 API(`ComputeDispatch.rsh`·`WaveOperations.rsh`)와 SL0 함수의 compute probe, A18 전체 화면·복사·clear·밉·표본 보조·자리표시, A20 `TextureInspector.rsf`, A03 LUT 좌표·SDR 출력 부호화·색 지도, A05 `BuiltinVertexInput.rsh`, A06 `DepthConversion.rsh` 일부 | SL0, RB6·RG1, GX3·GX4(dispatch). `TextureReadback.rsf`(SL0에서 옮김, 결정 SLD21) | (I) 다섯 차원·형식별 텍스처 표시 기준 영상(RB 18.1절 완료 기준과 같은 시험), (N) 밉 생성 = CPU 박스 필터(sRGB 포함), (C) 웨이브 경로·대체 경로 결과 동일(웨이브 경로는 지금 `D3D12_SM6`에서만 돈다) |
| **SL2** | RN RS0 | A05 머티리얼 틀·노드 라이브러리·로컬 메시(정적·동적), A06 view·장면 텍스처·GPU Scene·HZB 생성, A07 메시 공통·depth·velocity·Unlit base, A17 화면 pass 보조·수동 노출·최소 tone map, A20 GPU 메시지·print·버퍼 시각화, A02 `DualNumbers`·`ProceduralNoise`·`Barycentrics`, A18 버퍼 업로드, `Shared/SurfaceEnums.h`·`ViewLayout.h`·`GpuSceneLayout.h` | SL1, MS MS1–MS3·MS4 일부, 메시 에셋 | (I) RN RS0 (d) Unlit 기준 영상, (N) 깊이 복원·velocity 부호·HZB = CPU 기준, (G) GPU 메시지 왕복 |
| **SL3** | RN RS1 | A08 광원 기록·감쇠·light grid, A09 `ForwardLighting.rsh`, A04 환경 BRDF LUT 표본·에너지 보존(`EnergyConservation.rsh`)·직접광 평가(`DefaultLit`)·구 광원·에너지 표 생성, A11 cube 표본·sky light 상수, A21 scan·축소, A18 간접 인자, A20 조명 시각화 | SL2, GX2 typed buffer UAV, 내장 자원 팩의 BRDF LUT·에너지 표(SLR5) | (N) grid 셀 목록 ⊇ CPU 판정(RN RS1 (a)), (N) 단일 광원 조도 = CPU 해석해, (N) LUT 표본·백색 노 시험(다중 산란 보정 후), (C) scan·축소의 웨이브·대체 경로 결과 동일 |
| **SL4** | RN RS2 | A07 `GBufferEncoding.rsh`·`GBufferPass.rsf`, A09 `DeferredLightingPass.rsf` | SL3 | (N) GBuffer 인코딩 왕복 CPU 시험(RN RS2 (b)), (I) Deferred·Forward+ 영상 대조(RN RS2 (a)) |
| **SL5** | RN RS3 | A10 그림자 depth·atlas·CSM·PCF·표본 진입점 | SL3 | (N) PCF 커널 가중 합 1·비교 결과 = CPU, (I) 두 경로 그림자 영상(RN RS3 (a)) |
| **SL6** | RN RS4 | A06 HZB 가림·인스턴스 컬링·LOD dither, A05 인스턴스·spline 변형, A21 작업 분배·이진 탐색·정렬 | SL2 | (N) 가시 누락 0(RN RS4 (a)), (G) 정렬 결정성·안정성, (N) spline 변형 = CPU |
| **SL7** | RN RS5 | A15 투명·블렌드 모드·분리 투명·distortion, A16 대기 LUT·하늘·높이 안개, `Shared/EnvironmentFlags.h` | SL3, MS MS7 | (N) 대기 투과율·높이 안개 적분 = CPU 수치 적분, (I) 두 경로 fog·sky 영상(RN RS5 (c)) |
| **SL8** | RN RS6, EIP U0·U1 | A17 다운샘플·histogram·자동 노출·bloom·LUT·tone map·grain·FXAA·TAA·spatial upscale·후처리 머티리얼, A03 색 보정·ΔE, A07 `CustomDepth`(RN 단계는 SLR11), A18 Gaussian·깊이 다운샘플, A21 quad 교환 | SL2(SL7은 투명 합성 자리만) | (N) LUT 생성 = CPU LUT(전 격자점), (N) 노출 적응 = CPU 시퀀스(고정 dt), (I) EIP 12.3절 시퀀스(정지·회전·고속·resize·cut) |
| **SL9** | RN RS7 | A11 capture·필터·sky light capture·간접 합성·scene capture, A12 SSAO·GTAO, A13 decal, A14 라이트맵·볼륨 라이트맵, A02 `SphericalGaussian` | SL4·SL5, bake 스펙(A14) | (N) 사전 필터 cube = CPU 중요도 적분(허용 오차), SH 투영 = CPU, (I) 계획 LT02 |
| **SL10a** | RN RS8 | A18 `MsaaResolve.rsf` | SL3·SL8 | (N) RB 11.2절 resolve fixture(깊이·velocity·휘도 가중) |
| **SL10b** | RN RS9 | A23 전체, A04 `RectLightIntegration.rsh`, A08 IES·rect atlas·광원 함수 | SL4·SL5, MS MS8 | (N) LTC 적분 = CPU 수치 적분, (N) SSS 커널 정규화, (I) 모델별 Deferred·Forward 대조 |
| **SL10c** | RN RS10 | A11 SSR·planar, A10 contact shadow, A06 HZB 화면 추적, A07 `DepthAux` | SL4·SL8 | (N) HZB 추적 = CPU 광선 진행(합성 깊이), (I) 두 경로 screen-space 영상 |
| **SL10d** | RN RS12 | A22 전체 | SL5·SL8 | (N) CPU skinning 대조(RN RS12), (G) morph 정수 고정소수점 누적이 모든 형식(GLES 포함)·dispatch 순서에서 bit 일치 |
| **SL10e** | RN RS13 | A20 debug view·선 묶음·아이콘·편집 합성·hit proxy, A19 `SimpleElements.rsf` | SL2·SL8 | (I) RN RS13 (a)–(e) |
| **SLU** | UI 툴킷 UT2 | A19 전체, A03 `ColorVisionDeficiency.rsh` | SL1(SH2·GX4), UI UT1 | (I) UI UV6 offscreen 픽셀 비교, (N) SDF 글리프 가장자리 폭 = CPU |
| **후속** | 각 기능 스펙 | A24–A35, 각 영역의 "들이지 않는 것(지금)" | 기능별 | 그 스펙이 정한다 |

**SH2와 함께 하는 이유(SL0).** SH2는 쿠킹된 셰이더로 그리는 첫 단계이고([SS 16절](../ShaderSystem/16-LaterStages.ko.md)), 그 완료 기준은 샘플 렌더러 셰이더 몇 개다. SH2에는 compute dispatch가 없으므로(GX4) SL0은 VS·PS probe로 시험할 수 있는 것만 둔다. 공통 라이브러리 1차 범위를 SH2와 함께 컴파일·시험하면 (1) 워커 규칙 검사(SS 7.2절)·형식별 컴파일 경로가 다양한 원문으로 먼저 검증되고, (2) 렌더러 단계가 시작될 때 기초 함수가 이미 CPU 대조를 거친 상태가 된다. 렌더러 기능·자원 바인딩이 없으므로 고정 순서를 앞당기는 것이 아니다. 하지 않으면 SL2에서 A01–A04를 함께 한다(그때도 7장 범위·8장 시험은 같다).
