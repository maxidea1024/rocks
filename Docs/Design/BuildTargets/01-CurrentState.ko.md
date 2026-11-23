## 1. 현재 상태

| 항목 | 현재 Rocks.Build | 문제 |
|---|---|---|
| 타깃 종류 | `TargetType { Game, Program }`(`BuildRules.cs:43`) | 에디터(Rocks Studio — [UI 툴킷](../UiToolkit.ko.md) 6절)와 전용 서버를 전제한 설계가 있는데 받을 타깃이 없다 |
| 에디터 전용 코드·데이터 | `EditorOnly`는 쿠킹 저장·로드 정책(`FieldEncodePolicy::excludedFlags`)의 데이터 플래그이고 "항상 컴파일된다"([리플렉션 기반](../ReflectionFoundations.ko.md) 134행) | 게임 빌드에서도 필드가 메모리 배치에 남는다. 에디터 전용 함수·코드를 컴파일에서 뺄 수단이 없다 |
| 모듈 type | 기술자 `type`(Runtime·Developer·Editor)은 게임·플러그인 모듈에만 있고 `Editor`는 "에디터 타깃이 아직 없음"으로 항상 뺀다([저장소 구성](../../Guides/RepositoryLayout.ko.md) 게임 빌드 연동). 엔진 모듈은 `SourceModuleRules.Group` 폴더뿐이다 | 엔진 모듈을 서버·클라이언트 전용으로 표시할 수 없다 |
| Release 배제 정책 | `ReleaseExclusion.cs:90`이 `Type == Game` 타깃만 검사한다 | Client·Server 타깃이 생기면 빠진다 |
| 의존 | (BS0에서 해소 — 3.5절 두 목록) `NativeModuleInput.Dependencies` 하나(`NativeBuildModels.cs:216`), 규칙 기반 클래스는 `RequiredModules` 하나 | `ActionGraph.cs:138-143`이 도달 집합 **전체**의 public include·public define을 넘긴다. 모든 의존이 사실상 Public이라 include가 새어 나간다. [전체 계획](../EngineImplementationPlan.ko.md) 282행은 "공개/비공개 의존성" 기록을 요구한다 |
| 서드파티 우회 | `ThirdPartyModuleRules`는 public include를 비우고, 소비자가 `ConsumerIncludeDirectories`·`ConsumerDefines`를 private으로 받는다(`Build/Rules/ThirdPartyModuleRules.cs:18-21, 76-90`, `FoundationTests.Build.cs:85-88`) | private 의존이 없어서 생긴 우회다 |
| 공개 헤더 검사 | `CheckPublicHeaders`가 타깃 도달 모듈 **전체**의 public include를 합친 환경에서 각 공개 헤더를 컴파일한다(`Rocks.Build.Cli/Program.cs:170-213`) | 공개 헤더가 의존을 선언하지 않은 모듈의 헤더를 include해도 잡지 못한다 |
| 리플렉션 메타데이터 | `MetadataRunner.TransitiveDependencies`(`MetadataRunner.cs:262`)·`MetadataLayout`의 도달 집합·include 순서·`DependsOn`(`MetadataLayout.cs:138, 156-183, 395-406`)이 도달 집합 전체를 쓴다 | 의존 범위와 맞물려야 한다(3.5절) |
| 프로젝트의 플러그인 참조 | `PluginReference(Name, Enabled)`(`Descriptors.cs:49`) | 타깃·플랫폼·구성 조건을 적을 자리가 없다 |
| 링크 | 정적 라이브러리 + 실행 파일, DLL 없음. 모듈 API export 매크로·모듈 수명 객체·로더가 없다 | Editor 타깃은 UE와 같이 모듈식으로 링크한다(3.8절, BD17) |
| 빌드 플랫폼 | `NativePlatform { Windows, MacOS, Android }`(`NativeBuildModels.cs:4`), `BuildContext.PlatformDirectory`는 셋 외에는 예외 | 전용 서버의 운영 플랫폼인 Linux x64를 빌드할 수 없다 |
| 서버 실행 | 실행 옵션 `--headless`(창·그래픽 장치를 만들지 않음, [AppInfo·CommandLine](../AppInfoCommandLine.ko.md) 379·548행)만 있다 | 서버 실행 파일을 만들 타깃이 없다 |
