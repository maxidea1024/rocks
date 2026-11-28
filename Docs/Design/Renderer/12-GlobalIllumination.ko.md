# 렌더러(Renderer) 설계 — 12. GI 두 축과 PathTracing 모드의 연결점

[목차](README.ko.md)

## 12. GI 두 축과 PathTracing 모드의 연결점

### 12.1 정적 축(`StaticLighting`)

**UE 5.8 근거.** 정적 조명은 base pass가 표본한다: 라이트맵 정책 선택(`R/BasePassRendering.cpp:2164–2255`, 종류 `R/LightMapRendering.h:347–366`), 상수(`EN/Public/LightmapUniformShaderParameters.h:14–25`), 셰이더 표본(`SH/BasePassPixelShader.usf:554–731`, volumetric lightmap `:572–603`, 2D 라이트맵 `:724, 731`), 정적 그림자 mask → GBufferE(`:1154`). Lumen diffuse GI가 켜진 view는 정적 간접광 배율을 0으로 둔다(`R/SceneRendering.cpp:2052–2058`, 주석 "this has the side effect of removing direct lighting from Static Lights"). Lumen 반사가 켜지면 반사 capture 배율도 0이다(`:2061–2067`).

**Rocks 연결점.**
- `StaticLighting::Baked`면 base pass(두 경로)가 `SampleStaticIndirect(surface)`로 라이트맵 또는 프로브를 표본해 SceneColor에 더한다. Deferred도 base pass에서 더한다(UE와 같다).
- 퍼뮤테이션: 정적 조명 정책 차원 {없음, 라이트맵, 프로브}(14.3절). 정책 선택은 인스턴스의 정적 조명 데이터 유무로 렌더러가 한다(UE `GetUniformLightMapPolicyType`와 같은 역할).
- 데이터 형식(라이트맵 계수·프로브 SH)과 bake는 계획 14.10절 bake 스펙이 정한다. 렌더러는 상수 slot `PrecomputedLighting`의 자리만 둔다.
- **이중 가산 금지.** 동적 GI가 켜진 view에서 정적 간접광을 쓰는 규칙(무시·움직이지 않는 객체만·혼합)은 방식별 지원표가 정한다(계획 14.3절). 렌더러는 view 상수에 **정적 간접 diffuse 배율·정적 반사 배율** 두 값을 두고 방식이 정한 값으로 채운다. UE의 배율 두 개와 같은 장치다.

### 12.2 동적 축(`DynamicGiMethod`)

**UE 5.8 근거.** 방식 선택은 view마다 한 번(`R/IndirectLightRendering.cpp:570–645`, 호출 `DSR:1403`)이고 선택 결과로 HZB 요구(furthest·closest)를 정한다(`DSR:1411–1433`). Lumen 진입점: 장면 갱신(`DSR:1967, 2895`), 장면 조명(`DSR:3042`, base pass 전), 간접광 계산·합성(`DSR:3430, 3519`, `R/IndirectLightRendering.cpp:1073–1262`), 투명 앞층(`DSR:3497, 3996`). 소비 자원: scene textures(GBuffer·깊이), HZB, velocity, distance field 또는 RT TLAS. 외부 GI plugin hook은 GBuffer A·B·C·깊이·SceneColor를 넘기는 delegate다(`R/DeferredShadingRenderer.h:225–255`, broadcast `R/IndirectLightRendering.cpp:1249–1262`). Lumen은 forward를 지원하지 않는다(`RU:2622`).

**Rocks 간접광 입력 계약.** 모든 동적 GI 방식(VoxelConeTracing·DynamicGI·RayTraced)은 아래 두 제품 가운데 하나 이상을 낸다. 두 경로가 같은 제품을 같은 함수로 소비한다(계획 14.3절 "모든 방식은 Forward+와 Deferred가 같은 GI 결과를 조명 입력으로 소비").

| 제품 | 뜻 | 생성 시점 | 소비(Forward+) | 소비(Deferred) |
|---|---|---|---|---|
| world-space 표본 함수 `SampleDynamicGiCache(surface)` | voxel·probe·radiance cache 등 world 공간 표현의 표본 | F8(base pass 전) | base pass 안(8.3절) | 조명 pass 안(7.3절과 같은 자리) |
| screen-space 결과 `IndirectDiffuse`·`IndirectSpecular` | 픽셀별 간접 diffuse·specular(denoise 뒤) | F12(현재 조명 뒤) | 보조 출력 색(8.4절)으로 합성 | GBuffer로 합성 |

- **방식이 받는 입력**(`IndirectLightingInputs`): 깊이·HZB(furthest·closest 선택), 법선·roughness(Deferred GBuffer A·B, Forward+ 보조 출력), velocity, 현재 SceneColor(screen trace), `RayScene`(RayTraced·하드웨어 trace), 장면 표현(SDF·voxel은 방식 소유).
- **방식 선택과 낮춤.** view마다 하나, 능력이 없으면 profile 순서로 낮추고 결과를 resolved profile에 남긴다(계획 14.3절). 렌더러는 선택 결과로 F5 HZB 종류·F4 보조 출력·F12 합성 pass 유무를 정한다(UE `DSR:1411–1433`과 같은 방식).
- **반사.** 반사 방식은 GI와 같은 두 축의 별도 선택이다(계획 14.3절). 1차 반사는 반사 capture + sky light(RS7), SSR은 screen-space 결과 제품의 specular로 들어간다(RS10).
- **AO.** SSAO·GTAO는 렌더러 공통 효과(RS7)이고 GI 방식이 자기 AO를 내면 view 선택으로 대체한다(UE 규칙 `R/IndirectLightRendering.cpp:608–627`과 같은 역할).

### 12.3 RayTraced와 PathTracing

- **`RayScene`.** BLAS·TLAS 갱신은 F2(GPU Scene) 뒤, base pass와 겹칠 수 있는 위치에 둔다. UE도 base pass와 async로 겹친다(`DSR:2983`, `R/RayTracing/RayTracing.cpp:1993`, `R/RayTracing/RayTracingScene.cpp:223, 506`). `RayScene` 자체는 계획 14.4절 별도 스펙이다. RT를 쓰는 효과가 하나도 없는 view에서는 만들지 않는다(UE `DSR:4405–4432`).
- **`DynamicGiMethod::RayTraced`**는 `RayScene`을 받아 screen-space 결과 제품을 낸다.
- **`RenderMode::PathTracing`.** view 단위 렌더 모드다.

| 항목 | Rocks | UE 5.8 근거 |
|---|---|---|
| 건너뛰는 것 | depth prepass 이후 F4–F14 전부(base pass·조명·투명·안개·하늘·화면 효과) | `DSR:2284–2288, 3058, 3482, 3735–3852`, `R/CompositionLighting/CompositionLighting.cpp:550, 604, 699` |
| 유지하는 것 | F1·F2·`RayScene`, `PostChain`(F15) | `DSR:4241`(후처리 입력) |
| 누적 | `ViewHistory`의 `PathTraceAccumulation`(radiance·분산·albedo·법선·깊이, 표본 수). 목표 표본 수까지 누적 | `R/PathTracing.cpp:906–965, 4257–4261, 4690` |
| 무효화 | 장면 변경·카메라 변경·설정 변경이 누적을 리셋 | `DSR:1119` |
| denoiser | 외부 공급자 interface(공간·시공간), albedo·법선 보조 입력 | `RPub/PathTracingDenoiser.h:20, 42, 112–118` |
| 요구 | 하드웨어 RT 또는 compute fallback(계획 14.3절). 없으면 `Unsupported`이고 Raster로 몰래 바꾸지 않는다 | — |

- PathTracing의 depth prepass·velocity 유지 여부는 UE에서 확인하지 못했다(**미확인**). Rocks는 화면 출력·피킹 일관성을 위해 깊이를 path tracer의 1차 hit 깊이로 채운다(설계, 후속 스펙에서 확정).
