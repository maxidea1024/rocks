## 6. 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| ML0 | Core: 수명 객체·진입 기술·진입점 매크로(Monolithic)·모듈 표 형식(개정 2). `ModuleLoader` 모듈: `ModuleStartContext`·`HostServices`(형식 키 매크로)·빌드 생성 모듈 표·의존 순서 시작·상태·`Changed`·report 두 종·묶음 종료·시작 실패 정리. `DynamicLibrary` POSIX `dlerror` 보강. 모든 기존 C++ 모듈에 `ROCKS_IMPLEMENT_MODULE`(대부분 기본 구현). 빌드: 모듈 이름 대소문자 충돌 검사(MD17) | 의존 순서 시작·역순 종료·실패 중단과 역순 정리·`Cycle`·`WrongThread`·`ShuttingDown`·`Failed` 재시도 시험, 매크로 누락·이름 불일치 링크·컴파일 오류 시험, 대소문자 충돌 거부 시험 |
| ML1 | 로딩 단계와 묶음: 기술자 `PostEngineInit`·`None`, 엔진 모듈 규칙 `LoadingPhase`, 모듈 표 행의 요청 시 적재 표지와 `StartPhase`의 건너뜀(3.8절), 단계 순서 빌드 검사(MD6), 부트 모듈 단계 강제, `StartPhase`의 앞 단계 차례 실행, Launch 통합(crash-launch 병합 뒤), 엔진 호스트의 `PostConfigInit`·호스트 묶음 종료, `Engine::Initialize`·`Shutdown`의 엔진 묶음과 참조 수, 타입 등록기 여럿. 세부는 3.11절(개정 5) | Sandbox 엔진 호스트가 모든 단계를 지나 부트·종료한다. 한 프로세스에서 Engine을 차례로 여러 번 만드는 시험이 엔진 묶음을 매번 시작·종료한다. 단계별 시작·종료 순서 시험 |
| ML2 | 타입 등록기(MD15)·모듈별 `ModuleRegistration`·모듈 자신의 타입만 등록하는 생성 함수, 실제 `ModuleLease`(MD16)와 생성 등록 함수의 lease 인자, 설정 등록(MD22, Monolithic), 기존 모듈별 등록 래퍼를 `Startup`·`RegisterSettings`로 옮기기, CVar·trace 채널 등록의 lease. ML1 전 연결과 ML1 뒤로 넘긴 것은 3.10절(개정 3) | 호스트 코드의 모듈별 명시 등록 호출이 HostSettings 설정 타입 외에 0. 늦은 단계 모듈의 CVar가 `ReportUnresolved` 경고 없이 설정 값을 받는 시험. 종료 뒤 남은 lease 진단 시험 |
| ML3 | `DynamicModules`(MD11)와 Monolithic 링크 규칙, type 검사, 링크 의존만의 순환 거부(동적 관계는 참조 사슬 아님), 표에 행이 없는 모듈 거부 | 실행 중 적재 모듈을 `Load`·`Find<T>(name)`으로 쓰는 시험 |
| ML4 | Modular 적재 — 선행: [빌드 타깃](../BuildTargets/README.ko.md) BS8. manifest 쓰기·읽기(원자적 쓰기, 모르는 키 거부), `ModuleBuildId` 재사용·SDK manifest, 파일 크기 대조, export 진입점과 강제 참조, `Prepare`, 실행 파일의 부트 모듈 import와 호스트 모듈(MD21), Linux `-soname`, OS 적재·report, `Modular` FoundationTests를 로더로 실행 | `Modular` FoundationTests가 Windows·macOS에서 로더로 모듈을 적재해 통과한다. `buildId` 불일치·크기 불일치·의존 DLL 누락·진입점 없음·모르는 키의 report 시험. 증분 빌드에서 `buildId`가 바뀌지 않는 시험 |
| ML5a | 엔진 플러그인 Modular(3.15절): 플러그인 binaries 폴더·플러그인 manifest(MD23), 실행 파일 manifest의 `plugins`(schema 4), manifest 사이 빌드 식별 대조, 합친 행의 의존 정렬, 빌드의 rpath | `EngineModular`가 UnrealExchange 플러그인을 Modular로 적재해 그 시험이 Windows·macOS에서 통과한다. 플러그인 manifest 불일치·없음·중복·순환의 report 시험 |
| ML5b | Rocks Studio 적재 — BS4·BS5와 함께: 프로젝트 플러그인, 플러그인 조건 해석, 빌드 목록 대조(MD19) | 게임·플러그인 모듈이 있는 프로젝트를 Rocks Studio가 열고 닫는다 |
| ML6 | 실행 중 해제(MD10)와 플러그인 끄기, 공유 라이브러리 지연 해제 | `Busy`(의존 모듈·살아 있는 인스턴스)·`Unsupported` 시험(아무것도 바뀌지 않음), 해제 뒤 다시 적재 시험(Modular), lease가 남은 경우의 지연 해제 시험 |
