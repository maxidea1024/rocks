## 9. 범위 밖과 채택하지 않은 UE 요소

### 9.1 채택하지 않은 UE 요소

| UE 5.8 요소 | 판단 |
|---|---|
| `BuildSettingsVersion`·`IncludeOrderVersion`(템플릿 `DefaultBuildSettings = V7`, `IncludeOrderVersion = Unreal5_8`) | 하위 호환용 버전 스위치다. Rocks는 하위 호환을 두지 않는다 |
| 모듈 type `RuntimeNoCommandlet`·`EditorNoCommandlet`·`ClientOnlyNoCommandlet` | commandlet이 에디터 실행 파일 안에서 도는 UE 구조용이다. Rocks commandlet은 별도 Program이다 |
| 모듈 type `CookedOnly`·`UncookedOnly`, 타깃의 `bBuildRequiresCookedData` | 쿠킹 데이터 요구를 타깃 축으로 두지 않는다(3.4절 로드). 필요해지면 다시 본다 |
| 모듈 type `Program`·UE `Developer`(Editor·Program 전용)·`RuntimeAndProgram`·`EditorAndProgram` | Program 전용 모듈은 Program 타깃만 참조하면 된다. 나머지는 3.3절 대응표로 합쳤다 |
| 모듈 기술자의 `TargetAllowList`·`ProgramAllowList`·`GameTargetAllowList`·`PlatformArchitectureAllowList` 등 | 모듈 type과 `platforms`로 나타낸다. 필요가 생기면 BD12와 같은 허용 목록으로 더한다 |
| 플러그인 기술자의 `SupportedPrograms`(Program 타깃은 목록에 없는 플러그인을 뺀다 — `UEBuildTarget.cs:5904-5909`) | 켠 플러그인의 모듈은 Program 타깃에도 지금처럼 빌드된다(타깃이 참조한 모듈만 링크된다). Program이 플러그인 모듈을 쓰는지는 타깃의 모듈 참조가 정한다 |
| 타깃 규칙의 `EnablePlugins`·`DisablePlugins`·`OptionalPlugins`·`BuildPlugins`(`UEBuildTarget.cs:5609-5636, 5721-5729`), `bAllowEnginePluginsEnabledByDefault`, 프로젝트 `DisableEnginePluginsByDefault`(`:5645-5649`) | 플러그인 활성의 단일 출처는 프로젝트 기술자다(조건은 BD12). 엔진 플러그인 기본 활성은 플러그인마다 `enabledByDefault`와 프로젝트의 명시적 끄기로 정한다 |
| 플러그인 참조 `Version`(`RequestedVersion`)·`HasExplicitPlatforms`·`Description`·`MarketplaceURL`(`PluginReferenceDescriptor.cs:40-45, 90, 95`) | 플러그인 버전 선택은 저장소에 한 벌만 있는 Rocks 구조에서 필요 없다. 명시 플랫폼은 플러그인 `platforms`(BD13)가 맡는다. 설명·스토어 URL은 설치되지 않은 플러그인 안내용이다 |
| 플러그인 참조 `bActivate`·`TargetsToExplicitlyLoad`, `bRequiresBuildPlatform`·`DisallowedPlugins`·`bExplicitlyLoaded` | Game Feature 플러그인·로딩 단계 재정의·플랫폼 SDK 요구용이다. Rocks에 대응 기능이 아직 없다 |
| 의존 축 `InternalIncludePaths`(같은 scope에만 보이는 제3의 공개 범위 — `ModuleRules.cs:1291`, `UEBuildModule.cs:736-740`) | 1차는 두지 않는다. 설치 SDK는 엔진 모듈의 public include를 그대로 내보낸다. 엔진 안에서만 쓰는 공개 API를 게임에 숨길 필요가 생기면 다시 본다 |
| `DynamicallyLoadedModuleNames`·`PublicDelayLoadDLLs`·`*RuntimeLibraryPaths` | 실행 중 적재 모듈 선언과 적재 경로는 모듈 로더 스펙(BD30)에서 정한다. 엔진 런타임 서드파티는 소스로 빌드하는 정적 라이브러리다. 셰이더 도구가 실행 중에 여는 DXC·SpirvCross·SpirvTools·D3DCompiler DLL(`ShaderToolSpike.Build.cs:27`)은 도구 쪽 적재이고, 배치는 패키징·스테이징(9.2절)과 모듈 로더 스펙에서 정한다 |
| `RuntimeDependencies`(실행 파일 옆에 둘 런타임 파일) | 실행 파일 배치·스테이징은 이 문서 범위 밖이다(9.2절) |
| `PublicSystemIncludePaths` | 서드파티 include를 system include로 넘길지는 경고 정책과 함께 BS3에서 정한다 |
| `CircularlyReferencedDependentModules` | 순환을 금지한다 |
| `WITH_UNREAL_DEVELOPER_TOOLS`(`UEBuildTarget.cs:6349`) | 두지 않는다. 런타임 모듈은 계층 규칙상 Developer 모듈에 의존하지 않으므로, Developer 모듈이 있는지를 코드가 알 필요가 없다 |
| `.uproject` `Enterprise`·`MarketplaceURL`·`EngineAssociation` | Enterprise는 템플릿 분류용 표지다. MarketplaceURL은 스토어 연동이다. 엔진 연결은 `engineVersion`과 설치 SDK 경로로 한다 |

### 9.2 범위 밖

- PCH·유니티 빌드: [유니티 빌드·PCH 설계](../UnityBuildPch.ko.md).
- 템플릿에서 새 프로젝트 만들기(UE `TemplateDefs.ini`의 폴더·파일 이름 바꾸기·내용 치환, 분류, 현지화된 표시 이름): [전체 계획](../EngineImplementationPlan.ko.md) 1945행의 `rocks template` 계획이며 2026-11-14에 보류했다.
- 플랫폼별 설정 파일(`Config/<Platform>/`): [설정 시스템](../ConfigSystem.ko.md)의 플랫폼 층이다.
- 패키징·스테이징(실행 파일 옆 런타임 파일, UE `RuntimeDependencies`·`AllowedConfigFiles`·`DirectoriesToAlwaysStageAsNonUFS`, Android 패키징 설정): [에셋 시스템](../AssetSystem/18-OutOfScope.ko.md) 18절 "범위 밖과 후속"의 Android 패키징 후속과 전체 계획 2430행 부근의 패키징 계획에서 정한다.
- Server 타깃의 기능 축소 범위: 네트워크 단계 스펙(3.7절).
- 모듈 수명 객체·로더(UE `IModuleInterface`·`FModuleManager`·플러그인 로딩 단계): [모듈 로더](../ModuleLoader/README.ko.md)(BD30). 이 문서는 요구 계약만 정한다.
- 코드 다시 불러오기(UE Live Coding — Win64 비출시 구성, `TargetRules.cs:1587-1590`): `Modular` 링크가 생긴 뒤 별도 스펙으로 정한다.
