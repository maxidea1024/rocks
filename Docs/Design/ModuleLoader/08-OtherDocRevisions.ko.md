## 8. 다른 문서 개정

이번 개정에서 함께 고친 문서:
- [빌드 타깃](../BuildTargets/README.ko.md): 3.8절 BD30과 9.2절의 "별도 스펙 모듈 로더"를 이 문서로 링크했다. BD30에 단계 관계(ML0–ML4는 BS5 전, ML5는 BS5와 함께)와 실행 파일 import 범위(MD21)를 적었다. 3.8절 Linux 링크 옵션에 `-soname`을, BS8에 빌드 정보 생성 TU의 Modular 정의 위치를 더했다.

구현 때 고칠 문서:
- [크래시 처리·실행](../CrashHandlingAndLaunch.ko.md): 8.1절에 모듈 단계(로더 생성·`EarliestPossible`, 도구 로그 뒤), 8.2절에 프로세스 묶음 종료(도구 로그 정리 전)를 더했다(개정 5, ML1). 개정 10(ML4b)에서 그 문서 개정 12로 8.1절 7a에 Modular 진입(시동 크래시 수집·manifest 로더·호스트 앱 기술)을 적었다.
- [CVar](../CVarSystem.ko.md) "모듈 언로드 때 핸들 해제(결정 필요)": MD16·MD22로 답했다(등록은 `RegisterSettings`, 핸들이 모듈 lease를 잡고, 모듈 `Shutdown`이 해제한다 — 개정 5).
- [저장소 구성](../../Guides/RepositoryLayout.ko.md) 게임 빌드 연동: `loadingPhase` 실행 중 반영을 이 문서로 링크했다(개정 5).
- [AppInfo·CommandLine](../AppInfoCommandLine/README.ko.md) 11절: `BuildDescription`에 `ModuleBuildId`를 더한다(ML4).
- [엔진 런타임](../EngineRuntime/04-ObjectSystem.ko.md) 4.1·6절: `Engine::Initialize`·`Shutdown`의 엔진 묶음 호출과 타입 등록기를 적었다(개정 5).
