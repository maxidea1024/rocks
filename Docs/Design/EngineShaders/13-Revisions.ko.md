# 엔진 셰이더 — 13. 개정 기록

[목차](README.ko.md)

## 13. 개정 기록

- 개정 1(2026-11-23, 초안): 사용자 지시("UE `Engine/Shaders`에 있는 것 대부분을 Rocks도 구현")에 따라 UE 5.8.4 `Engine/Shaders` 1,227개를 전수 분류했다(영역 35, 제외 10종 67개, 12장). 같은 날 사용자 결정(큰 기능 후속, 기본 영역 중심)과 추가 지시(렌더링 필수 컴포넌트 → 셰이더 표, 5장)를 반영했다. 기본 영역 A01–A23의 Rocks 파일·단계(SL0–SL10·SLU)·근거 문헌, 공통 라이브러리 1차 범위(SL0 — SS SH2와 함께), 시험 방침(컴파일 행렬·CPU 기준 구현 대조·golden·통계·기준 영상), 결정 SLD1–SLD16(권장안 확정), 다른 문서 요구 SLR1–SLR10을 적었다. 조사 중 확인한 사실: UE 5.7부터 고전 shading model 함수가 deprecated(Substrate로 대체)라 BRDF 근거를 Substrate 단일 slab 평가로 잡았다(SLD3). UE 5.8 ACES 버전 기본값은 2.0이다(SLD7). `SimpleF32*` 셰이더는 참조가 없다(1.4절).
- 개정 1 교차 검증 1차 반영(2026-11-23, 리뷰어 1명). origin/feature/engine-runtime 병합 뒤(SS 개정 28 — 장 파일 폴더, CC 개정 7) 반영했다.
  - 이름: SS 12.2·12.3절의 "셰이더 라이브러리"는 쿠킹 산출물이므로 이 문서를 **엔진 셰이더**로 바꾸고 폴더를 `Docs/Design/ShaderLibrary/` → `Docs/Design/EngineShaders/`로 옮겼다. 시험 루트는 `/Plugin/EngineShaderTests`. ID 접두 SL·SLD·SLR은 그대로 둔다(README).
  - 웨이브 기능: 실제 형식 정의(`Engine/Config/ShaderFormats/*.json`)에서 `waveOperations`는 `D3D12_SM6`에만 있다. SLR1을 "SS 기존 `features` 어휘를 SC10 정의 `ROCKS_FEATURE_*`로 내보내기 + 새 어휘 `typedUavLoad`"로 고치고, 임시 표에서 Vulkan·Metal을 0으로 고쳤다(3.2·4.1·7.2절). 정수는 bit 일치, float 합은 허용 오차 안(SLD5).
  - SL0 범위: CS 전용 `ComputeDispatch`·`WaveOperations`를 SL1(GX3·GX4 뒤)로 옮겼다. 환경 BRDF LUT 표본·에너지 보존은 SL3로 옮기고 SL0에는 해석 근사만 둔다. `Shading/SurfaceAttributes.rsh`를 SL0에 더하고 SLR6에 MS 7.2절·RN 14.1절 반영을 더했다.
  - VS·PS UAV 금지(SS 7.2절, `uavStages` = `compute`): GPU assert·print·quad overdraw의 PS 쪽은 "디버그 전용 PS UAV" 기능(SLR12, 결정 SLD18)이 있을 때만 기록하고, 없는 형식(D3D11·GLES 포함)은 no-op. CS 경로는 그대로.
  - custom depth: RN에 pass 단계를 요구하는 SLR11을 더하고 CM1a(RS0)와의 단계 차이를 5.3절에 적었다. 거리 컬링은 "RS0 CPU V1(셰이더 없음), GPU는 SL6"으로 고쳤다.
  - morph 누적: 정수 고정소수점 원자 합으로 정했다(SLD17).
  - `LateZDummyPS.usf`를 모바일 제외에서 A10 "들이지 않는 것"으로 옮겼다(NVIDIA 그림자 stencil 우회용 — `ShadowRendering.cpp:1048–1056, 1221–1224`). 모바일 제외 17개, A10 17개, 제외 합계 66개, 기본 영역 417개. subpass 셋의 제외 사유를 RB GX13(후속)과 연결했다.
  - `r.Substrate` 엔진 기본값 0·새 프로젝트만 Substrate(Blendable/Adaptive) 사실을 1.2·4.4절에 적고, deprecated 목록에 `RayTracing/RayTracingSkyLightEvaluation.ush`를 더했다.
  - 컴포넌트 표: 충돌 형상(CM3)·Spline(CM5)·ZoneShape(CM30) 편집 표시, `InteractiveFoliageComponent`(CM18), `BudgetedSkeletalMeshComponent`(CM22)를 더하고, light shaft(A25)·거리장 그림자·capsule 그림자(A27)를 5.2절로 옮겨 `Unsupported` 진단을 적었다. RS11에 SL 단계가 없음을 6장에 적었다.
  - 허용 오차를 사양 기반 표(GLSL ES 3.20 4.7.1·Vulkan 정밀도 표·D3D11 규칙)로 바꿨다(8.3절). 결정성: GPU 메시지는 정렬 다중집합 비교, 노출 적응은 고정 dt, 큰 표본은 2D 배치(폭 ≤ 16,384).
  - 내부 불일치: `ShaderPrelude` include 목록(네 파일)과 A01 → A02 include 예외, `ShadingTestKernels` 위치(시험 루트), 11장의 `LightingDebugCounters`·`Occlusion` 파일, `BlockCompression*` 이름 통일, PixelFormat 이름(`RG11B10Ufloat`·`RGB9E5Ufloat`). SS·CC 절 인용을 장 파일 링크로 바꿨다.
- 개정 2(2026-11-25): RB 개정 46(18.4·18.5절) 반영. 화면 마일스톤의 HDR 표시가 tone map을 요구하므로 A03 `FilmicToneCurve.rsh`(SL0)와 A17 `TonemapPass.rsf`의 노출 배율 + filmic 곡선 부분을 SL1에서 먼저 넣는다. LUT·자동 노출·bloom·vignette·grain은 SL8 그대로다. 단계 순서는 바꾸지 않는다(SL0 몫을 마일스톤이 먼저 쓴다).
