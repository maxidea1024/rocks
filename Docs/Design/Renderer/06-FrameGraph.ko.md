# 렌더러(Renderer) 설계 — 6. 공통 frame graph

[목차](README.ko.md)

## 6. 공통 frame graph

### 6.1 UE 5.8 순서(`FDeferredShadingSceneRenderer::Render`)

| 순서 | 단계 | 근거 | forward에서 |
|---|---|---|---|
| 1 | 장면 갱신·가시성 task 시작 | `DSR:1893`, `R/SceneRendering.cpp:4198, 4363, 4378` | 같다 |
| 2 | GPU Scene 업로드 | `DSR:2234–2255` | 같다 |
| 3 | depth prepass(+ velocity) | `DSR:2450–2504, 2693` | 전체 prepass 강제(`RU:681–700`) |
| 4 | occlusion query·HZB | `DSR:2712–2786` | 같다 |
| 5 | light grid | `DSR:2808` | 같다 |
| 6 | 그림자 depth | forward: base pass 전 `DSR:2922–2927`, deferred: base pass 뒤 `DSR:3292` | 앞당김 |
| 7 | forward 그림자 mask·볼륨 안개 | `DSR:2941–2944` | forward만 |
| 8 | DBuffer decal·forward AO | `DSR:2962`, `R/CompositionLighting/CompositionLighting.cpp:548–593` | AO는 깊이만으로 base pass 전 |
| 9 | base pass | `DSR:3060` | 조명까지 평가(`SH/BasePassPixelShader.usf:203–206, 1366, 1480`) |
| 10 | velocity(필요 시 별도) | `DSR:3373–3376`, `R/VelocityRendering.cpp:30–36` | 같다 |
| 11 | AO·decal(base pass 뒤) | `DSR:3392` | 없음 |
| 12 | 간접광·AO 합성, 직접광 | `DSR:3419–3480`(`RenderLights` 3468) | 건너뜀(`RU:1687–1696`) |
| 13 | 반사·sky 합성 | `DSR:3510–3529` | 건너뜀 |
| 14 | 볼륨 안개·구름 | `DSR:3662–3686` | 7에서 |
| 15 | sky atmosphere·height fog | `DSR:3785, 3823` | sky pass 건너뜀(`DSR:3782`), fog는 그린다 |
| 16 | 투명·distortion | `DSR:4012, 4026` | 같다 |
| 17 | path tracing | `DSR:4108–4119` | 같다 |
| 18 | 후처리 | `DSR:4312` | 같다 |

### 6.2 Rocks 순서

계획 11.4절의 공통 전반부·경로별 중반·공통 후반부를 pass로 풀었다. "공통"은 두 경로가 같은 pass·셰이더를 쓴다는 뜻이다.

| # | pass | Forward+ | Deferred | 출력 | 단계 |
|---|---|---|---|---|---|
| F1 | 장면 변경 적용·가시성 V1 | 공통 | 공통 | 가시 목록 | RS0 |
| F2 | GPU Scene 업로드 | 공통 | 공통 | 인스턴스 버퍼 | RS0 |
| F3 | sky LUT(대기) | 공통 | 공통 | LUT 텍스처 | RS5 |
| F4 | depth prepass(+ 이동 물체 velocity) | 공통(MSAA면 MSAA 깊이) | 공통 | 깊이·velocity·(선택) 보조 법선 | RS0 |
| F5 | HZB(V3)·재검사(V4) | 공통 | 공통 | HZB furthest·closest | RS0·RS4 |
| F6 | light grid | 공통 | 공통 | grid 버퍼 | RS1 |
| F7 | 그림자 depth(CSM 단·local 광원, atlas 하나) | 공통 | 공통 | shadow atlas | RS3 |
| F8 | AO·world-space GI 준비 | AO(깊이·보조 법선)와 GI cache 준비 | GI cache 준비(F11 전이면 되므로 F9 뒤로 옮겨도 된다). AO는 F10 | AO·GI cache | RS7·RS11 |
| F8a | (Forward+) DBuffer decal | 깊이 위에 decal을 DBuffer로 그리고 base pass가 읽는다 | — | DBuffer | RS7 |
| F9 | base pass | 조명 평가 → SceneColor(+ 보조 출력) | GBuffer 쓰기 + emissive·정적 간접광 → SceneColor | — | RS1·RS2 |
| F10 | (Deferred) GBuffer decal·AO | — | GBuffer에 decal 블렌드, AO | — | RS7 |
| F11 | (Deferred) 조명 pass | — | `DeferredLightingPass` | SceneColor | RS2 |
| F12 | 간접광 합성(반사·sky·GI screen-space) | 보조 출력 기반 | GBuffer 기반 | SceneColor | RS7·RS10 |
| F13 | 하늘·height fog(전체 화면) | 공통 | 공통 | SceneColor | RS5 |
| F14 | 투명(Standard)·distortion | 공통 | 공통 | SceneColor | RS5 |
| F15 | `PostChain`(DOF·temporal·투명 AfterDof·motion blur·AfterMotionBlur·노출·bloom·tone map·AA·spatial upscale) | 공통 | 공통 | 출력 색 | RS6 |
| F16 | HUD·UI 합성 → present | 공통 | 공통 | 스왑체인 | 화면 마일스톤 경로 |

- **F7을 base pass 앞에 둔다.** 두 경로가 조명 함수 안에서 그림자를 직접 표본하므로(9절) base pass 전에 있어야 한다. UE forward와 같은 위치다(`DSR:2922–2927`).
- **F8 Forward+ 보조 출력.** Forward+는 GBuffer가 없으므로 screen-space 효과(AO·SSR·contact shadow·screen-space GI)가 켜진 view에서만 depth prepass가 보조 법선·roughness(`RGB10A2Unorm`)를 함께 쓴다(계획 11.1절 "필요한 depth/normal/roughness 보조 출력 선언"). 간접광 합성(F12)이 albedo를 요구하면 base pass가 보조 색 타깃을 하나 더 쓴다(8.4절).
- **decal.** 계획 11.1절은 Deferred에 GBuffer decal, Forward에 mesh decal 또는 pre-shading decal을 요구한다. Deferred는 F10에서 GBuffer에 블렌드하고, Forward+는 F8a에서 DBuffer(base pass 전 decal 버퍼)에 그려 base pass가 읽는다. UE도 DBuffer decal을 base pass 전에 그린다(`DSR:2962`, `R/CompositionLighting/CompositionLighting.cpp:548`). DBuffer는 base pass SRV 예산을 쓰므로 퍼뮤테이션으로 켠다(7.4절).
- **순환 금지.** F12의 screen-space 추적은 F9·F11 뒤의 현재 SceneColor를 읽고, 결과는 별도 기여 버퍼로 합성한다(계획 11.4절 "같은 항을 이중 가산하지 않는다"). 렌더 그래프가 한 pass 안의 같은 subresource 읽기·쓰기를 거부한다(RenderBase 7.2절).
- **path tracing.** `RenderMode::PathTracing` view는 F4–F14를 건너뛰고 누적 pass로 SceneColor를 만든 뒤 F15로 간다(12.3절).
