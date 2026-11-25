# 엔진 런타임 — 1. 범위와 단계

[목차](README.ko.md)

## 1. 범위와 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| S0 | 엔진 모듈 빌드 규칙, 테스트 타깃(`Engine`), 실행 타깃(`Sandbox`) | 빈 Engine 모듈이 빌드되고 테스트 러너가 실행됨 |
| S1 | Core 보강: 게임 시계·스텝 계획, `Resolve<T>` 상속 허용, 네이티브 클래스 등록 | 시간 TK01–TK03 상당 테스트, 상속 Resolve 테스트 |
| S2 | `Engine` 상태 기계·메인 루프·EngineSubsystem | 헤드리스 N프레임 실행, 초기화 실패 역순 정리, 중복 종료 |
| S3 | World·Actor·Component·SceneComponent·지연 파괴·Tick 등록 | Spawn·Tick 중 Destroy·Transform 계층·복수 World 격리 |
| S4 | `PlatformWindow`(Win32 창·이벤트·키보드·마우스·XInput 게임패드)와 `Input`(프레임 입력 상태) | 창 닫기로 `RequestExit` 정상 종료, 헤드리스 입력 주입 테스트, Sandbox에서 마우스·키보드 반응 확인 |
| S5 | `Graphics` 계약과 `D3D12Graphics`·`OpenGLGraphics`, Vulkan·Metal 자리 | 삼각형 렌더, D3D12 offscreen readback 검사 |
| S6 | `AssetRuntime`·`AssetTools`(BMP 임포트)·텍스처 그리기 | BMP → 에셋 파일 → GPU 텍스처 → 화면의 사각형 |
| S7–S14 | Tick 의존성 완성(15절), Subsystem 의존성(16절), Actor 수명 완성(17절), GameInstance·LocalPlayer(18절), GameDirector·GameState·PlayerState(19절), Controller·Pawn(20절), 초기화 흐름(21절), 진단·문서(22절). 결정 ERD1–ERD24, 작업 순서는 22.3절 | 21.2절 14단계 시나리오와 21.4절 Director 없는 World 시나리오(원래 기준의 "붙임 계획 24절"은 저장소에 원문이 없어 21절이 정본이다) |

S4·S5는 S2·S3과 독립이다. 이번 범위에서 macOS는 창·Metal 구현이 없으므로 빌드·실행 검증을 하지 않는다.
