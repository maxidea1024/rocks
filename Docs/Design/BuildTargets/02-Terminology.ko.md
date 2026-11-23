## 2. 용어 대응

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| 타깃 종류 `TargetType` | `TargetType`(`UBT/Configuration/Rules/TargetRules.cs:21-47`) | Game·Editor·Client·Server·Program |
| 에디터 코드 매크로 `ROCKS_WITH_EDITOR` | `WITH_EDITOR` | 에디터 기능 코드(편집 hook·편집기 전용 함수)를 컴파일하는지 |
| 에디터 데이터 매크로 `ROCKS_WITH_EDITOR_DATA` | `WITH_EDITORONLY_DATA` | 에디터 전용 필드를 메모리 배치에 두는지 |
| 서버·클라이언트 코드 매크로 `ROCKS_WITH_SERVER_CODE`·`ROCKS_WITH_CLIENT_CODE` | `WITH_SERVER_CODE`·`WITH_CLIENT_CODE` | 서버 전용·클라이언트 전용 코드를 컴파일하는지 |
| 모듈 type | `ModuleHostType`(`UBT/Configuration/Descriptors/ModuleDescriptor.cs:785-810`) | 어느 타깃에 들어갈 수 있는 모듈인지 |
| 공개 의존 `PublicDependencies` | `PublicDependencyModuleNames` | 이 모듈의 공개 헤더가 쓰는 모듈. 소비자에게 include·정의가 넘어간다 |
| 비공개 의존 `PrivateDependencies` | `PrivateDependencyModuleNames` | 이 모듈의 구현만 쓰는 모듈 |
| 공개 환경 | `FindModulesInPublicCompileEnvironment`(`UBT/Configuration/UEBuildModule.cs:627-647`) | 모듈 자신 + 공개 의존을 따라 닿는 모듈의 public include·define |
| 컴파일 환경 | `FindModulesInPrivateCompileEnvironment`(같은 파일 607–621행) | 공개 환경 + private include·define + 비공개 의존의 공개 환경 |
| 플러그인 활성 조건 | `PluginReferenceDescriptor`의 `*AllowList`(`UBT/Configuration/Descriptors/PluginReferenceDescriptor.cs:50-75`) | 프로젝트 기술자의 플러그인 참조에 붙는 타깃·플랫폼·구성 목록 |
