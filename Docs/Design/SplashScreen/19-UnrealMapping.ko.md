# 시작 화면(스플래시) — 19. 대응표

[목차](README.ko.md)

## 19. 대응표

| Rocks | UE 5.8 |
|---|---|
| `SplashScreen::Show`·`Hide`·`State` | `FPlatformSplash::Show`·`Hide`·`IsShown` |
| `platform::SetSystemDialogHook`·`NotifySystemDialogOpening` | (없음 — 호출처마다 `Hide`) |
| `SplashScreen::SetText(SplashTextSlot)` | `SetSplashText(SplashTextType)` |
| `SplashTextSlot::Title`·`Version`·`Progress`·`Copyright`·`Custom1–6` | `GameName`·`VersionInfo1`·`StartupProgress`·`CopyrightInfo`·`User1–6` |
| 막대 + 작업 표시줄 진행 | `SetProgress`(작업 표시줄만) |
| `SplashSettings::<종류>.image` | `SetCustomSplashImage`, 프로젝트 설정 Splash 범주 |
| `StudioSplash` | `EdSplash` |
| `--no-splash` | `-NOSPLASH` |
| `Splash.enabled` | CVar `WindowsApplication.ShowSplash` |
| `StartupProgress`·`StartupStep` | `EditorInit`의 `FScopedSlowTask(100)` |
| `StartupSplashSource` | `FFeedbackContextEditor::ProgressReported` |
| `Suspend`·`Resume` | 대화상자 앞뒤 `Hide`·`Show` |
| 낱개 파일 배포 | `+EngineExtraStageFiles=(Path="Content/Splash")` |
