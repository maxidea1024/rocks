# Foundation 게임 템플릿

이 템플릿은 설치형 Rocks SDK를 사용하는 최소 C++ 게임 프로젝트다. 현재 제공되는 예제는 객체 생성과 메타데이터 등록을 확인한다. Component 시스템, 씬, 에디터와 PIE 기능의 구현 예제는 아직 포함하지 않는다.

## 현재 동작

- `Game.rocksproject`는 프로젝트 기술자다(UE .uproject 대응). 엔진 버전·모듈(`SampleGame`)·대상 플랫폼을 적는다. 형식은 [저장소 구성](../../../Docs/Design/RepositoryLayout.ko.md)의 기술자 절을 따른다.
- `Game.Build.cs`는 `SampleGame` 실행 모듈과 `ObjectRuntime` 의존성을 선언한다.
- `Game.Target.cs`는 `Game` 타깃이 빌드할 모듈(`SampleGame`)을 고른다. 리플렉션 헤더는 모듈이 소유한다: `Game.Build.cs`의 `Metadata = ["GameObject.h"]`가 메타컴파일러가 처리할 헤더이고, 생성 헤더는 `<GameObject.generated.h>`로 include한다.
- `SampleGame.rocksids.json`(모듈 이름 + `.rocksids.json`, `ModuleDirectory` = 프로젝트 루트)은 이 모듈의 타입·필드 식별자를 보관한다. 소스의 매크로에는 식별자를 적지 않는다.
- `Source/GameObject.h`는 자체 `rocks::Object` 기반 클래스와 `ROCKS_CLASS`, 읽기 전용 `ROCKS_PROPERTY`를 선언한다.
- `Source/Main.cpp`는 모듈 집계 헤더 `<SampleGame/SampleGame.module.generated.h>`의 `rocks::generated::SampleGame::RegisterModuleTypes`로 이 모듈(과 리플렉션 타입을 가진 SDK 의존 모듈)의 타입을 등록하고, 객체를 만든 뒤 핸들로 조회하여 기본 체력 값이 100인지 검사한다. 성공 시 종료 코드는 0이다.
- SDK 모듈의 리플렉션 타입(예: SDK가 담은 모듈의 `ROCKS_CLASS`)을 상속·참조하려면 그 모듈에 의존하고 헤더를 include한다. 게임 빌드는 SDK의 descriptor(`Metadata/<모듈>/<모듈>.rocksmeta.json`)를 import하므로 SDK 모듈을 다시 생성하지 않는다.

템플릿 디렉터리를 SDK 외부의 새 게임 디렉터리로 복사한 뒤, 엔진 저장소에서 다음 명령을 실행한다. SDK는 게임 빌드에 사용할 플랫폼과 구성에 맞춰 미리 생성해야 한다. `SDK_경로`와 `게임_경로`는 실제 절대 경로로 바꾼다.

```text
dotnet msbuild Rocks.proj /t:InstalledSdk /p:Configuration=Debug /p:SdkDestination="SDK_경로"
dotnet msbuild Rocks.proj /t:Game /p:Configuration=Debug /p:SdkDirectory="SDK_경로" /p:GameDirectory="게임_경로"
```

SDK 생성 경로는 존재하지 않는 새 디렉터리여야 한다. 게임 실행 파일은 게임 디렉터리의 `out/Debug/bin` 아래 생성된다. Windows 파일 이름은 `SampleGame.exe`, macOS 파일 이름은 `SampleGame`이다. 상세 도구 요구 사항과 구성 호환성은 엔진의 `Build/README.md`를 따른다.

## Component 템플릿 추가 조건

Component 시스템의 실제 API와 수명 계약이 구현되고 검증된 이후 이 템플릿에 동작하는 예제를 추가한다. 이름이나 호출 형태를 미리 확정하지 않으며, 다음 사용 경로와 검증을 포함한다.

1. **생성·부착**: 게임 오브젝트에 기본 Component를 생성하고 부착한다. 생성 실패, 중복 부착, 허용되지 않은 소유자와 분리·재부착 시 오류를 명확히 확인한다.
2. **소유·수명·GC**: 오브젝트와 Component의 소유권 및 강한 참조를 명시한다. 오브젝트 파괴, Component 분리, 참조 순환과 수집 이후에 잘못된 핸들이 거부되는지 확인한다. 임시 참조가 실제 보존 규약을 위반하지 않는지 검증한다.
3. **직렬화·복원**: Component 구성과 필드를 저장하고 복원하여 타입, 기본값, 참조와 소유 관계가 유지되는지 확인한다. 지원하지 않는 필드나 손상된 입력은 명시적으로 실패한다.
4. **에디터·PIE**: 실제 에디터 기능이 준비되면 Component를 추가·수정·제거하고 저장한다. PIE 실행 시 편집 월드와 실행 월드의 객체·Component 상태가 독립적인지, 종료 후 편집 상태가 보존되는지 검증한다.
5. **자동 실행 검증**: 설치 SDK를 사용하는 외부 게임에서 예제를 빌드하고 실행한다. 정상 종료뿐 아니라 잘못된 부착, 해제 후 접근, 저장·복원 실패와 리소스 정리까지 유닛 테스트와 통합 하네스로 확인한다. Windows와 macOS의 실제 실행 결과를 각각 기록한다.

Component 구현 전에는 위 항목을 예제의 현재 기능이나 성공한 검증 결과로 간주하지 않는다.
