## 8. 다른 문서 개정

이번 개정에서 함께 고친 문서:
- [저장소 구성](../../Guides/RepositoryLayout.ko.md): "후속"에 이 문서 링크를 더했다. 기술자 표·모듈 type 표·게임 빌드 연동·플랫폼 폴더 목록(Linux)은 BS2·BS4·BS6 구현 때 고친다.
- [리플렉션 기반](../ReflectionFoundations.ko.md) 134행 `EditorOnly`: BD6의 선언 규칙으로 바뀐다는 주석을 달았다.
- [유니티 빌드·PCH](../UnityBuildPch.ko.md) 8.4절 248행: 제공 모듈 후보를 "컴파일 환경에 보이는 제공 모듈"로 고쳤다.
- [셰이더 시스템](../ShaderSystem/09-ToolLoadingReleaseExclusion.ko.md) 9절: Release 배제 대상을 Game·Client·Server로 고쳤다.
- [AppInfo·CommandLine](../AppInfoCommandLine/README.ko.md) 6장·11.1절 `build::TargetType`: 다섯 값으로 고쳤다. `HostKind`(304행)는 Editor·Server 타깃이 생길 때 더한다.
- [UI 툴킷](../UiToolkit.ko.md) 7.3절 (g) `MinimalAPI`·`*_API` 행(개정 2): BD27·BD28로 정했다고 고쳤다.
- [셰이더 시스템](../ShaderSystem/16-LaterStages.ko.md) 16.1절 SSD19(개정 36): Sandbox의 워커를 선행 타깃(3.11절)으로 먼저 빌드한다고 고쳤다.

구현 때 고칠 문서:
- [전체 계획](../EngineImplementationPlan.ko.md) 282행 "공개/비공개 의존성": 이 문서 3.5절이 구현 스펙이다(링크만 더한다).
- `Programs/Rocks.Build/README.ko.md`: 단계마다 지원 범위를 갱신한다.
