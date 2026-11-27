# 시작 화면(스플래시) — 18. 다른 문서 개정

[목차](README.ko.md)

## 18. 다른 문서 개정

| 문서 | 개정 |
|---|---|
| [ModuleLoader](../ModuleLoader/03-Design/03.03-LoadingPhases.ko.md) 3.3절·9절 | `PostSplashScreen` 계열을 두지 않는 근거를 이 문서 SPD11·3절로 잇는다(이 개정에서 고침) |
| [ProgressReporting](../ProgressReporting.ko.md) 12절 | 받는 쪽에 스플래시(`StartupSplashSource` — 시작 작업 스냅샷을 스플래시 스레드가 당김)를 더한다. 그 문서는 500줄을 넘어 다음 수정 때 장별로 나눠야 하므로 PG 구현 세션이 그 개정에서 함께 더한다. 계약 변경은 없다 |
| [AppInfoCommandLine](../AppInfoCommandLine/README.ko.md) 8.3·10.2절 | 8.3절 부트 표 9a단계(10.3절 순서), 10.2절 `--no-splash`(개정 5에서 고침 — 그 문서를 장 파일로 나눈 뒤) |
| PlatformWindow(문서 없음 — EngineRuntime 9·10장 근처) | 프로세스 per-monitor v2 DPI 인식과 `Window`의 `WM_DPICHANGED` 처리(UE `InitHighDPI` 대응)는 스플래시와 별개 작업으로 남긴다. 스플래시는 스레드 DPI 문맥만 쓴다 |
| 패키징 설계(아직 없음) | 5.5절 낱개 파일 싣기 규칙 |
| `Engine/README.ko.md` | `Content/Splash` 폴더(이 개정에서 고침) |
| PlatformWindow `Window.h` | `ShowAndActivate`(숨긴 채 만든 창을 보이고 앞으로 — 9.4절, 개정 2·SP1에서 코드 주석으로 문서화) |
| PlatformBase `Platform/SystemDialog.h` | 시스템 대화상자 hook(9.5절, SP1) |
| `Templates/Game/Foundation/README.ko.md` | 템플릿 스플래시(이 개정에서 고침) |
