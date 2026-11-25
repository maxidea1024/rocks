## 18. 화면 마일스톤과 구현 순서

### 18.1 화면 마일스톤 범위

**성격.** 처음에는 화면에 최소한으로 그려 시험하는 단계였으나, 그 목적은 의미가 없어졌다. 렌더 기반을 제대로 세우는 단계로 정의한다(2026-11-16 사용자 결정 "원래 목표는 미니멀하게 테스트하고자 했던건데 이제 의미없음. 렌더코어 제대로 구현하는게 나음"). 고정 순서(A0-B → A0-O → B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)에서의 자리는 그대로이고, 렌더 세션이 맡는다.

**결정(2026-11-16).**
- 셰이더 경로: 쿠킹 셰이더 위에서 그린다. 셰이더 시스템 결정 11 (나)다. backend별 원문·런타임 컴파일 경로(`ShaderSource`·`D3DCompile`·Vulkan 컴파일 훅)는 SH2e2(2026-11-24)에서 없앴다.
- 렌더 기반 구조: 렌더 lane·장치 자원 수명·내장 자원 팩을 마일스톤 안에서 만든다.
- 텍스처 범위: AssetSystem A5의 텍스처 전부를 화면에서 확인한다.
- 검증 backend: 셰이더 경로를 따른다. D3D12·Vulkan·GLES 미리보기다.

**포함.**

| 영역 | 항목 |
|---|---|
| Core | CP1–CP7, CP4·CP8(콘솔 설정·Config, 별도 스펙) |
| 셰이더 시스템 | 6절 4항 순서의 P2·P8·P10·P3·P4·SH0·P6a·P5·SH1·P14·P9·P11·P6b·SH2. 결정 2–7과 SX1–SX14를 그 기한에 정한다. P7·SH3·SH4는 렌더러 RS0 전으로 옮겼다(18.4절 SM11·SM12, 개정 46) |
| Graphics | GX1–GX11. VulkanGraphics(SH2), OpenGLGraphics의 GLES 미리보기 전환(P11) |
| RenderBase | RB1–RB7 |
| RenderGraph | RG1–RG2 |
| 텍스처 표시 | 6.5절 계약으로 쿠킹된 A5 텍스처를 GPU 자원으로 만들어 그린다. 다섯 차원 전부다: 2D 사각형, Cube 하늘 상자, 2DArray·CubeArray는 slice 선택, Volume은 단면. 모든 비압축 형식(HDR은 tone map pass로 표시), 샘플러 설정·swizzle·sRGB, CPU Availability의 대체 표시도 포함한다 |
| 통합 | WorldIntegration 텍스처 컴포넌트가 `RenderHandle`을 예약하고 프레임 packet으로 넘긴다(업로드와 함께 — 18.4절 SM8). Sandbox에서 키보드·마우스·게임패드로 텍스처·slice·카메라를 바꾼다 |

**범위 밖(다음 단계).**
- Forward+·Deferred 조명 경로와 GI(계획 11·14절).
- 메시 에셋. 내장 메시만 쓴다.
- 텍스처 압축·색 보정(에셋 세션)과 텍스처 스트리밍(렌더 세션, 마일스톤 다음 단계). 6.5절 계약의 상주 꼬리 덩어리는 마일스톤에서도 그대로 읽는다.
- D3D11·Metal(SH5), Android(SH6).
- RG3–RG5, GX12·GX13.

**완료 기준.**
- 셰이더 시스템 SH0–SH2 완료 기준(SH3·SH4는 18.4절 SM11·SM12).
- 이 문서 RB1–RB7·RG1–RG2 완료 기준.
- Sandbox self-test가 다섯 차원·형식별 장면을 D3D12·Vulkan·GLES 미리보기에서 허용 오차 안으로 대조한다. Vulkan·GLES는 CI에서 NOT RUN일 수 있고, 그때는 로컬 결과를 기록한다.
- 검증 계층 오류 0.
- Release 실행 파일에 셰이더 컴파일러가 없다(셰이더 시스템 9절).
- 렌더 lane 모드와 인라인 모드의 결과가 같다.

### 18.2 구현 순서(제안)

괄호는 의존이다. 셰이더 시스템 단계는 그 문서 6절 4항 순서를 따른다.

1. Core: CP1 → CP2·CP3·CP6·CP7(병행) → CP8 → CP4 → CP5.
2. Graphics:
   - GX7 → GX11 → GX10. GX8·GX9는 GX7 뒤 언제든.
   - A0-G 뒤 GX1·GX2.
   - 셰이더 SH2 뒤 GX3 → GX4 → GX5. GX6은 P2·SH2 뒤.
   - 후속: GX12(GX5 뒤), GX13(모바일 렌더러·SH6과 함께 — 4.4절 GX13 결정, 개정 40).
3. RenderBase:
   - RB1(CP1·CP6·GX7·GX10·GX11, 설정 snapshot은 CP4 뒤)
   - RB2(GX2·GX7·GX8·GX11)
   - RB3(SH2·P14·P6b·GX1·GX11·CP7, 일괄 생성 선택은 CP2)
   - RB4(GX3·GX6·GX8·CP3, SH2·SH3)
   - RB5(P14·SX4·GX3·GX4)
   - RB6(GX3·GX4·RB2)
   - RB7(GX9·CP5)
4. RenderGraph: RG1(RB4·RB7·GX1·GX2·GX4·GX5·CP3) → RG2(GX8) → RG3(CP2·GX11, 같은 attachment 연속 pass 병합은 GX13과 무관 — 개정 40) → RG4(GX1) → RG5(GX12).

### 18.3 RB3–RB6 남은 항목(개정 40, 2026-11-25)

SH2e3(RB3 셰이더 집합·RB4의 SH2분)·GX3·GX4(개정 37)·GX6(개정 39) 뒤에 스펙(5·6·9–11절)과 코드(`Engine/Source/Runtime/RenderBase`·`RenderGraph`·`Graphics`)를 대조한 결과다. "결정"은 권장안으로 확정했고(애매하면 UE 5.8), "순서"는 화면 마일스톤(18.1절)에 필요한 순서다. 1–4는 개정 41–45에서 구현했다(각 행의 "구현 개정").

| ID | 단계 | 항목 | 지금 | 결정 | 순서 |
|---|---|---|---|---|---|
| RR1 | RB6 | 상태 프리셋(11.2절) | 없음(`AlphaBlend()` 하나) | 구현(11.4절 1항) — **구현 개정 41** | 1 |
| RR2 | RB6 | 전체화면 pass: 엔진 셰이더 `FullscreenTriangle`·RenderBase 그리기 도우미·그래프 `AddFullscreenPass`(11.2절) | Sandbox 시험 셰이더만 있음 | 구현(11.4절 2·3항) — **구현 개정 41** | 1 |
| RR3 | RB6 | RenderBase 경로 null 텍스처 바인딩 0건 계수(11.3절) | 장치가 null을 흰 텍스처로 바꿀 뿐 세지 않음 | 구현: 장치 계수 `NullTextureBindings`(11.4절 4항) — **구현 개정 41** | 1 |
| RR4 | RB5 | 정점 입력 타입 등록(늦은 등록 거부·결정성, 10.2·10.3절) | 등록부 없음 — 셰이더 집합·쿠커가 호출자 목록을 받음 | 구현: 셰이더 타입 등록부에 함께 둔다(10.6절 1항) — **구현 개정 42** | 2 |
| RR5 | RB5 | include 연결 `/Engine/Generated/VertexInput.rsh`(10.2절) | 없음 — job이 정점 입력 타입을 받지 않아 축이 컴파일에 반영되지 않음 | 구현(10.6절 2항, 셰이더 시스템 개정 48) — **구현 개정 42** | 2 |
| RR6 | RB5 | 엔진 기본 정점 입력 타입(10.2절 (2)) | 없음 | 구현(10.6절 3항) — **구현 개정 43** | 2 |
| RR7 | RB6·RB2 | 내장 자원 팩 1차(11.1절 — 2D 넷·차원별 대체·0·1·identity 버퍼·기본 메시·fallback 셰이더), `Pending`·`Failed` handle의 대체 자원과 계수(6.2·6.4절) | 없음(`OnLaneStart` hook만) | 구현(11.4절 5–7항) — **구현 개정 43** | 3 |
| RR8 | RB4 | 셰이더 교체(9.8절) | 없음 | 구현(9.8절 "구현 설계") — SH4 감시 재컴파일이 이것을 부른다 — **구현 개정 44** | 4 |
| RR9 | RB3 | 엔진 → 프로젝트 라이브러리 조회 순서·중복 키 오류(9.2절) | 집합 하나가 라이브러리 하나 | 후속: 프로젝트 라이브러리는 머티리얼 시스템이 연다(MS 단계). 마일스톤에는 프로젝트 셰이더가 없다 | — |
| RR10 | RB3 | 로드 때 일괄 생성 선택(9.2절) | 없음 | 버림: SH2e2부터 장치 셰이더는 검사·복사·decode만 하는 CPU 사본이다(셰이더 시스템 16.1절 SH2e2 메모). 비용은 파이프라인 생성이며 그 미리 준비는 9.7절 후속이다 | — |
| RR11 | RB3 | 엔진 셰이더 집합을 렌더 lane 시작 단계에서 열기(5.5·9.2절) | Sandbox 장치별 집합만 | 구현(9.12절): 엔진 셰이더 호스트 서비스(등록부·원문 디렉터리·공급자·쿠킹 원천 자리)와 lane 시작의 `EngineRenderResources`, Developer `DevelopmentShaderProvider` 모듈 — **구현 개정 45**. 쿠킹 원천 구현은 에셋 쪽 매니페스트 v2 뒤 | 3 |
| RR12 | RB4 | `ParameterBatch` 자원 부분(단계별 여러 텍스처·샘플러·버퍼 view, null·종류 불일치 검증, 9.3절) | 장치는 텍스처 slot 하나·이름 표 개발 경로 | 후속(렌더러 RS0 전): 장치 쪽 다중 텍스처 slot이 함께 필요하다. 마일스톤 텍스처 표시는 pass마다 텍스처 하나다 | — |
| RR13 | RB4 | 공유 상수 slot 표(9.5절) | 없음 | 후속(렌더러 RS0): 바인딩 layout의 구조체 identity(SX8)와 렌더러 slot 등록이 먼저다(9.11절 9항과 같다) | — |
| RR14 | RB4 | 바인딩 지도를 MetaSchema 스키마로 만들기(결정 R11) | 손으로 쓴 레이아웃 표 | 후속(SH3·P7) | — |
| RR15 | RB5 | 정적 정점 배치 등록·PSO 미리 준비(10.2·9.7절) | 없음 | 후속(9.7절 미리 준비와 함께). 엔진 기본 정점 입력 타입의 배치는 RenderBase 상수다(10.6절 3항) | — |
| RR16 | RB5 | 메시 정점 입력 `.rsh` 함수 인터페이스(10.2·10.5절) | 없음 | 후속(렌더러 RS0) | — |
| RR17 | RB6 | viewport 부분 clear·UAV clear 셰이더, (값, 형식, 차원) 기본 텍스처 캐시(11.1·11.2절) | 없음 | 후속(스펙 그대로 — 엔진 셰이더 SL1 `ClearResources`) | — |
| RR18 | RB2 | 장치 재생성(6.2절, BR04의 device loss) | 없음 | 후속(장치 손실 복구 단계) | — |
| RR19 | GX13 | subpass·framebuffer fetch 검토 | 후속 | 후속 확정(4.4절 GX13 결정) | — |

이미 끝난 것(대조 결과): RB3 9.10절 완료 기준(SH2e3), RB4의 바인딩 지도(상수)·상수 블록·파이프라인 캐시(그래픽·compute)·GLES 업로드 판단·상수 버퍼 묶기(GX6 — `PerFrame` 블록 회수는 마지막 사용 fence, 9.11절), RB5의 정점 배치 ⊇ 입력 마스크·형식 계열 검사(`MatchShaderVertexInputs`, SH2e2)와 다중·인스턴스 stream 픽셀 대조(GX3a `VertexSelfTest`), RB6의 attachment load clear.

### 18.4 화면 마일스톤 남은 항목(개정 46, 2026-11-25)

18.1절 완료 기준과 코드(HEAD `b7b405f8`)를 대조한 결과다. 쿠킹 텍스처 업로드(6.5절)는 에셋 세션이 쿠킹 텍스처 데이터를 올린 뒤 다음 단계에서 한다. "결정"은 권장안으로 확정했고(애매하면 UE 5.8), "순서"는 이번 단계 안의 순서다. "다음 단계"는 업로드와 함께 하는 일, "후속"은 마일스톤 밖이다.

| ID | 항목 | 지금 | 결정 | 순서 |
|---|---|---|---|---|
| SM1 | Sandbox `--engine --window` 렌더 경로: 장치 생성, 렌더 lane(Thread), 표면 작업(스왑체인 생성·resize·표면 소실), lane 시작의 `EngineRenderResources`, 프레임 packet, 장면 HDR 타깃 → tone map → 스왑체인, 엔진 종료 hook으로 lane 종료 순서(5.5절) | RR11의 등록부 Freeze까지만. 장치·lane이 없고 창은 `PresentBlankFrame` | 구현(18.5절) | 1 |
| SM2 | 화면 view: CameraDirector `CameraView` → 카메라 상대 view·투영 | 없음 | 구현(18.5절 2항): reversed-Z 무한 원평면, near 10 cm(UE `NearClipPlane`), 수평 FOV, 창 종횡비 | 1 |
| SM3 | HDR tone map pass(18.1절 "HDR은 tone map pass로 표시") | 없음 | 구현(18.5절 3항): 엔진 셰이더 `Color/FilmicToneCurve.rsh`(엔진 셰이더 A03 filmic — SL0 몫)와 `PostProcess/TonemapPass.rsf`의 최소 판(A17 — LUT 없이 곡선 직접). LUT 굽기·자동 노출·bloom·vignette·grain은 SL8(RS6) | 2 |
| SM4 | 입력 조작: 카메라(마우스·키보드·게임패드) | Pawn이 눌림을 로그로 남길 뿐 | 구현(18.5절 4항): UE DefaultPawn 바인딩 | 3 |
| SM5 | 엔진 렌더 경로 self-test | 없음 | 구현(18.5절 5항): 숨긴 창, D3D12·D3D11·Vulkan·GLES 미리보기, tone map 결과 = CPU 기준, Thread = Inline, backend 사이 대조, 검증 오류 0 | 4 |
| SM6 | 텍스처 표시 pass(엔진 셰이더 `Debug/TextureInspector.rsf` — 다섯 차원·slice·mip, HDR은 노출 + filmic)와 표시 대상·slice 고르기 입력 | 없음 | 구현(18.5절 6항): 셰이더·pass·입력은 이번에 lane에서 만든 절차 생성 텍스처로 시험하고, 쿠킹 텍스처 연결은 다음 단계 | 5 |
| SM7 | 다섯 차원·형식별 쿠킹 텍스처 장면 대조(18.1절 완료 기준 3항) | — | 다음 단계(6.5절 업로드) | — |
| SM8 | WorldIntegration 텍스처 컴포넌트(`RenderHandle` 예약 → 프레임 packet) | 모듈 없음 | 다음 단계: 컴포넌트가 참조할 텍스처 에셋 타입이 에셋 세션 산출물로 생긴다. 그 전에 모듈을 만들면 참조 없는 자리표시가 된다(엔진 층 원칙). 이번 단계의 Sandbox 장면은 실행 파일이 직접 만든다 | — |
| SM9 | RR12 `ParameterBatch` 자원 부분·장치 다중 텍스처 slot | 18.3절 후속 | 후속 유지(렌더러 RS0 전): SM3·SM6도 pass마다 텍스처 하나다 | — |
| SM10 | RR13 공유 상수 slot 표 | 18.3절 후속 | 후속 유지(RS0): view 상수 slot이 첫 소비자다 | — |
| SM11 | 셰이더 시스템 SH3(파라미터 구조체 P7·생성 HLSL·그래프 파라미터 수집기) | 손으로 쓴 레이아웃 표 | 렌더러 RS0 전으로 옮김: 마일스톤 셰이더(유틸리티·tone map·텍스처 표시)는 루트 상수와 텍스처 하나라 손으로 쓴 표(셰이더 시스템 7.2절 5항)로 충분하다. 생성 구조체의 첫 소비자는 view 상수(SM10)와 머티리얼(MS)이다 | — |
| SM12 | 셰이더 시스템 SH4(감시 재컴파일 → `ShaderSetSlot::Propose`, 오류 위치, 디버그 심볼) | 런타임 교체(RR8)만 | 렌더러 RS0 전으로 옮김: 교체 경로(RR8)는 끝났고, 감시·재컴파일은 개발 반복 편의라 화면 완료 기준(픽셀 대조·검증 오류 0·Release 컴파일러 없음)에 들지 않는다. 셰이더 수가 늘어나는 RS0에서 이득이 크다 | — |
| SM13 | 엔진 셰이더 SL1 나머지: `GenerateMips`·`ClearResources`(RR17)·`CopyDepth`·`PlaceholderShaders`·`TextureSamplingHelpers`·`ComputeDispatch`·`WaveOperations` probe·`LutCoordinates`·`ColorMaps` | 없음 | 후속: 쿠킹 텍스처는 mip을 갖고 오므로 밉 생성은 런타임 생성 텍스처가 생길 때, LUT 좌표는 SL8, 자리표시는 MS MSR10과 함께. SM6이 쓰는 깊이·정수 시각화는 SM6에서 그 셰이더 안에 둔다 | — |
| SM14 | Release 실행 파일에 셰이더 컴파일러 없음(18.1절) | SH2 9절 정책이 검사 | 확인만(바뀐 것 없음) | — |

이에 따라 18.1절 완료 기준의 "셰이더 시스템 SH0–SH4"는 SH0–SH2가 된다(SH3·SH4는 셰이더 시스템 16절 표의 순서 그대로 RS0 전).

### 18.5 엔진 창 렌더 경로 설계(SM1–SM6, 개정 46)

Sandbox(실행 파일)에 둔다. 엔진 층에는 SM3 셰이더와 그 타입만 더한다. 정식 장면 렌더러는 RS0 `Renderer`다(렌더러 17절).

1. **구조(SM1).**
   - `ScreenRenderer`(렌더 lane 처리기): lane 시작에 스왑체인(표면이 있으면) → `EngineRenderResources::Open`(9.12절) → 파이프라인 캐시·그래프 풀을 만든다. 종료에 역순으로 `Retire`한다.
   - `ScreenHost`(게임 lane): 장치(명령줄 `--graphics=d3d12|d3d11|vulkan|opengl`, 기본 D3D12)·`RenderLane`을 만들고 엔진 루프 hook에서 프레임마다 view와 장면 목록을 프레임 packet으로 넘긴다. 엔진 종료 hook 다섯을 lane 종료 단계(`BlockFrames` → `WaitFramesIdle` → `DrainRetires` → `CloseResources` → `Stop`)에 잇는다. 장치는 엔진 `Shutdown` 뒤, 창보다 먼저 파괴한다.
   - 프레임 그래프: `SceneColor`(`RGBA16Float`)·`SceneDepth`(`D32Float`, clear 0) → `BasePass`(내장 메시를 fallback 메시 셰이더로, 깊이 `GreaterEqual`) → `Tonemap`(전체화면 pass, `SwapChainTarget`). 스왑체인의 그리기 view가 sRGB(장치 계약)라 tone map은 선형 값을 쓰고 부호화는 view가 한다. UE는 UNORM 백버퍼에 셰이더가 sRGB 부호화를 하지만 결과 값은 같다.
   - 표면 작업: 창 크기 변경은 `PostSurface`(resize, 0 크기면 건너뜀), Android 표면 소실은 `PostSurfaceAndWait`(스왑체인 `Retire` 뒤 장치 대기) 뒤 `AcknowledgeSurfaceLost`, 새 표면은 다시 생성. 스왑체인이 없는 프레임은 packet을 보내지 않는다.
   - lane 시작 실패(예: Release·Android에 셰이더 공급자가 없어 `UnsupportedCapability`)는 Warning을 남기고 렌더 없이 엔진을 계속 돌린다(빈 프레임 경로). 실패를 숨기지 않되 입력·게임 시험 경로를 막지 않기 위해서다.
   - 장면: 이번 단계는 실행 파일이 고정 목록(바닥 plane, HDR 색 cube 셋, 내장 텍스처)을 만든다. World 추출은 SM8 뒤다.
2. **view(SM2).** 게임 lane이 첫 LocalPlayer의 `CameraDirector::View()`로 double 행렬을 만든다: 항목 local → 카메라 상대 world(이동에서 카메라 위치를 뺌, 계획 2.2절) × `MakeViewRotation`(UE `FInverseRotationMatrix` × 축 바꿈) × `TryMakePerspective(fov/2, 폭, 높이, 10 cm, ∞)`(reversed-Z). 항목마다 float `localToClip`을 packet에 담는다(fallback 메시 셰이더 `BuiltinMeshConstants`). 컬은 뒤면(앞면 반시계 — 10.6절 3항 내장 메시 감김)이고 view 행렬식이 음수면 뒤집는다(UE `bReverseCulling`).
3. **tone map(SM3).** UE 5.8 기본 경로(`PostProcessCombineLUTs.usf` 색 경로를 LUT 없이 픽셀마다)와 같다: 장면 색 × 노출 배율 → 작업 색 공간 sRGB → AP1 → 넓은 색역 확장(`ExpandGamut` 1) → blue correction(0.6) → `FilmToneMap`(ACES glow·red modifier·AP1 desaturate·toe·shoulder) → `ToneCurveAmount`(1) → blue correction 되돌림 → sRGB(음수 0) → 스왑체인 sRGB view. 기본값은 UE `FPostProcessSettings`와 같다(slope 0.88, toe 0.55, shoulder 0.26, black clip 0, white clip 0.04 — `Engine/Private/Scene.cpp:436–450`). 색 보정(`ColorCorrectAll`)은 기본값에서 항등이라 넣지 않는다(SL8). 노출 배율은 packet 값이고 기본 1이다(자동 노출 SL8). 상수는 루트 상수 48 byte(`TonemapConstants`, 손으로 쓴 레이아웃 표)이고 타입 `TonemapPixel`은 RenderBase가 등록한다(RS0에서 `RendererShaders`로 옮긴다).
4. **입력(SM4).** SandboxPawn이 UE `ADefaultPawn` 바인딩을 따른다(`Engine/Private/DefaultPawn.cpp:94–126`): 앞뒤 W·S·위아래 화살표·왼쪽 스틱 Y, 좌우 A·D·왼쪽 스틱 X, 위아래 E·Space·남쪽 버튼·왼쪽 스틱 누름(+)과 Q·C·왼쪽 Ctrl·오른쪽 스틱 누름(−)(월드 Z), 회전율 좌우 화살표·오른쪽 스틱(45 °/s, UE `BaseTurnRate`). 속도는 1200 cm/s(UE `FloatingPawnMovement::MaxSpeed`)이고 입력 벡터를 길이 1로 자른다. 가속·감속(4000·8000)은 이동 컴포넌트 단계에서 넣는다. 마우스는 오른쪽 버튼을 누른 동안만 회전한다(창이 마우스를 가두지 않으므로 UE 편집 viewport 방식), 1 pixel당 0.175°(UE 기본 축 감도 0.07 × 컨트롤러 배율 2.5), pitch는 ±89.9°(UE `ViewPitchMin·Max`). 이동은 Pawn root, 회전은 Controller `controlRotation`에 쓴다.
5. **self-test(SM5).** 실행 파일 안에서 장치마다 숨긴 창과 Sandbox 게임(GameInstance·SandboxPawn)으로 엔진을 몇 프레임 돌린다. 마지막 프레임은 tone map을 capture 텍스처(`RGBA8UnormSrgb`)에도 그려 읽는다. 단언: (a) 화면 가운데 HDR 색 cube 픽셀 = CPU 기준 tone map(같은 식의 C++ 구현, 채널 ±2) (b) Thread·Inline 모드 픽셀 동일 (c) backend 사이 채널 ±8·2%(8절 기준) (d) 검증 오류 0, 남은 lane 작업·handle·장치 회수 0 (e) 주입한 W 입력 N 프레임 뒤 카메라 위치 = 1200 cm/s × N × 고정 delta(시간 운 없음).
6. **텍스처 표시(SM6).** `Debug/TextureInspector.rsf`에 차원별 PS(2D·2D array slice·cube 면 펼침·cube array slice·volume 단면)와 mip 선택을 두고 루트 상수로 slice·mip·노출을 받는다. HDR은 SM3 곡선, 정수 형식은 값 그대로 정규화, 깊이는 reversed-Z 깊이를 그대로 회색으로 보인다. 입력: 숫자 1–5 차원, `[`·`]` slice, `-`·`=` mip(패드 어깨 버튼). 이번 단계는 lane이 만든 절차 생성 텍스처를 쓴다.

**버린 대안.** (가) 게임 lane에서 장치를 직접 쓰는 렌더(lane 없이) — 18.1절 완료 기준 "lane 모드 = 인라인 모드"와 5절 구조를 건너뛴다. (나) Sandbox `SampleRenderer` 재사용 — Sandbox 시험 셰이더라 엔진 셰이더 집합·내장 자원 팩 경로를 거치지 않는다. (다) UNORM 스왑체인 + 셰이더 sRGB 부호화(UE 방식) — 스왑체인 계약(그리기 view sRGB)을 바꿔야 하고 결과 값이 같다. (라) 32³ LUT 굽기 — LUT 생성 pass는 색 보정과 함께 SL8이 맡는다(엔진 셰이더 A17 "RS0은 LUT 없이 곡선 직접").
