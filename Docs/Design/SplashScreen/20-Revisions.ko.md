# 시작 화면(스플래시) — 개정 이력

[목차](README.ko.md)

## 개정 이력

- 개정 1(2026-11-26): 초안. UE 5.8 스플래시 조사(플랫폼별 구현·표시 지점·이미지 찾기·에디터 진행·한계), Rocks 구조(`SplashScreen` 모듈·표시 판정·`@2x`/DPI·글자 칸·진행 중계 당기기·단계 가중치와 측정 보정·스레드·주 창 넘기기·대화상자 hook·설정·명령줄), API, 한도, 시험, 단계 SP0–SP6, 결정 SPD1–SPD12. 기본 이미지(Studio·게임)와 Foundation 템플릿 이미지, 생성기를 함께 넣었다. 같은 날 교차 검증 1차(리뷰어 1명, 필수 6·권장 8·사소 11)를 반영했다: 시작 경로를 시작 작업 Primary 구간과 단계 계수로만 보고(하위 작업 합성이 단계 가중치를 무력화), `Result<UniquePtr<StartupProgress>>`, Windows Show 비동기와 가짜 presenter seam, 스레드 단위 DPI 문맥(프로세스 DPI는 PlatformWindow 별도 작업), 설정을 branch `Engine` 섹션 `Splash`로·struct는 `Engine` 모듈(SPD13)·사용자 층 지정자·출시 세션 층 제약, `opensMainWindow`와 호스트별 입력 표(`NoMainWindow`), 단계를 호스트가 경계를 정할 수 있는 단위로(`HostServices`·`Engine`), macOS `RunOnMainThread`·`NSWindow`·common modes 타이머·잠금 규칙·원자 표시 hook, 원천 소유와 `release`, 주 창 넘기기의 추가 present·전경 실패, 불확정 막대 조건, 배율 1.0 초과면 `@2x`, `TaskbarButtonCreated`, 보정 시계·경로, UE 행 번호·`.bmp`·로그 수준 이름 정정.
- 개정 2(2026-11-26): SP1 구현에 맞췄다. 설정을 설정 struct 대신 Engine 모듈의 `ui.splash.*` 부팅 CVar로(10절·SPD13 — 호스트마다 리플렉션 타입 등록이 필요 없다), 판정 입력의 bool 칸과 `MakeSplashVisibilityInput`(11절), 대화상자 hook의 bool 반환(9.5절), `Window::ShowAndActivate`와 Sandbox의 프레임 수 넘기기(9.4절), SDK의 스플래시 싣기를 SP6로(5.5절 — SDK가 아직 엔진 데이터 폴더를 싣지 않는다), 칸 배치 덮어쓰기·Custom 칸·창 연기 시험을 SP1b로(6.4·14·15절). SP1 구현 완료: 시험(Splash·SplashSettings 18개, Engine·PlatformWindow·Config·HostSettings)·format·policy 통과, Sandbox `--engine --window` 사람 확인(Windows 100%, 2026-11-26).
- 개정 3(2026-11-26): 기본·템플릿 스플래시를 768×384(2:1, `@2x` 1536×768)로 낮췄다(사용자 의견 "높이가 다소 길다" — 12절). 생성기·모의 화면을 다시 만들었고 엔진 코드는 바꾸지 않았다.
- 개정 4(2026-11-26): 최소 표시 시간(사용자 요구 "작업이 빨리 끝나도 2초는 보이게, 설정할 수 있게"): `ui.splash.min_seconds`(기본 2초), `CanHide()`, 넘기기 조건(9.7절·SPD15), 버린 대안에서 뺌(17절), 시험 8a.
- 개정 5(2026-11-27): SP1b 설계 — 배치 덮어쓰기·Custom 칸·막대 덮어쓰기를 코드 API로(6.5절·SPD16, 설정 키 없음), 줄바꿈은 후속, 창 연기 시험은 `Sandbox --splash-self-test`(시험 10, 누수 검사는 후속).
- 개정 6(2026-11-27): 기본·템플릿 스플래시 그림을 사용자가 준 흑요석 배경(생성형 AI, `Obsidian.png`)으로 바꿨다. Studio와 게임 기본은 같은 그림(왼쪽 스택 로고, 사용자 의견으로 왼쪽으로 50 px 옮김), 템플릿은 띠 오른쪽 위 심볼. 생성기를 원본 합성 방식으로 줄이고 `template_art.py`를 지웠다(12절).
