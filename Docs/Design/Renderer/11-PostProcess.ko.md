# 렌더러(Renderer) 설계 — 11. 후처리와 업스케일 연결점

[목차](README.ko.md)

## 11. 후처리와 업스케일 연결점

### 11.1 UE 5.8 순서

`R/PostProcess/PostProcessing.cpp:412–2265`(호출 `DSR:4312`): DOF `:976` → 색 보정 LUT `:1089` → TAA·TSR·외부 temporal upscaler `:1107–1184` → motion blur `:1269`(AfterMotionBlur 투명 `:1253`) → 축소 사슬 `:1330–1351` → 지역 노출 `:1369` → histogram `:1406` → 노출 `:1440–1461` → bloom(FFT `:1506` 또는 Gaussian `:1555–1564`) → lens flare `:1571` → tone map `:1629` → FXAA `:1648`·SMAA `:1661` → tone map 뒤 후처리 머티리얼 → primary upscale `:2143–2175` → secondary upscale `:2179–2205`.

- 해상도: TSR까지는 render 해상도, TSR은 출력(secondary) 해상도로 쓰므로 그 뒤 motion blur·bloom·tone map은 출력 해상도다(`R/PostProcess/TemporalSuperResolution.cpp:1982`). spatial 경로는 primary upscale에서 바뀐다(`R/PostProcess/PostProcessing.cpp:683`, `EN/Public/SceneView.h:136–147`).
- 외부 upscaler interface: temporal(`RPub/TemporalUpscaler.h:12, 75`), spatial(`R/PostProcess/PostProcessUpscale.h:41, 61`).
- HDR 출력: tone map이 출력 장치 형식(ST2084·scRGB 등)을 LUT 단계에서 인코딩(`R/PostProcess/PostProcessTonemap.cpp:246`, `Core/Public/GenericPlatform/GenericPlatformMisc.h:472–486`, `SH/PostProcessCombineLUTs.usf:434, 440`).

### 11.2 Rocks `PostChain`

| # | pass | 해상도 | 입력 | 단계 |
|---|---|---|---|---|
| PC1 | DOF(후속) | render | SceneColor·깊이 | 계획 26절 |
| PC2 | `TranslucentAfterDof` 합성 | render | — | RS5 |
| PC3 | temporal: `Upscaler::AddPasses`(계획 12.1절의 TAA·TAAU·FSR·DLSS·XeSS) | render → 출력 | `UpscaleInputs`: HDR 색·깊이·velocity·노출·reactive·투명 mask·jitter·reset | RS6(U1) |
| PC4 | motion blur(후속) | 출력 | velocity | 계획 26절 |
| PC5 | `TranslucentAfterMotionBlur` 합성 | 출력 | — | RS5 |
| PC6 | 노출(histogram → 자동 노출) | 출력 | — | RS6 |
| PC7 | bloom(Gaussian 1차) | 출력 | — | RS6 |
| PC8 | tone map + 색 보정 LUT + 출력 인코딩(SDR sRGB, HDR은 계획 26절) | 출력 | 노출 | RS0(최소)·RS6 |
| PC9 | spatial AA(FXAA, 선택) | 출력 | — | RS6 |
| PC9a | 편집 표시 합성(에디터 view만, UE `EditorPrimitive`) | 출력 | 편집 단순 요소, 장면 깊이 | RS13 |
| PC9b | 디버그 표시 합성(디버그 그리기 빌드, UE `DebugPrimitive`) | 출력 | 디버그 단순 요소, 장면 깊이 | RS13 |
| PC10 | spatial upscale(Native·Spatial 모드, 계획 12.1절 U0) | render → 출력 | — | RS6 |
| PC11 | HUD·UI 합성 | 출력 | — | 화면 마일스톤 경로 |

- **temporal 자리.** 계획 12.2절 "선형 HDR color, depth, motion, exposure가 유효한 단계에서 실행하고 tone mapping·출력 UI는 그 뒤"를 따른다. `UpscaleInputs`의 노출은 이전 프레임 노출 값이다(이번 프레임 PC6이 뒤에 있으므로). UE도 temporal 단계 뒤에 노출을 계산한다(`:1107–1184` → `:1440`).
- **velocity 규약.** 렌더러 velocity는 계획 12.2절의 엔진 공통 규약(현재 표본 → 이전 위치, 출력 정규화)이고 adapter가 SDK 단위로 바꾼다. reversed-Z 깊이는 adapter가 SDK 깊이 규약 표시로 넘긴다(계획 12.1절).
- **텍스처 mip bias.** temporal upscale(PC3)을 쓰는 view는 머티리얼 텍스처 표본에 음수 mip bias(log2(render 폭 ÷ 출력 폭))를 view 상수로 건다. 텍스처 스트리밍의 표본 해상도 계산(TextureStreaming 4.1절 "렌더 폭 × 2^(−mip bias)")이 이 값을 쓰므로 렌더러가 view마다 그 값을 사용 기록과 함께 넘긴다(RQ21).
- **표시 합성 자리(PC9a·PC9b).** UE와 같이 tone map → spatial AA → tone map 뒤 후처리 머티리얼 다음이고 spatial upscale 앞이다(UE `EPass` 순서 `Tonemap → FXAA → SMAA → PostProcessMaterialAfterTonemapping → … → EditorPrimitive → DebugPrimitive → … → PrimaryUpscale` — `Renderer/Private/PostProcess/PostProcessing.cpp:507–550`, 533–534행). 표시는 tone map 영향을 받지 않고 AA로 흐려지지 않는다. RS6 전(RS0의 최소 tone map만 있을 때)에는 PC8 바로 뒤에 붙인다.
- **경로 독립.** `PostChain`은 SceneColor·깊이·velocity만 받으므로 Forward+·Deferred·PathTracing이 같은 사슬을 쓴다. 경로 전환·upscaler 교체·크기 변경은 history reset이다(계획 12.2절).
- 세부 효과(DOF·motion blur·지역 노출·lens flare·film grain·후처리 머티리얼)는 계획 26절 후처리 catalog 스펙이 정한다. 이 문서는 자리(해상도·입력)만 정한다.
