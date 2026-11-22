# Foundation 게임 템플릿

이 템플릿은 설치형 Rocks SDK를 사용하는 최소 C++ 게임 프로젝트다. 현재 제공되는 예제는 객체 생성과 메타데이터 등록을 확인한다. Component 시스템, 씬, 에디터와 Tryout 기능의 구현 예제는 아직 포함하지 않는다.

## 현재 동작

템플릿 이름은 `FoundationTemplate`이다(`Template.json`의 `templateName`). 새 프로젝트를 만들 때 이 이름과 그 대문자(`FOUNDATIONTEMPLATE`)·소문자(`foundationtemplate`) 철자가 파일 이름과 내용에서 새 이름으로 바뀐다([Visual Studio 프로젝트 파일](../../../Docs/Design/ProjectFiles.ko.md) 11절). 그래서 템플릿 이름을 담지 않은 이름(`GameObject` 등)은 그대로 남는다.

- `FoundationTemplate.rocksproject`는 프로젝트 기술자다(UE .uproject 대응). 엔진 버전·모듈(`FoundationTemplate`)·대상 플랫폼을 적는다. 형식은 [저장소 구성](../../../Docs/Guides/RepositoryLayout.ko.md)의 기술자 절을 따른다.
- `FoundationTemplate.Build.cs`는 `FoundationTemplate` 실행 모듈과 `ObjectRuntime`·`Launch` 의존성을 선언한다.
- `FoundationTemplate.Target.cs`는 `FoundationTemplate` 타깃이 빌드할 모듈(`FoundationTemplate`)을 고른다. 게임 빌드는 `--target`이 없으면 `.rocksproject` 이름의 타깃을 빌드한다. 리플렉션 헤더는 모듈이 소유한다: 빌드가 모듈의 include 디렉터리(`Source`)에서 `ROCKS_CLASS` 표지가 있는 헤더(`GameObject.h`)를 찾아 메타컴파일러에 넘기고(규칙에 목록을 적지 않는다 — [설계](../../../Docs/Design/ReflectionFoundations.ko.md) 1.10절), 생성 헤더는 `<GameObject.generated.h>`로 include한다.
- `FoundationTemplate.rocksids.json`(모듈 이름 + `.rocksids.json`, `ModuleDirectory` = 프로젝트 루트)은 이 모듈의 타입·필드 식별자를 보관한다. 소스의 매크로에는 식별자를 적지 않는다. `new-project`가 새 ID로 다시 발급한다(`ids reissue`).
- `Source/GameObject.h`는 namespace `foundationtemplate`의 `rocks::Object` 기반 클래스와 `ROCKS_CLASS`, 읽기 전용 `ROCKS_PROPERTY`를 선언한다.
- `Source/FoundationTemplateModule.cpp`는 모듈 진입점 `ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, FoundationTemplate)`을 정의한다. 모든 C++ 모듈은 진입점이 하나 있어야 하며, 빌드가 실행 파일마다 만드는 모듈 표가 이를 참조하므로 빠지면 링크 오류다([모듈 로더](../../../Docs/Design/ModuleLoader.ko.md) 3.2절). 시작·종료 때 할 일이 생기면 `rocks::ModuleImplementation` 파생 클래스로 바꾼다. 모듈 진입점은 프로세스 진입점이 아니다.
- `Source/Main.cpp`는 공통 실행 계층 `Launch`의 진입점 매크로(`ROCKS_IMPLEMENT_APPLICATION`)로 프로세스 진입점을 만든다. Launch가 크래시 수집(덤프 도우미, `Saved/Crashes/<이름>-<시작 UTC>-<pid>/`)과 명령줄 캡처를 맡고 본문 `Run`의 반환값을 종료 코드로 쓴다. 새 실행 프로그램도 이 모양을 따른다(`main`을 직접 정의하지 않는다, [실행 진입점 설계](../../../Docs/Design/CrashHandlingAndLaunch.ko.md) 16절).
- 본문은 모듈 생성 헤더 `<FoundationTemplate/FoundationTemplate.module.generated.h>`에 선언된 `rocks::generated::FoundationTemplate::RegisterModuleTypes`로 이 모듈(과 리플렉션 타입을 가진 SDK 의존 모듈)의 타입을 등록하고, 객체를 만든 뒤 핸들로 조회하여 기본 체력 값이 100인지 검사한다. 성공 시 종료 코드는 0이다. 등록 함수의 정의는 빌드가 만드는 모듈 생성 소스(`<출력>/Generated/<모듈>/Source/<모듈>.module.generated.cpp`)에 있고 실행 파일에 함께 컴파일된다. 모듈 생성 헤더는 선언만 담으므로 `rocks::GeneratedType<foundationtemplate::GameObject>`를 쓰는 코드는 그 타입의 생성 헤더 `<GameObject.generated.h>`를 include한다.
- SDK 모듈의 리플렉션 타입(예: SDK가 담은 모듈의 `ROCKS_CLASS`)을 상속·참조하려면 그 모듈에 의존하고 헤더를 include한다. 다른 모듈의 클래스를 상속하려면 그 기반이 클래스 전체를 export해야 한다(`class ROCKS_<모듈>_API` — 없으면 `RMC2002`). 게임 모듈끼리 상속할 때도 기반 클래스에 자기 모듈 매크로(`class ROCKS_<게임 모듈>_API`)를 붙인다. 게임 빌드는 SDK의 descriptor(`Configurations/<구성>/Metadata/<모듈>/<모듈>.rocksmeta.json`)를 import하므로 SDK 모듈을 다시 생성하지 않는다.
- `Template.json`은 템플릿 정의(이름, 복사하지 않는 파일, 이름을 바꿀 확장자)이고, 이 README와 함께 새 프로젝트에 복사되지 않는다.

SDK를 만든 뒤 `new-project`로 새 프로젝트를 만들고 빌드한다. SDK는 템플릿을 `Templates/Game/Foundation`에 싣는다. `SDK_경로`·`부모_경로`·`이름`은 실제 값으로 바꾼다.

```text
dotnet msbuild Rocks.proj /t:InstalledSdk /p:SdkDestination="SDK_경로"
dotnet "SDK_경로/Tools/Rocks.Build.Cli/Rocks.Build.Cli.dll" new-project --template Foundation --name 이름 --destination "부모_경로"
dotnet msbuild Rocks.proj /t:Game /p:Configuration=Debug /p:SdkDirectory="SDK_경로" /p:GameDirectory="부모_경로/이름"
```

SDK 생성 경로는 존재하지 않는 새 디렉터리여야 한다. SDK 하나에 Debug·Development·Release 구성이 함께 들어가므로(`/p:SdkConfigurations=Debug`·`/p:SdkConfigurations=Debug+Release`처럼 줄일 수 있다) 게임은 그중 어느 구성으로도 빌드할 수 있다. `new-project`는 SDK의 엔진 버전으로 `engineVersion`을 맞추고, Windows에서는 Visual Studio 솔루션도 만든다(`--projectfiles off`로 끈다). 게임 실행 파일은 게임 디렉터리의 `out/<platform>-<타깃 소문자>-<구성>/bin`(예: 이름이 `MyGame`이면 Windows Debug는 `out/windows-mygame-debug/bin`) 아래 생성된다. SDK를 `sdk-register`로 등록했으면 `/p:SdkDirectory`를 생략해도 `engineVersion`으로 고른다(엔진 `Build/README.md`). 상세 도구 요구 사항과 구성 호환성은 엔진의 `Build/README.md`를 따른다.

## Component 템플릿 추가 조건

Component 시스템의 실제 API와 수명 계약이 구현되고 검증된 이후 이 템플릿에 동작하는 예제를 추가한다. 이름이나 호출 형태를 미리 확정하지 않으며, 다음 사용 경로와 검증을 포함한다.

1. **생성·부착**: 게임 오브젝트에 기본 Component를 생성하고 부착한다. 생성 실패, 중복 부착, 허용되지 않은 소유자와 분리·재부착 시 오류를 명확히 확인한다.
2. **소유·수명·GC**: 오브젝트와 Component의 소유권 및 강한 참조를 명시한다. 오브젝트 파괴, Component 분리, 참조 순환과 수집 이후에 잘못된 핸들이 거부되는지 확인한다. 임시 참조가 실제 보존 규약을 위반하지 않는지 검증한다.
3. **직렬화·복원**: Component 구성과 필드를 저장하고 복원하여 타입, 기본값, 참조와 소유 관계가 유지되는지 확인한다. 지원하지 않는 필드나 손상된 입력은 명시적으로 실패한다.
4. **에디터·Tryout**: 실제 에디터 기능이 준비되면 Component를 추가·수정·제거하고 저장한다. Tryout 실행 시 편집 월드와 실행 월드의 객체·Component 상태가 독립적인지, 종료 후 편집 상태가 보존되는지 검증한다.
5. **자동 실행 검증**: 설치 SDK를 사용하는 외부 게임에서 예제를 빌드하고 실행한다. 정상 종료뿐 아니라 잘못된 부착, 해제 후 접근, 저장·복원 실패와 리소스 정리까지 유닛 테스트와 통합 하네스로 확인한다. Windows와 macOS의 실제 실행 결과를 각각 기록한다.

Component 구현 전에는 위 항목을 예제의 현재 기능이나 성공한 검증 결과로 간주하지 않는다.
