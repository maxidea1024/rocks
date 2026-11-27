# 앱 정보와 명령줄 — 11. 빌드 정보 주입

[목차](README.ko.md)

## 11. 빌드 정보 주입

### 11.1 레코드

```cpp
namespace rocks::build {

enum class BuildConfiguration : uint8 { Debug, Development, Release };
enum class TargetType : uint8 { Game, Editor, Client, Server, Program };   // 빌드 타깃 BD1

// 빌드가 실행 파일마다 주입한 상수 레코드다. 실패하지 않고 프로세스 수명 동안 같은 값이다(스레드 안전).
struct BuildDescription {
    BuildConfiguration configuration;
    TargetType targetType;
    StringView targetName;      // 빌드 대상 이름(예: "Sandbox")
    StringView projectName;     // Game 대상의 프로젝트 이름, Program은 빈 값
    StringView branch;          // 모르면 빈 값
    StringView commit;          // 40자 16진, 모르면 빈 값
    bool commitDirty;           // 커밋되지 않은 변경 위에서 빌드했다
    StringView commitDate;      // 커밋 시각 ISO 8601 UTC(예: "2026-11-18T09:12:00Z"), 모르면 빈 값
    StringView buildUrl;        // CI가 준 job 주소(빌드 때 환경 변수 ROCKS_BUILD_URL), 없으면 빈 값
    bool withDebugInfo;
};

const BuildDescription& Description() noexcept;

} // namespace rocks::build
```

### 11.2 주입 방식

- Rocks.Build가 실행 파일마다 생성 TU 두 개를 만든다. 하나는 기존 `<exe>.BuildInfo.cpp`이고, 다른 하나가 새 `<exe>.BuildStamp.cpp`(`Description()` 정의)다.
  - 매크로가 아니라 생성 TU에 두는 이유가 있다. 매크로로 주입하면 값이 바뀔 때 그 매크로를 보는 모든 TU를 다시 컴파일한다. 생성 TU면 그 TU 하나만 다시 컴파일한다.
- **값 얻기.** 브랜치·커밋·커밋 시각은 빌드 시작 때 `git rev-parse HEAD`·`git symbolic-ref --short HEAD`·`git show -s --format=%cI HEAD`·`git status --porcelain`(dirty)로 한 번 얻는다. git이 없거나 저장소가 아니면(설치 SDK) 빈 값이다. 빌드 실패가 아니다.
  - CI는 보통 detached HEAD로 받으므로 `symbolic-ref`가 실패해 branch가 빈 값이 된다. 환경 변수 `ROCKS_BUILD_BRANCH`가 있으면 그 값을 쓴다(UE는 CI가 `Build.version`의 BranchName을 채운다).
  - **비용.** 커밋이 바뀌거나 dirty 여부가 바뀌면 BuildStamp TU 하나만 다시 컴파일되지만 모든 실행 파일이 다시 링크된다(Release는 ThinLTO 링크). `git status --porcelain`은 빌드마다 한 번 돈다. 지금 BuildInfo TU도 엔진 버전이 바뀌면 같은 방식으로 다시 링크된다(`Programs/Rocks.Build.Tests/BuildInfoTests.cs:649–651`). 다시 링크하는 비용은 AP2 완료 때 측정해 보고한다.
- **날짜는 커밋 시각이다(D11).** UE는 `__DATE__`(컴파일 시각)를 쓴다. 이것은 같은 소스에서 빌드할 때마다 byte가 달라진다. Rocks는 재현 빌드를 위해 커밋 시각을 쓴다. dirty 빌드는 `commitDirty`로 구분한다.
- **캐시 무효화와 분리(D12).** `ToolBuildId()`는 입력 내용으로 정해지는 진단·캐시 무효화 키다(`BuildInfo.h:20–24`). BuildStamp TU는 그 입력에서 뺀다. 커밋만 바뀌고 코드가 같으면 `ToolBuildId`가 같아야 한다. 쿠킹 byte·에셋 너겟 키에도 넣지 않는다(엔진 버전과 같은 규칙).
- **구성·대상 종류.** 이미 있는 매크로(`ROCKS_BUILD_*`)와 빌드 규칙의 `TargetType`에서 생성 TU에 상수로 쓴다. C++ 코드의 `if constexpr` 분기는 계속 `IsReleaseBuild` 상수를 쓴다. `Description()`은 진단·출력용이다.
  - 대상 종류는 컴파일 정의 `ROCKS_TARGET_KIND_GAME`·`ROCKS_TARGET_KIND_PROGRAM`으로도 넣는다. 생성 명령줄 스키마가 "Game 대상의 Release"에서 도움말 문자열을 빼는 데 쓴다(7.2절). Base `Compiler.h`가 정확히 하나만 1인지 확인한다(구성 매크로와 같은 방식).
- **사용처.** `--version` 출력(14.6절), 시작 로그(D14), 크래시 보고·trace 파일 머리(나중).

`--version` 출력 예(C++·C# 같은 모양):

```
RocksCommandlet 0.1.0 (Development, Program, commit 4a28102d, 2026-11-18)
```
