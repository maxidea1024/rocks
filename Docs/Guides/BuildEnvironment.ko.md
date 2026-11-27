# 빌드 환경 설정

아무것도 설치하지 않은 컴퓨터에서 Rocks를 처음 빌드하기까지의 순서다. 빌드 명령과 옵션 전체는 [빌드 안내](../../Build/README.md)에 있다.

## 1. 빠른 시작

**Windows**

1. Git을 설치한다.
   ```powershell
   winget install --id Git.Git -e --source winget
   ```
2. **새 터미널을 열고** 저장소를 받는다(방금 설치한 git은 새 터미널부터 보인다. GitHub 로그인 창이 뜨면 로그인한다).
   ```powershell
   git clone https://github.com/maxidea1024/rocks.git C:\work\rocks
   ```
3. 탐색기에서 `C:\work\rocks\Setup.bat`을 두 번 누르고, `Install the missing items? [Y/n]`에 Enter를 누른다.
4. 새 터미널을 열고 빌드한다.
   ```powershell
   cd C:\work\rocks
   dotnet msbuild Rocks.proj -t:Test -p:Configuration=Debug -p:RocksTarget=Foundation
   ```

**macOS**(CI 기준, 로컬 미검증)

1. 저장소를 받는다(Git이 없으면 Command Line Tools 설치 창이 뜬다. 설치한 뒤 다시 실행한다).
   ```sh
   git clone https://github.com/maxidea1024/rocks.git ~/work/rocks
   ```
2. 마법사를 실행하고 `Install the missing items? [Y/n]`에 Enter를 누른다.
   ```sh
   cd ~/work/rocks && ./Setup.sh
   ```
3. 새 터미널을 열고 빌드한다.
   ```sh
   cd ~/work/rocks
   dotnet msbuild Rocks.proj -t:Test -p:Configuration=Debug -p:RocksTarget=Foundation
   ```

**Linux**는 아직 C++ 빌드 호스트가 아니다. 5절을 본다.

## 2. 설치 마법사

마법사는 항목마다 한 줄로 상태를 보이고, 빠졌거나 틀린 것만 설치한 뒤 다시 점검한다. 여러 번 실행해도 결과가 같다.

```text
Rocks setup for Windows (C:\work\rocks)
[1/7] Git              OK         2.55.0.windows.3, core.longpaths=true
[2/7] .NET SDK         OK         10.0.302
[3/7] PowerShell 7     OK         7.7.0
[4/7] C++ build tools  OK         Visual Studio Community 2026 18.12.12412.261, MSVC 14.52.37025, Windows SDK 10.0.26100.0
[5/7] FXC SDK          OK         Windows SDK 10.0.22621.0 (d3dcompiler_47.dll)
[6/7] LLVM             OK         clang version 22.1.8 at C:\Users\<사용자>\AppData\Local\Rocks\toolchains\llvm-22.1.8\bin
[7/7] clang-format     OK         21.1.6 at C:\Users\<사용자>\AppData\Local\Rocks\toolchains\clang-format-21.1.6\clang-format.exe

Setup is complete. Open a new terminal (or restart your IDE) and run:
```

| 항목 | Windows | macOS |
|---|---|---|
| Git | winget, `core.longpaths=true` | Command Line Tools에 들어 있다 |
| .NET SDK | `global.json` 버전(10.0.302, 10.0.3xx)을 winget으로 | 같은 버전의 공식 압축 파일을 SHA-512로 확인하고 `~/.dotnet`에 |
| PowerShell 7 | winget | 늘(Rocks.proj가 `Build/Tools.ps1`로 빌드 도구를 준비한다), `Build/Toolchain.json`에 고정한 공식 tar.gz(SHA-256 확인) |
| C++ 도구 | Visual Studio 17.8 이상이 없으면 **Build Tools for Visual Studio 2026**(IDE 없음) | **Command Line Tools** |
| FXC SDK | 셰이더 시험용 Windows SDK 10.0.22621 | (해당 없음) |
| LLVM | 검증 LLVM 22.1.8을 `%LOCALAPPDATA%\Rocks\toolchains`에 받고 사용자 PATH 맨 앞에 | (Apple clang을 쓴다) |
| clang-format | 21.1.6을 같은 곳에 받고 `ROCKS_CLANG_FORMAT` 설정 | 같은 파일을 SHA-256으로 확인하고 `~/Library/Application Support/Rocks/toolchains`에, 시작 파일에 설정 |
| ISPC | 1.32.0을 같은 곳에 받고 `ROCKS_ISPC` 설정(텍스처 압축기의 ISPC 원본 — `Build/InstallIspc.ps1`) | 같은 스크립트로 같은 곳에, 시작 파일에 설정 |

- 기본은 IDE 없는 Build Tools(Windows)·Command Line Tools(macOS)다. IDE는 옵션이다(3절). macOS는 Homebrew 없이 끝난다(PowerShell 7도 고정 tar.gz를 `~/Library/Application Support/Rocks/toolchains/powershell-<판>`에 푼다).
- 받는 파일의 URL과 해시는 `Build/Toolchain.json`이 고정한다.
- Visual Studio Installer와 winget 설치 프로그램이 UAC 창을 띄울 수 있다. 마법사 자신은 관리자 권한이 필요 없다.
- 이미 떠 있는 터미널·VS Code·Visual Studio는 바뀐 PATH와 환경 변수를 모른다. 마법사가 끝나면 모두 다시 연다.
- 실패하면 그 줄에 원인과 `Next:`(다음 할 일)가 나온다. 고친 뒤 마법사를 다시 실행한다.

| 옵션(Windows / macOS) | 하는 일 |
|---|---|
| `-Check` / `--check` | 점검만 한다. 모두 통과하면 종료 코드 0 |
| `-Yes` / `--yes` | 묻지 않고 설치한다 |
| `-WithIde` | Visual Studio Community와 Clang IntelliSense 구성 요소(`.vsconfig`)를 먼저 설치하고 빌드에도 그것을 쓴다 |
| `-Android` / `--android` | Android NDK도 점검·설치한다(6절) |
| `-Skip 'FXC SDK'` | 셰이더 시험을 돌리지 않을 때 그 항목을 뺀다 |

터미널에서는 `.\Setup.bat -Check`, `./Setup.sh --check`처럼 쓴다. 설계는 [설치 마법사](../Design/SetupWizard.ko.md)에 있다.

## 3. Visual Studio가 꼭 있어야 하나요?

**Windows**
- 빌드만 하려면 Visual Studio IDE는 필요 없다. **Build Tools for Visual Studio 2026**(무료, IDE 없음)이면 된다. 이미 있는 Visual Studio나
  Build Tools는 17.8 이상(MSVC 14.38 이상)이어야 쓴다. VS 2019는 쓰지 않는다.
- 그래도 Build Tools는 꼭 필요하다. clang-cl이 MSVC의 표준 라이브러리·C 런타임과 Windows SDK를 쓰고, Rocks.Build가 vswhere로 그것을 찾는다.
- Visual Studio에서 편집하려면 Community(또는 상위 판)에 Clang 구성 요소 두 개("Windows용 C++ Clang 컴파일러",
  "LLVM(clang-cl) 도구 집합에 대한 MSBuild 지원")를 더 설치한다. `Setup.bat -WithIde`가 해 준다.
- 빌드는 Visual Studio 안의 clang-cl이 아니라 마법사가 설치한 검증 LLVM(PATH 맨 앞)을 쓴다.

**macOS**
- Command Line Tools만으로 빌드된다.
- CI가 검증한 것은 Xcode 16.4의 Apple clang(`Apple clang version 17.0.0 (clang-1700.0.13.5)`)이다. 다른 Xcode나 Command Line Tools만
  쓰면 빌드 때 `Toolchain: ... is not a verified MacOS toolchain` 알림이 한 줄 나올 수 있다. 빌드는 된다.

Visual Studio 솔루션 생성은 아직 없다([설계](../Design/ProjectFiles.ko.md)).

## 4. 첫 빌드 결과

1. C# 도구를 빌드한다(`Build succeeded. 0 Warning(s) 0 Error(s)`).
2. 리플렉션 코드를 만들고(`Rocks metadata: module ...: N types; generated`) C++를 빌드한다(`Native build: PASS; executed=...`).
3. 시험을 돌린다. 시험 묶음마다 `N test cases, 0 failures`가 나오고 명령의 종료 코드가 0이면 성공이다. 시험 중의 `[Error]`·`[Warning]`
   로그 줄은 시험이 일부러 낸 것이다.

| 경로 | 내용 |
|---|---|
| `out/dotnet/bin/` | C# 도구(Rocks.Build.Cli, MetaCompiler, ToolTests) |
| `out/dotnet-windows-foundation-debug/`(macOS는 `dotnet-macos-...`) | C++ 출력. 실행 파일 `bin/`, 시험 결과 `foundation-report.json`, 빌드 증거 `build-evidence.json` |

처음 빌드와 시험은 20 논리 코어 PC에서 합쳐 약 17분 걸렸다(기본 동시 작업 8개, `-p:Jobs=<N>`으로 바꾼다). 다시 실행하면 바뀐 곳만
빌드한다. 실패하면 `error MSB3073`이 나오고, 원인은 `build-evidence.json`의 `error`에 있다.

## 5. Linux

지금 Linux 호스트는 C++ 빌드를 받지 않는다(`Only Windows and macOS native hosts are supported.`). Linux 빌드 플랫폼(전용 서버·기반 시험)은
계획 단계다([빌드 타깃](../Design/BuildTargets/03-Design/03.07-DedicatedServer.ko.md) 3.7절). 지금은 C# 도구 빌드와 정책·서식 검사만 된다(x86_64, 로컬 미검증).

1. Git·curl·unzip을 설치하고 저장소를 받는다(예: `sudo apt install git curl unzip`).
2. `./Setup.sh`를 실행한다. .NET SDK와 clang-format 21.1.6을 고정 파일(SHA 확인)로 사용자 디렉터리에 받고, PATH와 `ROCKS_CLANG_FORMAT`을
   시작 파일(bash는 `~/.bashrc`)에 남긴다.
3. 새 터미널에서 C# 도구를 빌드하고 검사한다.
   ```sh
   dotnet build Programs/Rocks.Tools.csproj
   dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll policy --root .
   dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll format --root .
   ```

## 6. Android 대상(선택)

Windows·macOS에서 교차 빌드만 한다. CI는 Android 빌드를 돌리지 않는다.

1. Android Studio를 설치한다(SDK가 Windows `%LOCALAPPDATA%\Android\Sdk`, macOS `~/Library/Android/sdk`에 생긴다).
2. `Setup.bat -Android`(macOS `./Setup.sh --android`)로 NDK 29.0.14206865를 받고 `ANDROID_NDK_HOME`을 설정한다.
3. 새 터미널에서 빌드한다. 시험은 에뮬레이터나 기기 하나를 연결하고 돌린다.
   ```powershell
   dotnet msbuild Rocks.proj -t:Build -p:RocksPlatform=android -p:AndroidAbi=arm64 -p:AndroidApi=26
   dotnet msbuild Rocks.proj -t:Test -p:RocksPlatform=android -p:AndroidAbi=x86_64
   ```

NDK는 `-p:AndroidNdk=<경로>`, `adb`는 `-p:Adb=<경로>`로 한 번만 바꿀 수 있다. 출력은 `out/dotnet-android-<abi>-<타깃>-<구성>/`이다.

## 7. 선택 작업

**커밋 전 검사.** 첫 빌드 뒤에 쓸 수 있다. `format`에 `--write`를 붙이면 고친다.

```powershell
dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll format --root .
dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll policy --root .
```

**다른 구성과 검증 빌드.**

| 목적 | 명령 |
|---|---|
| 유니티 묶음·PCH를 끈 검증 빌드(빠진 include 찾기) | `dotnet msbuild Rocks.proj -t:Build -p:Unity=off -p:Pch=off -p:Configuration=Debug -p:RocksTarget=Foundation` |
| Release(ThinLTO) | `dotnet msbuild Rocks.proj -t:Test -p:Configuration=Release -p:RocksTarget=Foundation` |
| AddressSanitizer | `dotnet msbuild Rocks.proj -t:Test -p:Configuration=Asan -p:RocksTarget=Foundation` |
| Modular(모듈마다 공유 라이브러리 — export 누락 확인, native 시험과 크래시·Launch 하니스, [빌드 타깃](../Design/BuildTargets/03-Design/03.08-LinkType.ko.md) 3.8절 BS8d5·BS8d6) | `dotnet msbuild Rocks.proj -t:Test -p:Configuration=Debug -p:RocksTarget=FoundationModular` |
| Engine 층 Modular(엔진 플러그인 시험 제외, 3.8절 BS8e) | `dotnet msbuild Rocks.proj -t:Test -p:Configuration=Debug -p:RocksTarget=EngineModular` |
| CI처럼 검증 도구 모음만 받기 | 명령 뒤에 `-p:RequireVerifiedToolchain=on` |

유니티 빌드·PCH는 [유니티 빌드·PCH](../Design/UnityBuildPch.ko.md) 11절, 구성별 옵션은 [빌드 안내](../../Build/README.md#빌드-구성)에 있다.

**Clang 21을 계속 쓰기(Windows, CI 구성 아님).** MSVC 14.5x STL은 Clang 22 이상만 받는다. LLVM 21.1.6을 쓰려면 MSVC 14.44 도구 모음을
더하고 고른다. 빌드마다 `not a verified Windows toolchain` 알림이 나온다.

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "<Visual Studio 또는 Build Tools 설치 경로>" --add Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64 --passive
pwsh Build/InstallLlvm.ps1 -Version 21.1.6 -ToolchainRoot "$env:LOCALAPPDATA\Rocks\toolchains"
[Environment]::SetEnvironmentVariable('ROCKS_MSVC_TOOLSET', '14.44', 'User')
```

사용자 PATH 맨 앞을 `%LOCALAPPDATA%\Rocks\toolchains\llvm-21.1.6\bin`으로 바꾸고 프로그램을 다시 연다.

**셰이더 도구 바이너리.** DXC·SPIRV-Cross·SPIRV-Tools·FXC를 여는 시험만 필요하다. 없으면 그 시험은 SKIP이다.
`dotnet msbuild Rocks.proj -t:ThirdPartyBootstrap`으로 받는다. FXC는 Windows SDK 10.0.22621.0의 버전별 사본
`bin\10.0.22621.0\x64\d3dcompiler_47.dll`을 SHA-256으로 확인하고 복사한다. 마법사의 "FXC SDK" 항목이 그 SDK를 설치한다. 바이너리를 직접 다시 빌드할 때만 CMake·Ninja·Python 3이
필요하다([빌드 안내](../../Build/README.md#미리-빌드한-바이너리bootstrap)). Vulkan SDK는 빌드에 필요 없다(검증 층을 켤 때만 설치한다).

## 8. 문제 해결

| 증상 | 원인 | 해결 |
|---|---|---|
| `The selected MSVC STL requires Clang 22 or newer (STL1000), but the compiler is Clang 21...` | PATH 첫 `clang-cl`이 22 미만인데 MSVC 14.5x STL을 골랐다 | 마법사 실행, 또는 7절 "Clang 21을 계속 쓰기" |
| `Toolchain: ... is not a verified ... toolchain` 한 줄 | `Build/Toolchain.json` 검증 목록 밖 컴파일러. 빌드는 진행한다 | Windows는 마법사로 22.1.8. macOS는 3절 참고, 무시해도 된다 |
| `... --require-verified-toolchain on accepts only verified toolchains` | `-p:RequireVerifiedToolchain=on`(또는 같은 환경 변수)에서 미검증 도구 모음 | 검증 도구 모음을 쓰거나 옵션을 뺀다 |
| `Executable not found on PATH.`(clang-cl) | PATH에 LLVM `bin`이 없다 | 마법사 실행 뒤 새 터미널 |
| `Clang 20.1.8 is older than the minimum Clang 21 for Windows` | 옛 LLVM이 PATH 앞에 있다 | `where.exe clang-cl`로 찾아 순서를 고친다(마법사가 `Next:`로 알린다) |
| `Visual Studio C++ toolchain 17.8 or newer not found ...` | vswhere가 17.8 이상의 완전한 C++ 도구 설치를 못 찾았다(VS 2019·미완료 설치는 건너뛴다) | 마법사 실행(Build Tools 설치) 또는 Visual Studio Installer에서 복구 |
| `MSVC toolset 14.29... is older than the minimum 14.38.33130` | Developer 셸이나 `ROCKS_MSVC_TOOLSET`이 옛 도구 모음을 골랐다 | 보통 터미널에서 실행하거나 `ROCKS_MSVC_TOOLSET`을 지운다 |
| `winget not found` | Windows Server·LTSC 등 winget이 없는 Windows다 | Microsoft Store의 App Installer를 설치하거나 부록 A |
| 내려받기 실패(`Could not download ...`) | 네트워크·프록시 | 프록시면 `HTTPS_PROXY`를 설정하고 마법사를 다시 실행한다 |
| `Visual Studio environment initialization failed: ...` | VsDevCmd 실패(SDK 누락, 깨진 설치) | Visual Studio Installer에서 Windows 11 SDK 추가 또는 복구 |
| `ROCKS_MSVC_TOOLSET=14.44 is not the active MSVC toolset` | 그 도구 모음이 없거나 Developer PowerShell에서 실행했다 | 7절의 구성 요소 설치, 보통 터미널에서 실행 |
| `RMC3100: Rocks requires clang-format 21.1.6`(MetaCompiler) 또는 `Rocks requires clang-format 21.1.6` | `ROCKS_CLANG_FORMAT`이 비어 PATH의 다른 clang-format(LLVM 22.1.8에 든 22.1.8 등)을 썼다 | 마법사 실행 뒤 프로그램을 다시 연다 |
| 환경 변수를 바꿨는데 빌드가 옛 clang-cl·clang-format을 쓴다 | 이미 떠 있던 터미널·IDE는 새 값을 모른다 | 터미널·VS Code·Visual Studio를 닫고 다시 연다 |
| `InstallFormatter.ps1`이 `Access to the path is denied` | 빌드·시험이 그 `clang-format.exe`를 쓰는 중이다 | 빌드가 끝난 뒤 다시 실행한다 |
| `Clang 18.0.4 is older than the minimum Clang 20 for Android` | 옛 NDK를 골랐다(`ANDROID_NDK_HOME`이 옛 NDK) | `Setup.bat -Android` |
| `adb not found` 또는 `is exactly one device connected?` | `platform-tools`가 없거나 기기가 0개·여러 개 | SDK Manager에서 Platform-Tools 설치, 기기 하나만 연결(`ANDROID_SERIAL`) |
| 시험은 모두 통과했는데 `error MSB3073`, `build-evidence.json`의 `error`가 `Source inputs changed during the build/test interval` | 빌드·시험 중에 소스를 고쳤거나 git 작업을 했다 | 편집을 멈추고 다시 실행한다 |
| `c:\`와 `C:\`처럼 철자만 다른 경로 | 빌드가 디스크 철자로 맞추므로 다시 컴파일하지 않는다 | 할 일 없음. symlink·junction·`subst`는 풀지 않는다 |
| `--output must be under ...\out` | 출력 경로가 저장소 `out` 밖이다 | `-p:NativeOutput=out/<이름>` |
| `Access to the path ... is denied`(object 파일 교체 중) | 백신·인덱서가 파일을 열고 있었다. 지금 빌드는 열린 파일도 바꾸므로 거의 나지 않는다 | 다시 실행한다. 반복되면 `out`을 백신 검사 제외에 넣는다 |
| `MSB1008: Only one project can be specified.` | Git Bash가 `/t:`·`/p:`를 경로로 바꿨다 | `-t:`·`-p:` 형태로 쓴다 |
| `Only Windows and macOS native hosts are supported.` | Linux에서 C++ 빌드를 시도했다 | 5절 |
| `Setup.bat` 창이 바로 닫힌다 | 탐색기 밖(다른 프로그램)에서 실행했다 | 터미널에서 `.\Setup.bat`로 실행해 메시지를 본다 |

## 부록 A. Windows 수동 설치

마법사가 하는 일을 손으로 할 때의 순서다. PowerShell 7 터미널에서 저장소 루트(`C:\work\rocks`)에서 진행한다.

1. 도구를 설치한다. .NET SDK는 버전을 정해야 한다(빼면 10.0.4xx가 깔려 `global.json`과 맞지 않는다).
   ```powershell
   winget install --id Microsoft.DotNet.SDK.10 -e --source winget --version 10.0.302
   winget install --id Microsoft.PowerShell -e --source winget
   winget install --id 7zip.7zip -e --source winget
   git config --global core.longpaths true
   ```
   winget이 없으면 각 제품의 공식 설치 파일을 쓴다(.NET SDK는 정확히 10.0.302).
2. Build Tools를 설치한다. IDE로 편집하려면 대신 Community를 `--config C:\work\rocks\.vsconfig`로 설치한다.
   ```powershell
   winget install --id Microsoft.VisualStudio.BuildTools -e --source winget --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
   ```
   이미 설치한 Visual Studio·Build Tools(17.8 이상)에는 `setup.exe modify`로 더한다.
   ```powershell
   & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "<설치 경로>" --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --passive
   ```
3. LLVM과 clang-format을 저장소 밖 고정 위치에 받는다. `-ToolchainRoot`를 빼면 저장소 `out\toolchains`에 풀려 `out`을 지울 때 PATH가 깨진다.
   ```powershell
   pwsh Build/InstallLlvm.ps1 -ToolchainRoot "$env:LOCALAPPDATA\Rocks\toolchains"
   pwsh Build/InstallFormatter.ps1 -ToolchainRoot "$env:LOCALAPPDATA\Rocks\toolchains"
   [Environment]::SetEnvironmentVariable('ROCKS_CLANG_FORMAT', "$env:LOCALAPPDATA\Rocks\toolchains\clang-format-21.1.6\clang-format.exe", 'User')
   pwsh Build/InstallIspc.ps1 -ToolchainRoot "$env:LOCALAPPDATA\Rocks\toolchains"
   [Environment]::SetEnvironmentVariable('ROCKS_ISPC', "$env:LOCALAPPDATA\Rocks\toolchains\ispc-1.32.0\ispc.exe", 'User')
   ```
4. 시작 메뉴 "계정의 환경 변수 편집" → 사용자 변수 `Path` → 새로 만들기에 `%LOCALAPPDATA%\Rocks\toolchains\llvm-22.1.8\bin`을 넣고
   "위로 이동"으로 맨 위에 둔다. Windows는 시스템 PATH 뒤에 사용자 PATH를 붙이므로, 시스템 `Path`에 다른 `clang-cl.exe`
   (예: `C:\Program Files\LLVM\bin`)가 있으면 그 항목을 지우거나 LLVM `bin`을 시스템 `Path` 맨 위에 둔다.
5. 프로그램을 모두 다시 열고 확인한다. PowerShell의 `where`는 다른 명령이라 `where.exe`로 쓴다.

| 명령 | 기대 출력 |
|---|---|
| `dotnet --version` | `10.0.302`(또는 더 높은 `10.0.3xx`) |
| `where.exe clang-cl` | 첫 줄이 `...\AppData\Local\Rocks\toolchains\llvm-22.1.8\bin\clang-cl.exe` |
| `clang-cl --version` | 첫 줄이 `clang version 22.1.8 (https://github.com/llvm/llvm-project ca7933e47d3a3451d81e72ac174dcb5aa28b59d1)` |
| `& $env:ROCKS_CLANG_FORMAT --version` | `clang-format version 21.1.6` |
| `& $env:ROCKS_ISPC --version` | `Intel(r) Implicit SPMD Program Compiler (Intel(r) ISPC), 1.32.0 (...)` |
| `& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath` | Visual Studio 또는 Build Tools 설치 경로 |

`.\Setup.bat -Check`로도 같은 것을 확인할 수 있다.

## 부록 B. macOS 수동 설치

CI 기준, 로컬 미검증. 저장소 루트에서 진행한다.

1. `xcode-select --install`로 Command Line Tools를 설치한다(Xcode 앱을 쓰려면 `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`).
2. .NET SDK 10.0.302를 설치한다. Microsoft 다운로드 페이지의 macOS 설치 파일(정확히 10.0.302)을 쓰거나, `Build/Toolchain.json`의
   `dotnet.sdk.archives`에 적힌 압축 파일을 받아 `shasum -a 512`가 같은 값인지 확인한 뒤 `~/.dotnet`에 풀고
   `export PATH="$HOME/.dotnet:$PATH"`를 `~/.zshrc`에 더한다. Homebrew의 `dotnet-sdk`는 다른 버전대일 수 있다.
3. clang-format 21.1.6을 설치한다. `Build/Toolchain.json`의 `clangFormat.packages`에 적힌 wheel을 받아 `shasum -a 256`을 확인하고,
   `unzip -p <wheel> clang_format/data/bin/clang-format`으로 꺼낸 파일을 `~/Library/Application Support/Rocks/toolchains/clang-format-21.1.6/`에
   두고 실행 권한을 준다. 그 경로를 `export ROCKS_CLANG_FORMAT="..."`로 `~/.zshrc`에 더한다.
4. 새 터미널에서 확인한다: `which clang++`는 `/usr/bin/clang++`, `clang++ --version` 첫 줄은 `Apple clang version 17...`,
   `"$ROCKS_CLANG_FORMAT" --version`은 `clang-format version 21.1.6`. Homebrew LLVM이 PATH 앞에 있으면 그 Clang으로 빌드되므로 뺀다.

## 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1 | 2026-11-21 | 첫 판. 설치 마법사(`Setup.bat`·`Setup.sh`)를 기본 흐름으로 두고 수동 설치는 부록으로. Windows는 이 PC에서 확인했다(마법사 `-Check`·자체 시험, `InstallLlvm.ps1`·`InstallFormatter.ps1`, Foundation Debug 빌드·시험 전부 통과). macOS는 CI의 `Setup.sh --check`만, Linux·Android는 코드 기준이다 |
| 2 | 2026-11-21 | 리뷰 반영: 빠른 시작에서 Git 설치 뒤 새 터미널, FXC SDK 항목(셰이더 시험용 SDK 10.0.22621), VS 17.8·MSVC 14.38 하한, macOS·Linux의 고정 내려받기(Homebrew 불필요, `dotnet-install.sh` 제거), Linux 마법사 설치와 영구 설정, 문제 해결 행 추가 |

TODO: VS 프로젝트 파일 생성([설계](../Design/ProjectFiles.ko.md))이 구현되면 3절에 생성 명령과 IDE에서 빌드·디버그하는 순서를 더한다.
