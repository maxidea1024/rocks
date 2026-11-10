# Rocks 빌드 및 검증

## 요구 사항

- .NET 10 SDK `10.0.302` (`global.json`, 최신 패치 롤포워드), Windows/macOS의 Clang C++23 도구 체인.
- `clang-format` 21.1.6. `ROCKS_CLANG_FORMAT`에 실행 파일 경로를 지정한다. `pwsh Build/InstallFormatter.ps1`은 고정 SHA-256을 확인한 후 네이티브 바이너리를 설치한다.
- Windows는 Visual Studio C++ 도구와 Windows SDK가 필요하다. macOS는 Xcode 도구와 macOS SDK가 필요하며 기본 배포 대상은 macOS 13.0이다.
- C++ 컴파일러와 C# 컴파일러의 경고, MSBuild 경고는 오류로 취급한다. 경고 비활성화나 단계 축소는 허용하지 않는다.

## 엔진 빌드

```text
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Debug /p:RocksTarget=Foundation
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Asan /p:RocksTarget=Foundation
dotnet msbuild Rocks.proj /t:Build /p:Configuration=Release /p:RocksTarget=Engine
```

`Compiler`, `Jobs`, `NativeOutput` 속성으로 컴파일러 경로, 병렬 작업 수, 출력 위치를 지정할 수 있다. `RocksTarget=Core`는 오브젝트 모듈을 제외한 기반 테스트를 선택한다. 출력과 증거는 기본적으로 `out/` 아래에 기록한다.

MSBuild는 .NET 도구와 C# 규칙을 빌드하고 단계를 조직한다. `Rocks.Build`가 네이티브 작업 그래프, 입력·출력 해시, 증분 여부, 실패 복구를 담당한다. `Build/Rules/*.Build.cs`는 모듈을, `*.Target.cs`는 실행 대상을 정의한다. 규칙은 신뢰된 프로젝트 코드이며 동적으로 읽는 외부 입력은 명시해야 한다.

## 설치 SDK와 게임 빌드

```text
dotnet msbuild Rocks.proj /t:InstalledSdk /p:SdkDestination=C:/work/rocks-sdk/debug /p:Configuration=Debug
dotnet msbuild Rocks.proj /t:Game /p:SdkDirectory=C:/work/rocks-sdk/debug /p:GameDirectory=C:/work/my-game /p:Configuration=Debug
```

SDK 대상 디렉터리는 새 경로여야 한다. SDK의 파일 목록과 내용, 플랫폼·아키텍처·컴파일러 및 SDK ABI·구성이 일치하지 않으면 게임 빌드를 중단한다. 게임 소스와 출력은 SDK 밖에 둔다. `Templates/Game/Foundation`은 C# 모듈·타깃 규칙과 생성된 CLASS/PROPERTY를 사용하는 예제다. 현재 SDK 내보내기는 외부 라이브러리와 임의 컴파일·링크 옵션이 있는 모듈을 지원하지 않으며 해당 선언을 오류로 거부한다.

## 메타 선언

`ROCKS_CLASS()` 또는 `ROCKS_CLASS(Abstract)`를 `class`/`struct` 바로 앞에 두고, 반영할 필드에 `ROCKS_PROPERTY(...)`를 붙인다. 선언에 ID나 스키마 버전 숫자를 넣지 않는다. 비공개 필드에는 `ROCKS_GENERATED_BODY()`가 필요하다. 타입 마커와 클래스 사이에는 공백·주석만 허용한다. 조건부 선언은 마커와 클래스를 같은 조건 블록에 둔다. 미지원 타입·상속·선언 위치·지정자와 모호한 선언은 오류다.

각 타깃은 `TargetRules.Metadata`와 `TargetRules.IdentityManifest`를 설정한다. 식별자는 소스 관리되는 `.rocksids.json`에 보관하며 일반 빌드는 파일을 수정하거나 누락된 ID를 자동 발급하지 않는다. 새 선언은 메타컴파일러의 명시적인 저작 명령으로 등록하고, 이름을 바꿀 때는 새 ID를 만들지 말고 `ids rename`을 사용한다. 삭제할 선언은 `ids retire`로 폐기한다. 타깃 전체에서 활성 명세 항목이 소스에 없으면 빌드가 실패한다.

```text
dotnet out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll ids add --manifest Game.rocksids.json --type sample::Character --field sample::Character::health
dotnet out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll ids rename --manifest Game.rocksids.json --kind field --old sample::Character::health --new sample::Character::hitPoints
```

명세와 코드 변경은 함께 버전 관리한다. CI PR 검사는 실제 공통 조상·양쪽 부모·병합 결과를 비교해 이름/ID 충돌, 이력 손실 및 서로 다른 이름 변경을 거부한다. 직접 push, 강제 push 또는 이 검사가 필수로 설정되지 않은 저장소는 자동으로 보호되지 않는다. 동일 선언에 두 ID가 이미 발급된 경우 자동으로 하나를 선택하지 않으며 데이터 마이그레이션 도구도 아직 없다. 소스에서 빠진 전체 메타 입력은 자동 탐색하지 않으므로 모든 반영 헤더를 타깃에 등록해야 한다.

`Category`와 `DisplayName`은 길이를 별도로 보관하는 UTF-8 바이트 문자열이다. 중간 NUL도 보존하므로 C 문자열 종료 문자로 길이를 추정하지 않는다. 생성 코드는 한글처럼 읽을 수 있는 문자를 그대로 표시하고, 제어 문자는 안전한 escape로 표현한다.

메타컴파일러의 `--clang-arg`는 현재 네이티브 Debug/Release/ASan 프로필과 붙임형 매크로 정의(`-DNAME=value`, clang-cl의 `/DNAME=value`), macOS의 xcrun 선택 sysroot 및 최소 버전만 허용한다. include 디렉터리는 `--include-dir`에 실제 컴파일 순서대로 지정한다. 미지원 옵션을 추가하려면 네이티브 컴파일과 AST의 동등성을 검토하고 회귀 검사를 먼저 추가해야 한다. 오류는 코드·위치·원인·수정 안내와 함께 출력한다.

Transient 필드는 GC 참조 추적에는 참여하지만 저장 그래프에 포함되지 않는다. ReadOnly 필드는 일반 쓰기를 거부하고 복원 트랜잭션의 쓰기는 허용한다. Abstract 타입은 등록할 수 있으나 생성할 수 없다. 현재 지원 범위는 단일 공개 비가상 상속과 기본 스칼라·Guid·String·ObjectRef로 제한된다.

## 별도 회귀 검사

```text
dotnet run --project Tools/Rocks.Build.Tests
dotnet run --project Tools/Rocks.Build.IntegrationTests -- --root .
dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll format --root .
```

기존 CMake/Python 경로는 이전 구현의 참고 자료다. 새 기능과 검증의 기준은 C#/.NET 경로다. 구현된 기능과 후속 과제는 실제 검증 범위와 구분하여 관리한다.
