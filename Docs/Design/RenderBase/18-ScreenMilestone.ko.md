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
| 셰이더 시스템 | 6절 4항 순서의 P2·P8·P10·P3·P4·SH0·P6a·P5·SH1·P14·P9·P11·P6b·SH2·P7·SH3·SH4. 결정 2–7과 SX1–SX14를 그 기한에 정한다 |
| Graphics | GX1–GX11. VulkanGraphics(SH2), OpenGLGraphics의 GLES 미리보기 전환(P11) |
| RenderBase | RB1–RB7 |
| RenderGraph | RG1–RG2 |
| 텍스처 표시 | 6.5절 계약으로 쿠킹된 A5 텍스처를 GPU 자원으로 만들어 그린다. 다섯 차원 전부다: 2D 사각형, Cube 하늘 상자, 2DArray·CubeArray는 slice 선택, Volume은 단면. 모든 비압축 형식(HDR은 tone map pass로 표시), 샘플러 설정·swizzle·sRGB, CPU Availability의 대체 표시도 포함한다 |
| 통합 | WorldIntegration 텍스처 컴포넌트가 `RenderHandle`을 예약하고 프레임 packet으로 넘긴다. Sandbox에서 키보드·마우스·게임패드로 텍스처·slice·카메라를 바꾼다 |

**범위 밖(다음 단계).**
- Forward+·Deferred 조명 경로와 GI(계획 11·14절).
- 메시 에셋. 내장 메시만 쓴다.
- 텍스처 압축·색 보정(에셋 세션)과 텍스처 스트리밍(렌더 세션, 마일스톤 다음 단계). 6.5절 계약의 상주 꼬리 덩어리는 마일스톤에서도 그대로 읽는다.
- D3D11·Metal(SH5), Android(SH6).
- RG3–RG5, GX12·GX13.

**완료 기준.**
- 셰이더 시스템 SH0–SH4 완료 기준.
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

SH2e3(RB3 셰이더 집합·RB4의 SH2분)·GX3·GX4(개정 37)·GX6(개정 39) 뒤에 스펙(5·6·9–11절)과 코드(`Engine/Source/Runtime/RenderBase`·`RenderGraph`·`Graphics`)를 대조한 결과다. "결정"은 권장안으로 확정했고(애매하면 UE 5.8), "순서"는 화면 마일스톤(18.1절)에 필요한 순서다. 1–4는 이 개정에서 구현한다.

| ID | 단계 | 항목 | 지금 | 결정 | 순서 |
|---|---|---|---|---|---|
| RR1 | RB6 | 상태 프리셋(11.2절) | 없음(`AlphaBlend()` 하나) | 구현(11.4절 1항) | 1 |
| RR2 | RB6 | 전체화면 pass: 엔진 셰이더 `FullscreenTriangle`·RenderBase 그리기 도우미·그래프 `AddFullscreenPass`(11.2절) | Sandbox 시험 셰이더만 있음 | 구현(11.4절 2·3항) | 1 |
| RR3 | RB6 | RenderBase 경로 null 텍스처 바인딩 0건 계수(11.3절) | 장치가 null을 흰 텍스처로 바꿀 뿐 세지 않음 | 구현: 장치 계수 `NullTextureBindings`(11.4절 4항) | 1 |
| RR4 | RB5 | 정점 입력 타입 등록(늦은 등록 거부·결정성, 10.2·10.3절) | 등록부 없음 — 셰이더 집합·쿠커가 호출자 목록을 받음 | 구현: 셰이더 타입 등록부에 함께 둔다(10.6절 1항) | 2 |
| RR5 | RB5 | include 연결 `/Engine/Generated/VertexInput.rsh`(10.2절) | 없음 — job이 정점 입력 타입을 받지 않아 축이 컴파일에 반영되지 않음 | 구현(10.6절 2항, 셰이더 시스템 개정 48) | 2 |
| RR6 | RB5 | 엔진 기본 정점 입력 타입(10.2절 (2)) | 없음 | 구현(10.6절 3항) | 2 |
| RR7 | RB6·RB2 | 내장 자원 팩 1차(11.1절 — 2D 넷·차원별 대체·0·1·identity 버퍼·기본 메시·fallback 셰이더), `Pending`·`Failed` handle의 대체 자원과 계수(6.2·6.4절) | 없음(`OnLaneStart` hook만) | 구현(11.4절 5–7항) | 3 |
| RR8 | RB4 | 셰이더 교체(9.8절) | 없음 | 구현(9.8절 "구현 설계") — SH4 감시 재컴파일이 이것을 부른다 | 4 |
| RR9 | RB3 | 엔진 → 프로젝트 라이브러리 조회 순서·중복 키 오류(9.2절) | 집합 하나가 라이브러리 하나 | 후속: 프로젝트 라이브러리는 머티리얼 시스템이 연다(MS 단계). 마일스톤에는 프로젝트 셰이더가 없다 | — |
| RR10 | RB3 | 로드 때 일괄 생성 선택(9.2절) | 없음 | 버림: SH2e2부터 장치 셰이더는 검사·복사·decode만 하는 CPU 사본이다(셰이더 시스템 16.1절 SH2e2 메모). 비용은 파이프라인 생성이며 그 미리 준비는 9.7절 후속이다 | — |
| RR11 | RB3 | 엔진 셰이더 집합을 렌더 lane 시작 단계에서 열기(5.5·9.2절) | Sandbox 장치별 집합만 | 부분: RenderBase가 엔진 셰이더 타입을 등록하고(11.4절 2항) 팩이 엔진 셰이더 집합을 받는다(11.4절 5항). 엔진 호스트의 등록부·공급자 연결은 엔진 호스트가 렌더 lane을 쓰는 단계(EngineRuntime)에서 한다 | 3 |
| RR12 | RB4 | `ParameterBatch` 자원 부분(단계별 여러 텍스처·샘플러·버퍼 view, null·종류 불일치 검증, 9.3절) | 장치는 텍스처 slot 하나·이름 표 개발 경로 | 후속(렌더러 RS0 전): 장치 쪽 다중 텍스처 slot이 함께 필요하다. 마일스톤 텍스처 표시는 pass마다 텍스처 하나다 | — |
| RR13 | RB4 | 공유 상수 slot 표(9.5절) | 없음 | 후속(렌더러 RS0): 바인딩 layout의 구조체 identity(SX8)와 렌더러 slot 등록이 먼저다(9.11절 9항과 같다) | — |
| RR14 | RB4 | 바인딩 지도를 MetaSchema 스키마로 만들기(결정 R11) | 손으로 쓴 레이아웃 표 | 후속(SH3·P7) | — |
| RR15 | RB5 | 정적 정점 배치 등록·PSO 미리 준비(10.2·9.7절) | 없음 | 후속(9.7절 미리 준비와 함께). 엔진 기본 정점 입력 타입의 배치는 RenderBase 상수다(10.6절 3항) | — |
| RR16 | RB5 | 메시 정점 입력 `.rsh` 함수 인터페이스(10.2·10.5절) | 없음 | 후속(렌더러 RS0) | — |
| RR17 | RB6 | viewport 부분 clear·UAV clear 셰이더, (값, 형식, 차원) 기본 텍스처 캐시(11.1·11.2절) | 없음 | 후속(스펙 그대로 — 엔진 셰이더 SL1 `ClearResources`) | — |
| RR18 | RB2 | 장치 재생성(6.2절, BR04의 device loss) | 없음 | 후속(장치 손실 복구 단계) | — |
| RR19 | GX13 | subpass·framebuffer fetch 검토 | 후속 | 후속 확정(4.4절 GX13 결정) | — |

이미 끝난 것(대조 결과): RB3 9.10절 완료 기준(SH2e3), RB4의 바인딩 지도(상수)·상수 블록·파이프라인 캐시(그래픽·compute)·GLES 업로드 판단·상수 버퍼 묶기(GX6 — `PerFrame` 블록 회수는 마지막 사용 fence, 9.11절), RB5의 정점 배치 ⊇ 입력 마스크·형식 계열 검사(`MatchShaderVertexInputs`, SH2e2)와 다중·인스턴스 stream 픽셀 대조(GX3a `VertexSelfTest`), RB6의 attachment load clear.
