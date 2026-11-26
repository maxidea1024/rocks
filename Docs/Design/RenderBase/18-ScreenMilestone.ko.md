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
| 텍스처 표시 | 6.5절 계약으로 쿠킹된 A5 텍스처를 GPU 자원으로 만들어 그린다. 다섯 차원 전부다: 2D 사각형, Cube 하늘 상자, 2DArray·CubeArray는 slice 선택, Volume은 단면. 모든 비압축 형식(HDR은 tone map pass로 표시), 샘플러 설정·swizzle(셰이더 복제 — 6.5절)·sRGB, CPU Availability의 대체 표시도 포함한다 |
| 통합 | WorldIntegration 텍스처 컴포넌트(텍스처 렌더 캐시 `TextureRenderCache` — 18.5절 8항)가 `RenderHandle`을 예약하고 프레임 packet으로 넘긴다(업로드와 함께 — 18.4절 SM8). Sandbox에서 키보드·마우스·게임패드로 텍스처·slice·카메라를 바꾼다 |

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
| RR12 | RB4 | `ParameterBatch` 자원 부분(단계별 여러 텍스처·샘플러·버퍼 view, null·종류 불일치 검증, 9.3절) | 장치는 텍스처 slot 하나·이름 표 개발 경로 | 구현(9.13절, 설계 개정 50 — 18.4절 SM9): 장치 텍스처·샘플러 slot을 layout 위치로, 자원 지도 `ShaderResourceMap`과 `ParameterBatch`. 버퍼 view의 위치 묶기는 후속 — **구현 개정 51** | 5 |
| RR13 | RB4 | 공유 상수 slot 표(9.5절) | 없음 | 기반 구현(9.14절, 개정 53 — 표·파이프라인 지도·묶음, SX8 identity로 찾기). 렌더러 slot 등록·view마다 묶기는 렌더러 RS0 | — |
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
| SM7 | 다섯 차원·형식별 쿠킹 텍스처 장면 대조(18.1절 완료 기준 3항) | — | 구현(18.5절 7항, 설계 개정 48·구현 개정 49): 6.5절 업로드(RenderBase `CookedTexture.h`), swizzle 셰이더 복제, TextureInspector 정수 엔트리, Sandbox 표시 목록·쿠킹 시험 콘텐츠·장면 대조 — **구현 개정 49** | 6 |
| SM8 | WorldIntegration 텍스처 컴포넌트(`RenderHandle` 예약 → 프레임 packet) | 모듈은 메시 세션이 만들었다(D2b). 변환·업로드 연결은 Sandbox(SM7) | 구현(18.5절 8항, 설계 개정 54·구현 개정 55): 텍스처 에셋마다 GPU 자원 하나를 만드는 `TextureRenderCache`(UE `UTexture::UpdateResource` 자리). 리플렉션 World 컴포넌트는 만들지 않는다(UE 대응 없음 — 첫 World 사용자는 `BillboardComponent` CM2·머티리얼 MS). 변환 `PlatformTextureSource`를 Sandbox에서 옮겼다 — **구현 개정 55** | 8 |
| SM9 | RR12 `ParameterBatch` 자원 부분·장치 다중 텍스처 slot | 18.3절 후속 | 구현(9.13절, 설계 개정 50·구현 개정 51): 렌더러 RS0 전에 당겼다(2026-11-26 렌더 세션 순서) | 7 |
| SM10 | RR13 공유 상수 slot 표 | 18.3절 후속 | 기반(표·파이프라인 지도·묶음)은 구현(9.14절 개정 53). 렌더러 slot 등록·view마다 묶기는 RS0(view 상수 slot이 첫 소비자다) | — |
| SM11 | 셰이더 시스템 SH3(파라미터 구조체 P7·생성 HLSL·그래프 파라미터 수집기) | 손으로 쓴 레이아웃 표 | 렌더러 RS0 전으로 옮김: 마일스톤 셰이더(유틸리티·tone map·텍스처 표시)는 루트 상수와 텍스처 하나라 손으로 쓴 표(셰이더 시스템 7.2절 5항)로 충분하다. 생성 구조체의 첫 소비자는 view 상수(SM10)와 머티리얼(MS)이다 | SH3a(상수 멤버·생성 C++ 표·HLSL·바인딩 layout v4 identity·엔진 셰이더 이관)는 구현(셰이더 시스템 16.2절 개정 55), SH3b(자원 멤버)·SH3c(그래프 수집기)는 남음 | — |
| SM12 | 셰이더 시스템 SH4(감시 재컴파일 → `ShaderSetSlot::Propose`, 오류 위치, 디버그 심볼) | 런타임 교체(RR8)만 | 렌더러 RS0 전으로 옮김: 교체 경로(RR8)는 끝났고, 감시·재컴파일은 개발 반복 편의라 화면 완료 기준(픽셀 대조·검증 오류 0·Release 컴파일러 없음)에 들지 않는다. 셰이더 수가 늘어나는 RS0에서 이득이 크다 | — |
| SM13 | 엔진 셰이더 SL1 나머지: `GenerateMips`·`ClearResources`(RR17)·`CopyDepth`·`PlaceholderShaders`·`TextureSamplingHelpers`·`ComputeDispatch`·`WaveOperations` probe·`LutCoordinates`·`ColorMaps` | 없음 | 후속: 쿠킹 텍스처는 mip을 갖고 오므로 밉 생성은 런타임 생성 텍스처가 생길 때, LUT 좌표는 SL8, 자리표시는 MS MSR10과 함께. SM6이 쓰는 깊이·정수 시각화는 SM6에서 그 셰이더 안에 둔다 | — |
| SM14 | Release 실행 파일에 셰이더 컴파일러 없음(18.1절) | SH2 9절 정책이 검사 | 확인만(바뀐 것 없음) | — |

이에 따라 18.1절 완료 기준의 "셰이더 시스템 SH0–SH4"는 SH0–SH2가 된다(SH3·SH4는 셰이더 시스템 16절 표의 순서 그대로 RS0 전).

### 18.5 엔진 창 렌더 경로 설계(SM1–SM8, 개정 46·48·54)

Sandbox(실행 파일)에 둔다. 엔진 층에는 SM3 셰이더와 그 타입만 더한다. 정식 장면 렌더러는 RS0 `Renderer`다(렌더러 17절).

1. **구조(SM1).**
   - `ScreenRenderer`(렌더 lane 처리기): lane 시작에 스왑체인(표면이 있으면) → `EngineRenderResources::Open`(9.12절) → 파이프라인 캐시·그래프 풀을 만든다. 종료에 역순으로 `Retire`한다.
   - `ScreenHost`(게임 lane): 장치(명령줄 `--graphics=auto|d3d12|d3d11|vulkan|opengl`, 기본 auto — 만들 수 있는 첫 장치를 D3D12 → Vulkan → OpenGL ES 순으로, 플랫폼 분기 없음)·`RenderLane`을 만들고 엔진 루프 hook에서 프레임마다 view와 장면 목록을 프레임 packet으로 넘긴다. 엔진 종료 hook 다섯을 lane 종료 단계(`BlockFrames` → `WaitFramesIdle` → `DrainRetires` → `CloseResources` → `Stop`)에 잇는다. 장치는 엔진 `Shutdown` 뒤, 창보다 먼저 파괴한다.
   - 프레임 그래프: `SceneColor`(`RGBA16Float`)·`SceneDepth`(`D32Float`, clear 0) → `BasePass`(내장 메시를 fallback 메시 셰이더로, 깊이 `GreaterEqual`) → `Tonemap`(전체화면 pass, `SwapChainTarget`). 스왑체인의 그리기 view가 sRGB(장치 계약)라 tone map은 선형 값을 쓰고 부호화는 view가 한다. UE는 UNORM 백버퍼에 셰이더가 sRGB 부호화를 하지만 결과 값은 같다.
   - 표면 작업: 창 크기 변경은 `PostSurface`(resize, 0 크기면 건너뜀), Android 표면 소실은 `PostSurfaceAndWait`(스왑체인 `Retire` 뒤 장치 대기) 뒤 `AcknowledgeSurfaceLost`, 새 표면은 다시 생성. 스왑체인이 없는 프레임은 packet을 보내지 않는다.
   - lane 시작 실패(예: Release·Android에 셰이더 공급자가 없어 `UnsupportedCapability`)는 Warning을 남기고 렌더 없이 엔진을 계속 돌린다(빈 프레임 경로). 실패를 숨기지 않되 입력·게임 시험 경로를 막지 않기 위해서다.
   - 장면: 이번 단계는 실행 파일이 고정 목록(바닥 plane, HDR 색 cube 셋, 내장 텍스처)을 만든다. World 추출은 렌더러 RS0(RQ20)이다. SM8은 텍스처 자원 쪽만 엔진 층으로 옮긴다(8항).
2. **view(SM2).** 게임 lane이 첫 LocalPlayer의 `CameraDirector::View()`로 double 행렬을 만든다: 항목 local → 카메라 상대 world(이동에서 카메라 위치를 뺌, 계획 2.2절) × `MakeViewRotation`(UE `FInverseRotationMatrix` × 축 바꿈) × `TryMakePerspective(fov/2, 폭, 높이, 10 cm, ∞)`(reversed-Z). 항목마다 float `localToClip`을 packet에 담는다(fallback 메시 셰이더 `BuiltinMeshConstants`). 컬은 뒤면(앞면 반시계 — 10.6절 3항 내장 메시 감김)이고 view 행렬식이 음수면 뒤집는다(UE `bReverseCulling`).
3. **tone map(SM3).** UE 5.8 기본 경로(`PostProcessCombineLUTs.usf` 색 경로를 LUT 없이 픽셀마다)와 같다: 장면 색 × 노출 배율 → 작업 색 공간 sRGB → AP1 → 넓은 색역 확장(`ExpandGamut` 1) → blue correction(0.6) → `FilmToneMap`(ACES glow·red modifier·AP1 desaturate·toe·shoulder) → `ToneCurveAmount`(1) → blue correction 되돌림 → sRGB(음수 0) → 스왑체인 sRGB view. 기본값은 UE `FPostProcessSettings`와 같다(slope 0.88, toe 0.55, shoulder 0.26, black clip 0, white clip 0.04 — `Engine/Private/Scene.cpp:436–450`). 색 보정(`ColorCorrectAll`)은 기본값에서 항등이라 넣지 않는다(SL8). 노출 배율은 packet 값이고 기본 1이다(자동 노출 SL8). 상수는 루트 상수 48 byte(`TonemapConstants`, 손으로 쓴 레이아웃 표)이고 타입 `TonemapPixel`은 RenderBase가 등록한다(RS0에서 `RendererShaders`로 옮긴다).
4. **입력(SM4).** SandboxPawn이 UE `ADefaultPawn` 바인딩을 따른다(`Engine/Private/DefaultPawn.cpp:94–126`): 앞뒤 W·S·위아래 화살표·왼쪽 스틱 Y, 좌우 A·D·왼쪽 스틱 X, 위아래 E·Space·남쪽 버튼·왼쪽 스틱 누름(+)과 Q·C·왼쪽 Ctrl·오른쪽 스틱 누름(−)(월드 Z), 회전율 좌우 화살표·오른쪽 스틱(45 °/s, UE `BaseTurnRate`). 속도는 1200 cm/s(UE `FloatingPawnMovement::MaxSpeed`)이고 입력 벡터를 길이 1로 자른다. 가속·감속(4000·8000)은 이동 컴포넌트 단계에서 넣는다. 마우스는 오른쪽 버튼을 누른 동안만 회전한다(창이 마우스를 가두지 않으므로 UE 편집 viewport 방식), 1 pixel당 0.175°(UE 기본 축 감도 0.07 × 컨트롤러 배율 2.5), pitch는 ±89.9°(UE `ViewPitchMin·Max`). 이동은 Pawn root, 회전은 Controller `controlRotation`에 쓴다.
5. **self-test(SM5).** 실행 파일 안에서 장치마다 숨긴 창과 Sandbox 게임(GameInstance·SandboxPawn)으로 엔진을 몇 프레임 돌린다. 마지막 프레임은 tone map을 capture 텍스처(`RGBA8UnormSrgb`)에도 그려 읽는다. 단언: (a) 화면 가운데 HDR 색 cube 픽셀 = CPU 기준 tone map(같은 식의 C++ 구현, 채널 ±2) (b) Thread·Inline 모드 픽셀 동일 (c) backend 사이 채널 ±8·2%(8절 기준) (d) 검증 오류 0, 남은 lane 작업·handle·장치 회수 0 (e) 주입한 W 입력 N 프레임 뒤 카메라 위치 = 1200 cm/s × N × 고정 delta(시간 운 없음).
6. **텍스처 표시(SM6).** `Debug/TextureInspector.rsf`에 차원별 PS(2D·2D array slice·cube 가로 십자 펼침·cube array 원소 펼침·volume z 단면)와 mip 선택을 두고 루트 상수 64 byte(`TextureInspectorConstants` — tone map 상수 셋 + mip·층·volume 깊이·표시 방식)로 받는다. 표시 방식: LDR은 선형 값 그대로, HDR은 SM3 곡선, 깊이는 빨강 채널을 회색으로(reversed-Z라 가까울수록 밝다). 정수 형식 표시는 `Texture2D<uint>` 엔트리가 따로 필요해 쿠킹 텍스처 형식과 함께 다음 단계(SM7 — 7항)에서 넣는다. 표시는 장면을 대신하는 전체화면 view다(스왑체인·capture). 입력(호스트): 숫자 0 장면, 1 2D, 2 HDR 2D, 3 2D 배열, 4 cube, 5 cube 배열, 6 volume, `[`·`]` 층(패드 어깨 버튼), `-`·`=` mip. 이번 단계는 lane 시작에 만든 절차 생성 텍스처(2D 4×4 mip 셋, HDR 1×1, 배열 층 셋, cube 면 여섯 색, cube 배열 둘, volume 깊이 넷)를 쓰고, self-test가 차원마다 픽셀을 단언한다.

**버린 대안.** (가) 게임 lane에서 장치를 직접 쓰는 렌더(lane 없이) — 18.1절 완료 기준 "lane 모드 = 인라인 모드"와 5절 구조를 건너뛴다. (나) Sandbox `SampleRenderer` 재사용 — Sandbox 시험 셰이더라 엔진 셰이더 집합·내장 자원 팩 경로를 거치지 않는다. (다) UNORM 스왑체인 + 셰이더 sRGB 부호화(UE 방식) — 스왑체인 계약(그리기 view sRGB)을 바꿔야 하고 결과 값이 같다. (라) 32³ LUT 굽기 — LUT 생성 pass는 색 보정과 함께 SL8이 맡는다(엔진 셰이더 A17 "RS0은 LUT 없이 곡선 직접").

**구현(개정 46, 2026-11-25).** SM1–SM6을 위 설계대로 구현했다.
- 파일: 엔진 셰이더 `Engine/Shaders/Private/Color/FilmicToneCurve.rsh`·`PostProcess/TonemapPass.rsf`·`Debug/TextureInspector.rsf`, RenderBase `EngineShaderTypes.h`(`TonemapPixel`·`TonemapConstants`, `TextureInspector*Pixel` 다섯·`TextureInspectorConstants`·`TextureInspectorDisplay`), Sandbox `ScreenRenderer`(lane 처리기·프레임 그래프·표면 작업·표시 텍스처)·`ScreenHost`(장치·lane·view·장면·종료 hook·표시 입력)·`ScreenSelfTest`, `SandboxGame`(DefaultPawn 방식 이동), `EngineHost`·`Main`(`--graphics=`·`--graphics-validation`·`--no-render`).
- 설계와 다르게 정한 것: (1) 기본 backend는 플랫폼 분기 대신 `auto`(D3D12 → Vulkan → OpenGL ES 순으로 만들 수 있는 첫 장치 — 실행 파일에도 플랫폼 경계 정책이 걸린다). (2) `EngineRenderResources::Retire`가 lease 없는 집합 객체를 그 자리에서 놓는다(9.12절 2항 — 엔진 묶음의 공급자 모듈이 `Engine::Shutdown` 끝에서 내려갈 때 그 저장소가 남아 있으면 Fatal이었다). (3) 0인 채널의 `log10`을 `1e-10`으로 막았다(UE는 NaN이 나는 자리 — UE-29935 주석).
- 함께 고친 것: `Sandbox --engine`이 설정 단계에서 `Settings of module Collision could not be registered: NotFound`로 실패했다(CD1). 원인은 `HostSettings::RegisterModuleSettings`가 모듈의 `RegisterSettings`를 부르기 전에 그 모듈의 리플렉션 타입을 설정 문맥의 타입 등록에 넣지 않는 것이다(스스로 넣는 것은 Config·TraceControl뿐 — UE의 "적재되지 않은 모듈 구획 무시"와는 다른 문제다: Collision 모듈은 적재되어 있다). HostConfig는 다른 세션 몫이라 Sandbox가 `HostSettingsOptions::registerModuleTypes = RegisterCollisionTypes`로 막힘을 풀었다. 설정 struct를 올리는 모든 모듈의 타입을 HostSettings가 모듈 표 순서로 넣는 정공법은 HostConfig 세션에 넘긴다(Commandlet 호스트도 같은 문제가 있다).
- 시험(Sandbox `--self-test`, Debug): 여덟 장치(D3D12·D3D12 WARP·D3D11·D3D11 WARP·OpenGL ES·GL core 위 ES·Vulkan·lavapipe)에서 엔진 창 경로 Thread·Inline 두 모드 — 장면 HDR cube·과노출 cube·Pending 텍스처 typed 대체·배경 = CPU tone map(±2), 텍스처 표시 2D mip 0·1·HDR·배열 층·cube 십자 다섯 칸·cube 배열 원소·volume z 단면, 두 모드 픽셀 동일, 검증 오류 0, lane 작업·handle·장치 회수 0. 헤드리스 엔진의 카메라 입력(W 6 프레임 120 cm, 오른쪽 화살표 6 프레임 4.5°). 엔진 호스트 자식 다섯 항목 PASS.

7. **쿠킹 텍스처(SM7, 개정 48).** 6.5절 계약의 텍스처를 GPU에 올려 텍스처 표시로 그리고, 다섯 차원·형식별 장면을 대조한다(18.1절 완료 기준 3항). 텍스처 스트리밍·압축은 범위 밖이다(18.1절) — 상주 꼬리와 스트리밍 mip bulk를 모두 읽어 한 번에 올린다.
   - **엔진 층.** RenderBase `CookedTexture.h`(6.5절 "업로드")와 엔진 셰이더 둘이다. `Material/MaterialTextureSampling.rsh`(엔진 셰이더 A05)에 sampler type 복제 `ReplicateRedChannel`과 쿠킹 swizzle 적용 `ApplyCookedChannelSwizzle`을 먼저 둔다(6.5절 "swizzle의 뜻과 적용" — 파일의 나머지 내용은 SL2 그대로). `Debug/TextureInspector.rsf`는 그것을 거친다.
   - **TextureInspector 확장.** 상수를 96 byte로 늘린다(`TextureInspectorConstants`): 앞 64 byte는 그대로이고, `swizzle`(쿠킹 swizzle 네 칸 — `SwizzleSource` 번호 0 R·1 G·2 B·3 A·4 0·5 1)과 `integer`(x = 정수 값 → 표시 값 배율)를 더한다. `selection.z`는 고른 mip의 volume 깊이다(SM6은 mip 0 깊이를 넣어 mip > 0 volume 단면이 어긋났다). 정수 형식 엔트리 여섯을 더한다: 2D·2D 배열·volume × uint·int(`Texture2D<uint4>` 등). texel을 `Load`로 읽고(정수 형식은 필터 표본이 없다), swizzle 뒤 배율을 곱해 LDR로 표시한다. cube·cube 배열 정수 형식은 표시하지 않는다: HLSL `TextureCube`에는 `Load`가 없다. 쿠킹은 정수 형식을 만들지 않고(AssetSystem 12.4.3절 출력 형식), 업로드 자체는 그 차원도 받는다.
   - **표시 목록(Sandbox).** 표시는 RenderHandle 하나와 표시 값(차원·성분 계열 float·uint·int·표시 방식·swizzle·정수 배율·층 수·mip 수·mip별 volume 깊이)이다. 게임 lane의 `ScreenHost`가 목록을 갖고, 프레임 packet에 고른 항목을 담는다. 렌더 lane은 handle이 Ready면 그 텍스처로, Pending·Failed면 차원·성분 계열이 맞는 typed 대체로 그린다(6.2절). 맞는 대체가 없는 조합(정수 2D 배열·volume)은 검정으로 그리고 대체 계수를 올린다.
     - 엔진 절차 텍스처(SM6 여섯)는 메모리 `TexturePlatformData`로 만들어 같은 업로드 경로로 올린다. lane 시작의 직접 생성은 없앤다. SM6 표시 시험 기대값은 그대로다.
     - 쿠킹 텍스처: EngineAssets가 있으면(`--set=asset.cooked_root=<쿠킹 루트>` 또는 엔진 쿠킹 루트) 매니페스트에서 대표 타입이 Texture 파생인 항목을 경로 순서로 요청한다. 준비 callback(패키지 공개, 게임 lane)에서 `TexturePlatformData`와 패키지 bulk를 `CookedTextureSource`로 바꿔 `PrepareCookedTexture` → `CreateCookedTextureOnLane`으로 올린다. lease는 handle `Retire` 뒤 엔진 종료 hook에서 놓는다(EngineAssets 종료 계약 11.8절 6).
     - CPU Availability 텍스처는 올리지 않고 표시가 `Black2D` 대체다(6.5절).
     - 변환(`TexturePlatformData` → `CookedTextureSource`): 차원 Texture2D·TextureCube·Texture2DArray·TextureCubeArray·Volume → GraphicsTypes 차원, `pixelFormat` 번호 → `PixelFormatFromNumber`, 기준 크기 sizeX·sizeY, depth는 volume의 sizeZ(아니면 1), slice는 `numSlices`(volume 1), 샘플러 필터 다섯·주소 넷·border 셋 → GraphicsTypes 같은 값, swizzle 넷 → `SwizzleSource` 같은 번호. 스트리밍 mip bulk는 `Package::BulkBytes`, 꼬리 덩어리는 `tailBulk`다. bulk를 읽지 못하면 `CorruptEntry`와 같은 처리(올리지 않고 Failed)다.
   - **입력.** SM6 그대로(0 장면, 1–6 절차 텍스처, `[`·`]` 층, `-`·`=` mip)에 `,`·`.`(패드 D-pad 왼쪽·오른쪽) 목록 전체의 이전·다음을 더한다(쿠킹 텍스처 포함). 고른 항목 이름·층·mip을 Info 로그로 남긴다.
   - **시험 콘텐츠.** `Programs/Sandbox/TestContent/`가 프로젝트다(`SandboxTestContent.rocksproject`). 원본은 `Sources/`(무손실 PNG·Radiance HDR), 에디터 패키지는 `Content/Textures/*.rasset`이고 둘 다 커밋한다. `Saved/`·`Intermediate/`는 gitignore다. 쿠킹은 `Commandlet asset-cook --profile=Windows --project=Programs/Sandbox/TestContent`(결과 `TestContent/Saved/Cooked/Windows`)다. 텍스처는 다섯 차원 × LDR(sRGB 켬·끔)과 2D 형식 대표(회색 Grayscale R8 `.rrrr`, 회색 sRGB, HDR `RGBA16Float`, HDR_F32 `RGBA32Float`, HalfFloat `R16Float`, 16비트 PNG Masks `RGBA16Unorm`, CPU Availability)다. 원본 픽셀은 사분면 색(2D·배열·volume)과 면마다 한 색(cube)이라 표본점이 필터와 무관하게 한 texel 값이다.
   - **self-test(장치: D3D12·WARP·D3D11·D3D11 WARP·Vulkan·lavapipe·GLES 미리보기·GL core 위 ES).**
     - (가) 메모리 플랫폼 데이터 장면: 다섯 차원 × `RGBA8Unorm`·`RGBA8UnormSrgb`·`RGBA16Float`·`RGBA32Float`·`R8Unorm`(`.rrrr`) 와 2D·2D 배열·volume × `R8Uint`·`RGBA16Sint`. 장면마다 고른 mip·층의 표본점 픽셀 = CPU 기준(형식 byte decode → 쿠킹 swizzle → 표시 방식(LDR 그대로·HDR filmic·정수 배율) → capture sRGB 부호화, 채널 ±2). 장치가 형식·차원을 못 하면(`UnsupportedCapability`) 그 장면만 NOT RUN 사유를 남긴다.
     - (나) 쿠킹 장면: 시험 콘텐츠 쿠킹이 있으면 엔진(EngineAssets, 쿠킹 루트 지정)을 돌려 텍스처를 요청하고, 모두 Ready가 된 뒤(통지 gate — 프레임 수에 기대지 않는다) 텍스처마다 표시 프레임을 그려 같은 CPU 기준(쿠킹 byte에서 계산)으로 대조한다. CPU Availability는 검정이다. 쿠킹이 없으면 NOT RUN(사유에 쿠킹 명령). GLES는 Windows 쿠킹 대상 backend가 아니다: 사유를 남기되, 장치가 그 형식을 올릴 수 있는 장면은 그대로 대조한다.
     - 공통: Thread·Inline 두 모드 픽셀 동일, backend 사이 표본점 채널 ±8(8절 기준 — 처음 돈 장치가 기준), 검증 오류 0, lane 작업·handle·장치 회수 0.
     - EngineTests `CookedTexture_*`: 양성(다섯 차원 desc·크기, 꼬리 패딩을 뺀 묶기, BC·ASTC 블록 크기 — ASTC 올림, 샘플러 바꿈·이방성 0 → 4), 음성(잘린 bulk·잘린 꼬리·mip 크기 사슬·byte 수 불일치·꼬리 정렬·겹침·형식 sRGB 불일치·모르는 형식·깊이 형식·BC 블록 배수 아님·형식 능력 없음·cube array 기능 없음·장치 한도 초과), lane 작업의 Ready·Failed 통지.
   - **버린 대안.** (가) RenderBase가 Texture 모듈에 기대어 `TexturePlatformData`를 직접 받기 — RenderBase가 에셋 타입 모듈에 묶이고, 텍스처 에셋을 모르는 UE RenderCore 층과 어긋난다. (나) view swizzle 적용 — 6.5절 합의. (다) 렌더 lane이 패키지 bulk를 직접 읽기 — Package·bulk는 게임 스레드 전용이다(AssetRuntime `Package.h`). (라) self-test가 그 자리에서 쿠킹(TextureTools)하기 — 실행 파일이 Developer 쿠커 모듈에 기대게 되고 쿠킹 명령 경로를 거치지 않는다. 메모리 장면(가)이 쿠킹 없이 형식 범위를 덮는다.

**구현(개정 49, 2026-11-26).** 7항을 위 설계대로 구현했다(커밋 e5032c7f·6dbb98d6·183fe524).
- 파일: RenderBase `CookedTexture.h`(`CookedTextureSource`·`CookedTextureIssue`·`CheckCookedTexture`·`PackCookedTexturePixels`·`PrepareCookedTexture`·`CreateCookedTextureOnLane`), `RenderNotices.h`의 `RenderBlob::TryAllocate`·`MutableView`(조각을 이어 채우는 lane 소유 byte — 묶기가 두 번 복사하지 않게), `EngineShaderTypes.h`(상수 96 byte, 정수 엔트리 여섯 `TextureInspector{2D,2DArray,Volume}{Uint,Int}Pixel`), 엔진 셰이더 `Material/MaterialTextureSampling.rsh`·`Debug/TextureInspector.rsf`. Sandbox: `PlatformTextureSource`(변환), `MemoryPlatformTexture`(쿠킹 배치의 메모리 플랫폼 데이터), `TexelCodec`(texel 부호화·해석), `CookedTextureLibrary`(매니페스트 텍스처 요청 → 준비 callback → 업로드), `ScreenHost` 표시 목록(`InspectorEntry`·`AddTexture`·`AddUnchecked`·`ProcessNotices`), `ScreenRenderer` 차원·성분 계열 열한 파이프라인, `CpuDisplayColor`(SM5 CPU tone map을 옮겨 텍스처 표시 기준과 함께 씀), `TextureSceneCapture`·`TextureSceneSelfTest`·`CookedTextureSelfTest`, `Main`의 `--texture-self-test`(텍스처 장면만).
- 설계와 다르게 정한 것: (1) 표시 목록 이동은 `,`·`.` 끝에서 장면(0)으로 돌아간다(목록 끝 → 장면 → 처음). (2) 메모리 장면의 선형 필터 장면은 고른 mip의 사분면이 2 texel 이상인 것만 둔다(사분면이 1 texel인 mip에서 bilinear는 이웃 사분면과 섞인다 — 첫 실행에서 모든 backend가 같은 값 187로 섞여 CPU 기준 쪽 장면 정의를 고쳤다). (3) `RenderLane` 통지는 `ScreenHost::ProcessNotices`가 프레임마다 거둔다(전에는 아무도 거두지 않아 FrameCompleted가 쌓였다).
- 시험 콘텐츠(`Programs/Sandbox/TestContent`): 원본 `Sources/`(PNG 8·16 bit, Radiance HDR — JPEG 없음), 패키지 `Content/Textures/*.rasset` 13개, `Config/CookSettings.json`(`alwaysCook: ["/Game/Textures/"]` — 경로 인자 없이 쿠킹 대상이 정해진다). 만든 명령(프로젝트 폴더에서, Git Bash는 `MSYS_NO_PATHCONV=1`): `asset-import /Game/Textures/<이름> Sources/<파일> [--srgb=off] [--compressionSettings=Grayscale|Masks|HDR|HDR_F32|HalfFloat] [--filter=Nearest --addressX=Clamp --addressY=Clamp] [--availability=CPU]`, cube `--cube=px:…,nx:…`, 배열 `--array=…`, cube 배열 `--cube-array=…;…`, volume `--volume=…`. 쿠킹은 `RocksCommandlet asset-cook --profile=Windows --project=Programs/Sandbox/TestContent`(13 targets — 결과 `Saved/Cooked/Windows`, gitignore). 에셋 세션 코드는 고치지 않았다.
- 시험(Sandbox `--texture-self-test`·`--self-test`, Debug): 여덟 장치(D3D12·D3D12 WARP·D3D11·D3D11 WARP·OpenGL ES·GL core 위 ES·Vulkan·lavapipe)에서 Thread·Inline 두 모드.
  - (가) 메모리 장면 34개 — 153 표본점이 CPU 기준(±2)과 같고, 처음 돈 D3D12와 다른 일곱 장치가 표본점 ±8 안(실측은 모두 같은 값), 음성(잘린 bulk BulkSize·잘린 꼬리 TailRange·mip byte MipByteSize·한도 DeviceLimit·ASTC4x4 UnsupportedFormat — GLES 두 장치는 ASTC·ETC2·BC7을 모두 샘플링해 NOT RUN, 장치 생성 실패 ResourceFailed → typed 대체 흰색, 정수 cube 검정), 검증 오류 0, lane·handle·회수 0.
  - (나) 쿠킹 장면 13개 — 53 표본점이 쿠킹 byte에서 계산한 CPU 기준과 같다(CPU Availability는 검정). GL core 위 ES는 `RGBA16Unorm`(Masks 16 bit)을 샘플링하지 못해 그 텍스처만 NOT RUN 사유(쿠킹 대상 backend 아님)를 남긴다. lavapipe는 쿠킹 장면을 돈 실행에서 데스크톱 세션이 끊겨 창 경로 전체가 NOT RUN이었다(`WindowPresentUnavailableReason` — 같은 날 앞선 실행에서 메모리 장면은 lavapipe도 PASS). 개정 51(RR12)에서 다시 돌려 lavapipe 쿠킹 장면 13개·53 표본점이 Thread·Inline 두 모드 모두 PASS다.
  - EngineTests `RenderBase_CookedTexture*` 일곱(위 7항 목록), `RenderBase_*` 20, `EngineShaders_CompileMatrix`(정수 엔트리·swizzle 함수가 모든 형식에서 컴파일).

8. **WorldIntegration 텍스처 렌더 캐시(SM8, 개정 54).** 18.1절 "통합" 행의 텍스처 컴포넌트다. 텍스처 에셋(AssetId)마다 GPU 자원 하나를 만들고, `RenderHandle`과 표시 값을 쓰는 쪽(프레임 packet을 만드는 쪽)에 준다. 6.5절 "업로드"의 변환 층을 Sandbox에서 엔진 층으로 옮긴다.
   - **UE 5.8 대응.** 텍스처 GPU 자원은 에셋이 가진다.
     - 만들기: `UTexture::PostLoad`가 `UpdateResource`를 부른다(`Engine/Private/Texture.cpp:1292, 1327`). `UpdateResourceWithParams`가 `CreateResource`로 `FTextureResource`를 만들고, `ENQUEUE_RENDER_COMMAND(SetTextureReference)`로 렌더 스레드에서 `InitResource`한다(`:330, 442–454`).
     - 해제: `ReleaseResource`가 `DeleteResource` 명령을 넣는다(`:300–327`). 파괴 때는 `BeginFinalReleaseResource`가 `ReleaseFence`를 걸고 `IsReadyForFinishDestroy`가 그것을 기다린다(`:1332–1347, 1365`).
     - 컴포넌트는 GPU 자원을 갖지 않는다. billboard proxy는 `Texture->GetResource()`를 읽기만 한다(`Engine/Private/Components/BillboardComponent.cpp:115`). 텍스처 편집기 viewport도 컴포넌트 없이 그 자원을 그린다(`Editor/TextureEditor/Private/Models/TextureEditorViewportClient.h`).
     - RenderCore는 텍스처 에셋을 모른다.
   - **결정: "텍스처 컴포넌트"는 WorldIntegration `TextureRenderCache`다.** 리플렉션 World 컴포넌트는 새로 만들지 않는다. 이유는 셋이다.
     - UE에는 텍스처만 보여 주는 일반 컴포넌트가 없다. 가장 가까운 `BillboardComponent`는 EditorOnly이고, CM2 묶음에서 icon sprite 렌더(CR23·RS13)와 함께 들어온다([컴포넌트 도입](../ComponentCatalog/03-RequiredComponents.ko.md) 3.11절).
     - 텍스처 자원은 에셋마다 하나다. 컴포넌트마다 두면 같은 텍스처가 중복된다. [엔진 런타임](../EngineRuntime/11-Assets.ko.md) 11절도 "WorldIntegration 텍스처 캐시가 AssetId별로"다.
     - 엔진 층 원칙상 자리표시 컴포넌트를 두지 않는다.

     첫 World 사용자는 `BillboardComponent`(CM2)와 머티리얼 텍스처 파라미터(MS)다. 렌더 상태를 만들 때(UE `CreateRenderState_Concurrent`) `Acquire`하고, 파괴할 때 `Release`한다. Sandbox 텍스처 표시는 UE 텍스처 편집기 viewport 자리라 캐시를 직접 쓴다.
   - **파일.** `Engine/Source/Runtime/WorldIntegration/Public/WorldIntegration/Textures/`에 둔다.
     - `PlatformTextureSource.h`: Sandbox에서 옮긴 변환이다(`TexturePlatformData` + bulk reader → `CookedTextureSource`·쿠킹 swizzle). 동작은 7항 "변환"과 같다. 차원 함수 이름만 `GraphicsTextureDimension`으로 바꾼다(`rocks` 이름공간에서도 뜻이 드러나게).
     - `TextureRenderCache.h`: 캐시다.
     - 모듈 의존에 `Texture`와 `RenderBase`를 더한다. `Texture`는 Texture 타입과 `TexturePlatformData`를 주고, AssetRuntime은 그것을 거쳐 들어온다. `RenderBase`는 lane과 `CookedTexture.h`를 준다.
     - EngineAssets에는 기대지 않는다. 캐시는 `AssetManager&`를 받는다(엔진은 `EngineAssetsSubsystem::Manager()`, 시험은 직접 만든 AssetManager).
   - **소유와 수명.**
     - 렌더 lane과 장치를 가진 쪽이 만든다. 지금은 Sandbox 엔진 호스트다. RS0 뒤에는 엔진 렌더 통합(WorldIntegration RenderScene 추출 — 렌더러 RQ20)이 가진다.
     - EngineSubsystem으로 두지 않는다. Engine·World는 Graphics를 모르고, lane은 실행 파일이 만든다([엔진 런타임](../EngineRuntime/03-Modules.ko.md) 3절).
     - 준비 callback이 캐시를 가리키므로 캐시는 AssetManager Shutdown(엔진 Shutdown) 뒤까지 산다. 엔진보다 먼저 선언해 엔진보다 뒤에 파괴한다.
     - 스레드는 게임 lane 전용이다(AssetManager 소유 스레드).
   - **항목과 상태.** 키는 AssetId다. 값은 사용자 수·티켓·lease·`RenderHandle`·상태·오류(`ErrorCode` + `CookedTextureIssue`)·표시 값(`TextureRenderInfo`)이다. 표시 값은 차원·형식·쿠킹 swizzle·폭·높이·깊이·slice 수·mip 수다. 상태는 다섯이다.
     - `Loading`: 요청했고 lease를 기다린다. handle이 없다.
     - `Uploading`: 예약한 handle로 업로드를 게시했다. 이때 그리면 typed 대체다(6.2절).
     - `Ready`: `ResourceReady` 통지를 받았다.
     - `Failed`: 두 경우다. 로드·변환·검사·게시가 실패했으면 handle이 없다. `ResourceFailed`를 받았으면 handle이 있다. 그 handle로 그리면 typed 대체이고, `Release`가 Retire한다.
     - `CpuPlaceholder`: CPU Availability다. 올리지 않으므로 handle이 없고, 그리는 쪽이 `Black2D`로 대체한다(6.5절). lease는 사용자가 있는 동안 쥔다(CPU 데이터를 읽는 쪽이 쓴다).
   - **흐름.**
     - `Acquire(id)`: 사용자 수를 올린다. 첫 `Acquire`면 `RequestAsset`(기대 타입 `Texture`)과 준비 callback(`TaskLane::Game`)을 건다. 요청 자체가 실패하면 항목을 만들지 않고 그 오류를 돌려준다. 종료 뒤면 `ShuttingDown`이다.
     - 준비 callback: lease를 쥐거나 실패(`Failed`)를 적고, 업로드는 하지 않는다.
       - callback은 Pump 안(엔진 3단계)에서 불린다. 그래서 lease 객체 접근(`AssetManager::With`)은 Pump 밖에서 한다.
       - 항목마다 순번(serial)이 있고, callback은 순번으로 항목을 찾는다. 그래서 `Release`·`Shutdown` 뒤 늦게 온 callback(놓은 티켓의 `Cancelled`, 종료의 `ShuttingDown`)은 아무것도 하지 않는다.
     - `Update()`: 프레임마다 게임 lane에서 부른다. Sandbox는 엔진 `EndFrame` hook에서 부르며, 같은 프레임 Pump 뒤라 지연이 없다. lease가 온 `Loading` 항목을 다음 순서로 처리한다.
       1. 텍스처·패키지를 연다.
       2. CPU Availability면 `CpuPlaceholder`로 끝낸다.
       3. `MakePlatformTextureSource`로 바꾼다(bulk는 `Package::BulkBytes`).
       4. `ReserveHandle` → `PrepareCookedTexture` → `PostResource(CreateCookedTextureOnLane)`.
       5. `Uploading`이 된다.

       handle을 예약한 뒤 실패하면 그 handle을 곧바로 Retire하고 `Failed`가 된다.
     - `HandleNotices(notices)`: lane 소유자가 `RenderNotices::TakeAll`로 거둔 통지를 넘긴다. 통지 큐의 소비자는 하나다(5.2절). 캐시 handle의 `ResourceReady`·`ResourceFailed`만 보고, 다른 통지는 무시한다.
     - `Find(id, out)`: 표시 값 사본을 준다.
       - 프레임 packet에는 이 값(handle + 표시 값)을 담는다. 캐시 포인터는 lane에 넘기지 않는다(1절 "프레임 packet과 handle만").
       - 렌더 lane은 handle을 6.2절 규칙대로 그린다.
       - `CpuPlaceholder`는 handle이 없으므로 packet을 만드는 쪽이 `Black2D`를 고른다.
     - `Release(id)`: 사용자 수를 내린다. 0이 되면 handle을 Retire하고, lease·티켓을 놓은 뒤 항목을 지운다(6.5절 "lease를 놓기 전에 handle을 Retire"). 모르는 id는 `NotFound`다.
       - 장치 자원은 패키지 byte를 빌리지 않는다(`PrepareCookedTexture`가 `RenderBlob`으로 복사한다). 그래서 Retire를 게시하는 것만으로 순서가 서고, `ResourceRetired`는 기다리지 않는다.
       - 회수는 lane FIFO와 장치 fence가 맡는다(UE `ReleaseFence` 자리 — 6.2절).
     - `Shutdown()`: 모든 handle을 Retire한 뒤 모든 lease·티켓을 놓는다. 이후 `Acquire`는 `ShuttingDown`, `Update`는 무동작이다. 소멸자도 `Shutdown`을 부른다.
       - 엔진 종료 hook `BeginShutdown`에서 부른다. 이때는 `BlockFrames` 전이라 lane이 아직 Retire를 받고, 종료 3단계 `DrainRetires`가 회수한다.
       - EngineAssets 종료 계약도 지킨다. `EngineLoopHooks`는 lease를 `CompleteInFlightWork`까지 놓아야 한다([AssetSystem](../AssetSystem/11-RuntimeLoading.ko.md) 11.8절 6).
     - `WithPlatformData(id, visit)`: lease가 있는 항목의 쿠킹 데이터와 bulk reader를 방문하는 동안 빌려 준다. 도구·self-test가 CPU 기준값을 만들 때 쓴다.
   - **Sandbox로 남는 것.**
     - 메모리 플랫폼 데이터 업로드(엔진 절차 텍스처·메모리 장면)는 에셋이 아니다. 그래서 캐시를 거치지 않고 옮긴 변환만 쓴다(`ScreenHost::AddTexture`).
     - `CookedTextureLibrary`는 매니페스트의 텍스처 항목을 경로 순서로 `Acquire`하고, `Uploading` 이후 항목을 표시 목록에 더한다(`ScreenHost::AddCachedTexture`). 그 handle은 캐시 것이라 ScreenHost가 Retire하지 않는다. self-test 기준 데이터는 `WithPlatformData`로 읽는다.
     - ScreenHost는 거둔 통지를 표시 목록과 캐시에 함께 넘긴다.
     - 종료 hook `BeginShutdown`은 `TextureRenderCache::Shutdown` → `ScreenHost::BlockFrames` 순으로 부른다.
   - **한계.**
     - 스트리밍 없음: 모든 mip을 한 번에 올린다(18.1절).
     - 에셋 다시 읽기·교체 hook 없음: lease를 쥐는 동안 패키지는 상주한다. 핫 리로드는 에셋 쪽 교체 통지가 생길 때 더한다.
     - 장치 재생성 없음(RR18).
     - 캐시 하나가 lane 하나를 쓴다.
     - 컴포넌트별 샘플러 override 없음: 쿠킹 샘플러를 그대로 쓴다(UE도 샘플러는 텍스처 자원에 있다).
   - **시험(EngineTests `WorldIntegration_TextureRenderCache*`·`WorldIntegration_PlatformTextureSource*`).** 순서가 결정적인 것만 쓴다. 로더 IO·worker는 손으로 돌리고(AssetRuntime 시험 도구), lane은 Inline 기록 장치이며, 통지는 lane 표지를 기다려 거둔다.
     - 변환 양성: 다섯 차원 대응, volume depth·slice 규칙, 샘플러·swizzle 값.
     - 변환 음성: 모르는 차원·형식 번호·샘플러 enum·swizzle, mip 수 불일치, 빈 bulk ref(스트리밍 mip·꼬리), reader 오류 전달.
     - 캐시 양성:
       - 쿠킹 텍스처 패키지 로드 → `Update` → `Uploading`(handle 예약, 장치 생성 한 번) → 통지 → `Ready`, 표시 값 대조.
       - 같은 id를 두 번 `Acquire`해도 요청은 하나고, 첫 `Release` 뒤에도 `Ready`다.
       - 마지막 `Release`가 Retire한 뒤(lane handle 0, 장치 회수) lease를 놓는다(패키지 내림).
     - 캐시 음성:
       - CPU Availability → `CpuPlaceholder`(handle·장치 생성 없음).
       - 빈 bulk ref → `Failed`(InvalidArgument, handle 없음).
       - mip byte 수 불일치 → `Failed`(`MipByteSize`, 예약한 handle Retire).
       - 장치 생성 실패 → `ResourceFailed` → `Failed`(handle 있음), `Release`가 Retire.
       - 모르는 AssetId·로드 실패 → `Failed`.
       - `Loading` 중 `Release` → 늦은 callback 무시, 업로드 없음.
       - `Release` 뒤 다시 `Acquire` → 옛 callback은 순번이 맞지 않아 무시.
       - 다른 handle의 통지 무시.
       - `Shutdown` 순서: Retire 게시가 lease 놓기보다 먼저이고, 이후 `Acquire`는 `ShuttingDown`.
     - Sandbox: `--texture-self-test`·`--self-test`가 여덟 장치에서 쿠킹 장면(나)을 캐시 경로로 돌려 같은 결과를 낸다(13 텍스처·53 표본점).
   - **버린 대안.**
     - (가) Rocks 고유 리플렉션 `TextureComponent`(World 컴포넌트가 텍스처를 들고 handle을 예약): UE 대응이 없는 자리표시이고, 컴포넌트마다 GPU 자원이 중복된다.
     - (나) `BillboardComponent`를 지금 넣기: EditorOnly·CM2다. icon sprite 렌더(CR23·RS13) 없이 데이터만 넣으면 자리표시다.
     - (다) 캐시를 EngineSubsystem으로 두기: Engine은 Graphics·lane을 모른다.
     - (라) 준비 callback 안에서 곧바로 업로드: callback은 Pump 안이라 lease 객체 접근 범위가 겹친다. 같은 프레임 hook에서 올려도 지연이 없다.
     - (마) `ResourceRetired`까지 lease를 쥐기: GPU 자원이 패키지 byte를 빌리지 않으므로 필요 없다. 게다가 종료 때 lease는 `CompleteInFlightWork`까지 놓아야 하는데 회수 대기(`DrainRetires`)는 그 뒤 단계라 계약과 부딪힌다.

**구현(개정 55, 2026-11-26).** 8항을 위 설계대로 구현했다.
- 파일: WorldIntegration `Textures/PlatformTextureSource.h`(Sandbox에서 옮김 — `GraphicsTextureDimension`), `Textures/TextureRenderInfo.h`(`TextureRenderState`·`TextureRenderInfo`), `Textures/TextureRenderCache.h`. 모듈 의존에 Texture·RenderBase. RenderBase 시험 기록 장치에 형식 능력·텍스처 생성 오류 주입(`SetPixelFormatCaps`·`SetCreateTextureError`). Sandbox `CookedTextureLibrary`(캐시를 가지고 `Acquire`·`Update`·`Release`), `ScreenHost::AddCachedTexture`·`AttachTextureCache`(거둔 통지를 캐시에 넘김, 캐시 handle은 Retire하지 않음)·`Device()`.
- 설계와 다르게 정한 것: (1) 캐시는 Sandbox `CookedTextureLibrary`가 가진다(엔진 호스트·쿠킹 self-test 둘 다 이 목록을 쓰므로). 목록 `Release`가 사용을 놓고 캐시 `Shutdown`을 부른다(종료 hook `BeginShutdown`). (2) 항목 표는 AssetId 해시 표, 통지 대조는 handle → AssetId 해시 표다(텍스처 수에 비례하지 않게).
- 시험(Debug):
  - EngineTests `WorldIntegration_PlatformTextureSource*` 셋·`WorldIntegration_TextureRenderCache*` 일곱이 모두 PASS다(위 8항 시험 목록). `RenderBase_*`·`RenderGraph*`·`MeshComponent_*`도 그대로 PASS다.
  - Sandbox `--texture-self-test`: 여덟 장치(D3D12·D3D12 WARP·D3D11·D3D11 WARP·OpenGL ES·GL core 위 ES·Vulkan·lavapipe) Thread·Inline 두 모드 0 failures. 쿠킹 장면은 캐시 경로로 13 텍스처·53 표본점이다. GL core 위 ES만 49 표본점이다: `RGBA16Unorm` 하나가 NOT RUN이고, 사유는 SM7과 같다.
