# 설치 마법사 설계

기준일: 2026-11-21, 개정 2. 저장소를 받은 뒤 가장 먼저 실행하는 설치 마법사(`Setup.bat`·`Setup.sh`)를 정한다. 사용 순서는
[빌드 환경 설정](../Guides/BuildEnvironment.ko.md)에 있다.

## 1. 목표

- 실행 하나로 끝난다. Windows는 탐색기에서 `Setup.bat`을 두 번 누르고, macOS는 `./Setup.sh`를 실행한다.
- 기본 실행은 질문을 한 번만 한다("Install the missing items? [Y/n]"). 옵션을 몰라도 된다.
- 항목마다 `[i/n] 이름 상태 내용` 한 줄로 보이고, 실패하면 원인과 `Next:`(다음 할 일)를 알린다. 자동으로 할 수 없는 경우는
  수동 설치 부록을 가리킨다.
- 여러 번 실행해도 결과가 같고, 이미 맞는 것은 다시 설치하지 않는다.
- 끝나면 다음 할 일 하나(새 터미널에서 Foundation Debug 시험)만 보인다.

## 2. 구조

| 파일 | 하는 일 |
|---|---|
| `Setup.bat`(루트) | `pwsh`가 있으면 그것으로, 없으면 Windows PowerShell 5.1로 `Build/Setup.ps1`을 실행한다. 처음 받은 Windows에는 `pwsh`가 없다 |
| `Build/Setup.ps1` | Windows 점검·설치. 5.1과 7 양쪽에서 돌도록 ASCII와 5.1 문법만 쓴다 |
| `Setup.sh`(루트) | `Build/Setup.sh`를 실행한다 |
| `Build/Setup.sh` | macOS 점검·설치(POSIX sh). Linux에서는 .NET SDK와 clang-format만 다룬다 |
| `.vsconfig`(루트) | Visual Studio 구성 요소 목록(C++ 데스크톱 워크로드, MSVC, Windows 11 SDK 26100·22621, Clang IntelliSense 두 개). `-WithIde`가 `--config`로 쓰고, Visual Studio도 저장소를 열 때 읽어 빠진 구성 요소를 안내한다 |

**빌드에는 Visual Studio IDE가 필요 없다.** clang-cl은 MSVC의 STL·C 런타임과 Windows SDK를 쓰고, Rocks.Build는 그것을
`vswhere -latest -products * -requires VC.Tools.x86.x64 -version [17.8,)`로 찾는다(`NativeEnvironment.cs`). `-products *`는 Build Tools도
찾는다. 하한 17.8과 MSVC 14.38.33130은 `Build/Toolchain.json`의 `visualStudio`가 정하고 빌드와 마법사가 함께 읽는다(UE 5.8과 같은 값,
[Clang 버전 지원](ClangVersionSupport.ko.md) 6.2절).
- 하한 이상의 C++ 설치가 없으면 기본으로 **Build Tools for Visual Studio 2026**(워크로드 `VCTools`, `VC.Tools.x86.x64`, `Windows11SDK.26100`,
  `Windows11SDK.22621`)을 설치한다. VS 2019 같은 하한 미만 설치는 쓰지도 고치지도 않는다.
- 하한 이상 설치가 있지만 C++ 도구가 없으면(예: C#용으로 깐 IDE) 두 번째 설치를 만들지 않고 그 설치에 C++ 구성 요소를 `modify`로 더한다.
  C++ 도구가 있는 설치에는 빠진 구성 요소만 더한다.
- IDE(Community)는 `-WithIde`일 때만 설치한다. 이때 IDE 단계를 빌드 도구 단계보다 먼저 돌리고, `.vsconfig`에 빌드 구성 요소가 모두 있으므로
  새 IDE가 빌드 도구도 맡는다(Build Tools를 따로 깔지 않는다).
- macOS의 기본은 Command Line Tools(`xcode-select --install`)이고 Xcode 앱은 필요 없다.

점검 항목과 설치 방법:

| 항목 | 점검 | 설치 |
|---|---|---|
| Git | `git` 존재, 실제로 적용되는 `core.longpaths=true`(Windows, 모든 설정 범위) | winget `Git.Git`, `git config --global core.longpaths true` |
| .NET SDK | `dotnet --list-sdks`에 `global.json` 버전대(같은 feature band, 패치는 고정값 이상) | Windows: winget `Microsoft.DotNet.SDK.<주>`를 `global.json` 버전으로. macOS·Linux: `Build/Toolchain.json` `dotnet.sdk.archives`의 공식 압축 파일을 SHA-512로 확인하고 `~/.dotnet`에 푼다 |
| PowerShell 7 | `pwsh` 7 이상(Windows는 늘, macOS는 `--android`일 때만 — `SelectAndroidNdk.ps1`이 쓴다) | winget `Microsoft.PowerShell`, macOS는 Homebrew `--cask powershell` |
| VS IDE(Windows, `-WithIde`) | 최신 IDE 설치(vswhere 기본 제품: Community·Professional·Enterprise, 하한 이상)에 `.vsconfig` 구성 요소 전부 | 있으면 `setup.exe modify --config .vsconfig`, 없으면 winget `Microsoft.VisualStudio.Community`(`--config .vsconfig`) |
| C++ 빌드 도구(Windows) | 위 vswhere 조건의 설치, 디스크의 Windows SDK, 고른 MSVC 도구 모음이 하한 이상(`ROCKS_MSVC_TOOLSET` 포함) | 위 규칙대로 `modify --add` 또는 winget `Microsoft.VisualStudio.BuildTools` |
| FXC SDK(Windows) | 고정 FXC 원본 파일(`Engine/Source/ThirdParty/D3DCompiler/ThirdParty.json`의 `sdkPath`, 지금 `bin/10.0.22621.0/x64/d3dcompiler_47.dll`)이 있는지 | `modify --add Windows11SDK.22621`. 셰이더 시험의 bootstrap만 쓰고 빌드에는 필요 없다([셰이더 시스템](ShaderSystem.ko.md) 8절 2항) |
| Xcode 도구(macOS) | `xcode-select -p`, PATH 첫 `clang++`가 Apple clang 최소 이상. 검증 목록 밖이면 내용에 CI 검증 줄을 함께 보인다(빌드는 "미검증 도구 모음" 알림 뒤 진행) | `xcode-select --install`(창을 마친 뒤 다시 실행) |
| LLVM(Windows) | 새 터미널 PATH의 첫 `clang-cl`이 `Build/Toolchain.json` 검증 목록에 있고, 고른 MSVC STL의 최소 Clang(`yvals_core.h` STL1000 두 형식) 이상 | 7-Zip이 없으면 winget `7zip.7zip`, `Build/InstallLlvm.ps1 -ToolchainRoot`, 사용자 PATH 맨 앞 |
| clang-format | `ROCKS_CLANG_FORMAT`이 가리키는 파일이 `clangFormat.version` | Windows: `Build/InstallFormatter.ps1 -ToolchainRoot`. macOS·Linux: `Setup.sh`가 같은 wheel(`clangFormat.packages`)을 SHA-256으로 확인하고 실행 파일만 꺼낸다. 이어서 `ROCKS_CLANG_FORMAT` 설정 |
| Android NDK(`-Android`) | SDK의 `ndk/<android.ndk.version>`과 `ANDROID_NDK_HOME`이 그 경로 | `Build/SelectAndroidNdk.ps1`(sdkmanager), `ANDROID_NDK_HOME` 설정 |

- **새 터미널 기준으로 본다.** Windows는 레지스트리의 시스템·사용자 환경(시스템 PATH 뒤 사용자 PATH)을, macOS·Linux는 현재 환경과 시작
  파일의 Rocks 블록을 읽는다. 마법사 프로세스 자신의 환경은 설치 직후 바뀌지 않기 때문이다.
- **설치 직전에 다시 본다.** 앞 단계의 설치가 뒤 항목을 이미 채웠을 수 있으므로(IDE가 빌드 도구를, Build Tools가 FXC SDK를) 각 항목은
  설치 바로 전에 한 번 더 점검하고, 통과하면 건너뛴다.
- **내려받기 고정.** 마법사가 직접 받는 파일은 `Build/Toolchain.json`이 URL과 해시를 정한다(LLVM 설치 파일 SHA-256, clang-format wheel
  SHA-256, .NET SDK 압축 파일 SHA-512 — Microsoft 릴리스 메타데이터의 값). `InstallFormatter.ps1`과 `Setup.sh`는 같은 항목을 읽는다.
  `dotnet-install.sh`처럼 검증 없이 받은 스크립트를 실행하지 않는다. winget·Visual Studio Installer·Homebrew는 각 배포 경로의 서명·해시를 쓴다.
- **설치 위치.** LLVM·clang-format은 저장소 밖 고정 위치(Windows `%LOCALAPPDATA%\Rocks\toolchains`, macOS
  `~/Library/Application Support/Rocks/toolchains`, Linux `~/.local/share/rocks/toolchains`)에 둔다. `out`을 지워도 PATH가 깨지지 않는다.
- **사용자 환경 쓰기(Windows).** 사용자 PATH는 레지스트리 원문(`%VARIABLE%`)과 값 종류(`REG_EXPAND_SZ`)를 보존한 채 맨 앞에 넣고,
  같은 디렉터리의 다른 철자는 지운다. 바꾼 뒤 `WM_SETTINGCHANGE`를 알려 탐색기에서 새로 연 프로그램이 새 값을 받게 한다.
- **시작 파일(macOS·Linux).** `# >>> Rocks setup >>>` 블록 안에 키마다 한 줄(`# rocks:<키>`)을 둔다. 같은 값이면 파일을 다시 쓰지 않는다.
  zsh는 `~/.zshrc`, bash는 macOS `~/.bash_profile`·Linux `~/.bashrc`, 그 밖은 `~/.profile`이다.
- **관리자 권한.** 마법사는 관리자로 돌지 않는다. Visual Studio Installer는 스스로 UAC를 띄운다. winget의 Git·.NET SDK·PowerShell·7-Zip
  패키지는 시스템 범위 설치 프로그램이라 UAC가 뜰 수 있다. LLVM·clang-format·환경 변수, macOS·Linux의 .NET SDK는 사용자 범위다.
- **다음 할 일.** winget이 없으면(Windows Server·LTSC) App Installer 또는 수동 설치 부록을, 내려받기 실패는 네트워크·프록시(`HTTPS_PROXY`)
  확인 뒤 다시 실행을 알린다. 그 밖에 자동 설치가 없는 항목도 부록을 가리킨다.
- **탐색기 실행.** 부모가 탐색기 → `cmd.exe`이면 끝에서 Enter를 기다린다(창이 바로 닫히지 않게). 터미널에서 실행하면 기다리지 않는다.
- **메시지.** 실행 중 메시지와 스크립트 주석은 영어다(`build-file-english`, 루트의 `Setup.bat`·`Setup.sh`·`.vsconfig`도 대상).

옵션: `-Check`(`--check`, 점검만, 모두 통과면 종료 코드 0), `-Yes`(`--yes`, 묻지 않음), `-WithIde`(Windows), `-Android`(`--android`),
`-ToolchainRoot`(Windows), `-Skip <항목 이름>`(Windows, 그 항목을 뺀다 — 셰이더 시험을 돌리지 않는 기계의 `'FXC SDK'`, CI가 쓴다),
`-SelfTest`(`--self-test`, 스크립트 단위 시험). 묻지 못하는 세션(입력 없음)에서는 설치하지 않고 `-Yes`를 안내한다.

## 3. 근거

- UE 5.8 `Setup.bat`·`Setup.sh`·`Setup.command`(저장소 루트)가 받은 직후 한 번 실행하는 진입점이다. UE는 의존 파일 내려받기·git hook·
  재배포 패키지 설치를 하고, 실패하면 `pause`로 창을 남긴다. Rocks는 의존 파일을 저장소에 두므로 같은 자리에서 도구 모음 점검·설치를 한다.
- UE UnrealBuildTool은 솔루션 옆에 `.vsconfig`(`{"version": "1.0", "components": [...]}`)를 써서 Visual Studio가 빠진 구성 요소를
  안내하게 한다(`VCProjectFileGenerator.cs` 863행, `MicrosoftPlatformSDK.VisualStudio.cs` `GetVisualStudioSuggestedComponents`).
  Rocks는 솔루션 생성이 아직 없으므로([VS 프로젝트 파일 생성](ProjectFiles.ko.md)) 루트에 고정 파일로 두고 마법사와 Visual Studio가 함께 읽는다.
- Visual Studio·MSVC 하한은 UE 5.8 `Engine/Config/Windows/Windows_SDK.json`(`MinimumVisualStudio2022Version` 17.8,
  `MinimumVisualStudio2026Version` 18.0, `MinimumVisualCppVersion` 14.38.33130)과 같다.
- 점검 규칙은 빌드와 같은 출처를 읽는다: `global.json`, `Build/Toolchain.json`(검증 목록·최소 Clang·NDK·clang-format·VS 하한), vswhere 조회 조건
  (`NativeEnvironment`), STL1000 조건(`ToolchainTable.ParseStlClangMinimum`과 같은 정규식), FXC 원본 경로(`D3DCompiler/ThirdParty.json`).

## 4. 검증

- `Build/Setup.ps1 -SelfTest`(CI Windows가 PowerShell 7과 5.1에서 돌린다)
  - 판단 함수: SDK 버전대, 검증 줄 대조, STL 조건(두 형식과 고른 도구 모음의 실제 `yvals_core.h`), VS 범위·도구 모음 버전, FXC 원본이
    마법사가 까는 SDK의 버전별 사본인지, `.vsconfig`가 빌드 구성 요소를 모두 담는지.
  - 환경 쓰기: PATH 맨 앞 넣기(철자·`%VARIABLE%`·빈 항목), 레지스트리 쓰기(임시 `HKCU\Software\RocksSetupSelfTest-<guid>` 키에서 원문·
    `REG_EXPAND_SZ` 보존과 두 번째 쓰기 무변경), 인자 인용.
  - 주 흐름(가짜 항목 주입): 모두 있으면 묻지 않고 0, 한 번 묻고 빠진 것만 설치, `-Check`·`n`·입력 없음은 설치하지 않고 1, 실패·수동 항목이
    다른 항목을 막지 않음, 앞 설치가 채운 항목은 설치하지 않음.
- CI Windows는 이어서 고정 LLVM `bin`을 시스템 PATH 맨 앞에, `ROCKS_CLANG_FORMAT`을 사용자 환경에 넣고(러너는 job 뒤 버려진다)
  `Setup.ps1 -Check -Skip 'FXC SDK'`가 0인지 본다. 러너 이미지에 SDK 10.0.22621이 없어서 FXC SDK만 뺀다.
- `Build/Setup.sh --self-test`: SDK 버전대, Clang 주 버전, JSON 값 읽기(실제 `global.json`·`Toolchain.json`, 고정 .NET SDK와 `global.json`
  일치, 플랫폼별 wheel·압축 파일 항목 형식), SHA 계산, 내려받기 실패의 `Next:`, 시작 파일 블록 쓰기(임시 파일). CI macOS는 이어서 실제
  `--check`를 돌린다.
- 개발 PC에서 고정 내려받기(wheel 두 개, .NET SDK osx-arm64 압축 파일)의 URL·해시를 실제로 받아 확인했다.

## 5. 한계

- 설치 단계(winget·Visual Studio Installer·Homebrew·실제 압축 풀기)는 시스템을 바꾸므로 자동 시험이 없다. 단위 시험은 설치 명령 앞뒤의
  판단·흐름·환경 쓰기를 덮는다.
- 시스템 PATH에 다른 `clang-cl`이 있으면 사용자 PATH가 이길 수 없다. 마법사는 그 위치와 할 일을 알리고 시스템 PATH는 고치지 않는다(관리자 권한).
- macOS의 PowerShell 설치(`--android`일 때만)는 Homebrew가 있어야 자동이다. 없으면 설치 방법을 알린다.
- winget 패키지 ID(`Microsoft.VisualStudio.BuildTools`·`Microsoft.VisualStudio.Community`)가 가리키는 판은 winget 저장소가 정한다
  (2026-11-21 조회에서 18.12.1, VS 2026). `--version 10.0.302`는 그 버전을 정확히 설치하고, 더 새 SDK가 이미 있어도 나란히 들어간다
  (`global.json` latestPatch가 고른다) — 로컬 미검증.
- Linux는 C++ 빌드 호스트가 아니므로([빌드 타깃](BuildTargets.ko.md) 3.7절) .NET SDK와 clang-format만 다룬다(x86_64만 고정 파일이 있다).
  Linux 빌드 플랫폼(BS6)이 생기면 컴파일러 점검을 더한다.
- macOS·Linux 경로는 CI macOS의 `--check`와 내려받기 확인 외에 로컬 실행 기록이 없다.

## 6. 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1 | 2026-11-21 | 첫 판. `Setup.bat`·`Build/Setup.ps1`·`Setup.sh`·`Build/Setup.sh`·`.vsconfig`, CI 자체 시험. 기본은 Build Tools(Windows)·Command Line Tools(macOS)이고 IDE는 `-WithIde` |
| 2 | 2026-11-21 | 리뷰 10건 반영. FXC SDK 항목과 SDK 22621(셰이더 시스템 개정 22), VS 17.8·MSVC 14.38.33130 하한, STL1000 두 형식, `-WithIde`가 IDE를 먼저 깔고 재사용·C++ 없는 설치는 modify, 설치 직전 재점검, macOS·Linux 내려받기 고정(.NET SDK SHA-512, clang-format wheel)과 Homebrew 없는 기본 흐름, Linux의 .NET·clang-format 설치, `Next:` 보강, 가짜 항목 주 흐름 시험과 CI Windows 실제 `-Check`(`-Skip`) |
