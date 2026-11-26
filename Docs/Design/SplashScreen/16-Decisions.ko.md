# 시작 화면(스플래시) — 16. 결정

[목차](README.ko.md)

## 16. 결정

| 번호 | 결정 | 근거 |
|---|---|---|
| SPD1 | OS 창 스플래시를 새 ClientOnly 모듈 `SplashScreen`에 둔다 | UE ApplicationCore 자리. Progress 의존을 PlatformWindow에 넣지 않는다 |
| SPD2 | 이름 `Splash`·`StudioSplash`, 위치 `Content/Splash`, 찾는 순서 설정→프로젝트→엔진, `.png`→`.jpg`→`.bmp` | UE와 같다(`EdSplash`만 Rocks 이름) |
| SPD3 | `@2x` 변형 + 모니터 배율, 스레드 단위 DPI 문맥 | UE Windows의 DPI 무시 개선. 프로세스 DPI 모드를 스플래시가 바꾸지 않는다 |
| SPD4 | 게임도 진행 막대 기본 켬, 진행 문구는 기본 끔 | 요구 2 + UE의 문구 끔 근거 유지 |
| SPD5 | 스플래시 스레드가 진행 중계를 당기고, 시작 경로는 시작 작업의 Primary 구간과 단계 계수로만 보고한다(하위 작업 안 씀) | 호스트 스레드가 막혀도 갱신. 하위 작업 합성은 단계 가중치를 무력화한다 |
| SPD6 | 시작 단계 가중치 + 측정 보정 | UE 고정 가중치의 멈춤 개선 |
| SPD7 | 주 창을 먼저 보이고 한 프레임 더 그린 뒤 스플래시를 숨긴다(guard 창 없음) | 빈 틈과 z-order 문제를 한 번에 |
| SPD8 | 대화상자 hook 하나로 시스템 대화상자 앞에서 숨긴다 | 호출처마다 `Hide` 누락 방지 |
| SPD9 | OS 디코더(WIC·ImageIO)와 OS 글꼴(Segoe UI·시스템 글꼴) | 출시 게임에 Developer 모듈·글꼴 파일을 넣지 않는다. UE Windows·macOS와 같다 |
| SPD10 | 끄기: `--no-splash`·`Splash.enabled`(프로젝트·사용자 층)·무인·headless·주 창 없는 실행·비게임 호스트 자동 | UE `-NOSPLASH`·CVar·`IsUnattended`와 같은 뜻 |
| SPD11 | 모듈 로더에 `PostSplashScreen` 계열 단계를 더하지 않는다 | Slate 기반 화면용, OS 스플래시와 무관 |
| SPD12 | 스플래시 이미지는 낱개 파일로 배포 | 에셋 시스템 전에 필요(UE `EngineExtraStageFiles`) |
| SPD13 | `SplashSettings`는 `Engine` 모듈(모든 대상)에 정의, 동작은 ClientOnly 모듈 | 서버에서 프로젝트 설정 섹션이 `UnknownSection`이 되지 않게 |
| SPD14 | Windows Show는 스레드만 띄우고 돌아온다 | UE와 같다. 디코딩이 시작 경로에 들어가지 않는다 |
