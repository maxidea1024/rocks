# 설치 마법사 설계

기준일: 2026-11-21, 개정 1. 저장소를 받은 뒤 가장 먼저 실행하는 설치 마법사(`Setup.bat`·`Setup.sh`)를 정한다. 사용 순서는
[빌드 환경 설정](../Guides/BuildEnvironment.ko.md)에 있다.

## 1. 목표

- 실행 하나로 끝난다. Windows는 탐색기에서 `Setup.bat`을 두 번 누르고, macOS는 `./Setup.sh`를 실행한다.
- 기본 실행은 질문을 한 번만 한다("Install the missing items? [Y/n]"). 옵션을 몰라도 된다.
- 항목마다 `[i/n] 이름 상태 내용` 한 줄로 보이고, 실패하면 원인과 다음 할 일을 한 줄로 알린다.
- 여러 번 실행해도 결과가 같고, 이미 맞는 것은 다시 설치하지 않는다.
- 끝나면 다음 할 일 하나(새 터미널에서 Foundation Debug 시험)만 보인다.

## 2. 구조

| 파일 | 하는 일 |
|---|---|
| `Setup.bat`(루트) | `pwsh`가 있으면 그것으로, 없으면 Windows PowerShell 5.1로 `Build/Setup.ps1`을 실행한다. 처음 받은 Windows에는 `pwsh`가 없다 |
| `Build/Setup.ps1` | Windows 점검·설치. 5.1과 7 양쪽에서 돌도록 ASCII와 5.1 문법만 쓴다 |
| `Setup.sh`(루트) | `Build/Setup.sh`를 실행한다 |
| `Build/Setup.sh` | macOS 점검·설치(POSIX sh). Linux에서는 점검만 한다 |
| `.vsconfig`(루트) | IDE로 편집할 때의 Visual Studio 구성 요소 목록(C++ 데스크톱 워크로드, MSVC, Windows 11 SDK, Clang IntelliSense 두 개). `-WithIde`가 `--config`로 쓰고, Visual Studio도 저장소를 열 때 읽어 빠진 구성 요소를 안내한다 |

**빌드에는 Visual Studio IDE가 필요 없다.** clang-cl은 MSVC의 STL·C 런타임과 Windows SDK를 쓰고, Rocks.Build는 그것을
`vswhere -latest -products * -requires VC.Tools.x86.x64`로 찾는다(`NativeEnvironment.cs`). `-products *`는 Build Tools도 찾는다. 그래서
Visual Studio가 없으면 기본으로 **Build Tools for Visual Studio 2026**(워크로드 `VCTools`, `VC.Tools.x86.x64`, `Windows11SDK.26100`)을
설치한다. IDE(Community)는 `-WithIde`일 때만 설치한다. 이미 Visual Studio나 Build Tools가 있으면 그 설치에 빠진 구성 요소만 `modify`로
더한다. macOS도 기본은 Command Line Tools(`xcode-select --install`)이고 Xcode 앱은 필요 없다.

점검 항목과 설치 방법:

| 항목 | 점검 | 설치 |
|---|---|---|
| Git | `git` 존재, `core.longpaths=true`(Windows) | winget `Git.Git`, `git config --global core.longpaths true` |
| .NET SDK | `dotnet --list-sdks`에 `global.json` 버전대(같은 feature band, 패치는 고정값 이상) | Windows: winget `Microsoft.DotNet.SDK.<주>`를 `global.json` 버전으로. macOS: `dotnet-install.sh --version`을 `~/.dotnet`에 |
| PowerShell 7 | `pwsh` 7 이상 | winget `Microsoft.PowerShell`, macOS는 Homebrew `--cask powershell` |
| C++ 빌드 도구(Windows) | 빌드가 고르는 설치(위 vswhere 조건), 디스크의 Windows SDK, `ROCKS_MSVC_TOOLSET` 도구 모음 | 있으면 `setup.exe modify --add Windows11SDK.26100`, 없으면 winget `Microsoft.VisualStudio.BuildTools`(`--add` 위 세 항목) |
| VS IDE(Windows, `-WithIde`) | 최신 IDE 설치(vswhere 기본 제품: Community·Professional·Enterprise)에 `.vsconfig` 구성 요소 전부 | 있으면 `setup.exe modify --config .vsconfig`, 없으면 winget `Microsoft.VisualStudio.Community`(`--config .vsconfig`) |
| Xcode 도구(macOS) | `xcode-select -p`, PATH 첫 `clang++`가 Apple clang 최소 이상. 검증 목록 밖이면 내용에 CI 검증 줄을 함께 보인다(빌드는 "미검증 도구 모음" 알림 뒤 진행) | `xcode-select --install`(창을 마친 뒤 다시 실행) |
| LLVM(Windows) | 새 터미널 PATH의 첫 `clang-cl`이 `Build/Toolchain.json` 검증 목록에 있고, 고른 MSVC STL의 최소 Clang(`yvals_core.h` STL1000) 이상 | 7-Zip이 없으면 winget `7zip.7zip`, `Build/InstallLlvm.ps1 -ToolchainRoot`, 사용자 PATH 맨 앞 |
| clang-format | `ROCKS_CLANG_FORMAT`이 가리키는 파일이 `clangFormat.version` | `Build/InstallFormatter.ps1 -ToolchainRoot`, `ROCKS_CLANG_FORMAT` 설정 |
| Android NDK(`-Android`) | SDK의 `ndk/<android.ndk.version>`과 `ANDROID_NDK_HOME`이 그 경로 | `Build/SelectAndroidNdk.ps1`(sdkmanager), `ANDROID_NDK_HOME` 설정 |

- **새 터미널 기준으로 본다.** Windows는 레지스트리의 시스템·사용자 환경(시스템 PATH 뒤 사용자 PATH)을, macOS는 현재 환경과 시작 파일의
  Rocks 블록을 읽는다. 마법사 프로세스 자신의 환경은 설치 직후 바뀌지 않기 때문이다.
- **설치 위치.** LLVM·clang-format은 저장소 밖 고정 위치(Windows `%LOCALAPPDATA%\Rocks\toolchains`, macOS
  `~/Library/Application Support/Rocks/toolchains`)에 둔다. `out`을 지워도 PATH가 깨지지 않는다. 설치 스크립트는 기존 것
  (`InstallLlvm.ps1`·`InstallFormatter.ps1`·`SelectAndroidNdk.ps1`)을 그대로 쓰고, 고정 버전·SHA-256의 출처는 바뀌지 않는다.
- **사용자 환경 쓰기(Windows).** 사용자 PATH는 레지스트리 원문(`%VARIABLE%`)과 값 종류(`REG_EXPAND_SZ`)를 보존한 채 맨 앞에 넣고,
  같은 디렉터리의 다른 철자는 지운다. 바꾼 뒤 `WM_SETTINGCHANGE`를 알려 탐색기에서 새로 연 프로그램이 새 값을 받게 한다.
- **시작 파일(macOS).** `# >>> Rocks setup >>>` 블록 안에 키마다 한 줄(`# rocks:<키>`)을 둔다. 같은 값이면 파일을 다시 쓰지 않는다.
  zsh는 `~/.zshrc`, bash는 `~/.bash_profile`, 그 밖은 `~/.profile`이다.
- **관리자 권한.** 마법사는 관리자로 돌지 않는다. Visual Studio Installer는 스스로 UAC를 띄운다. winget의 Git·.NET SDK·PowerShell·7-Zip
  패키지는 시스템 범위 설치 프로그램이라 UAC가 뜰 수 있다. LLVM·clang-format·환경 변수는 사용자 범위다.
- **탐색기 실행.** 부모가 탐색기 → `cmd.exe`이면 끝에서 Enter를 기다린다(창이 바로 닫히지 않게). 터미널에서 실행하면 기다리지 않는다.
- **메시지.** 실행 중 메시지와 스크립트 주석은 영어다(`build-file-english`, 루트의 `Setup.bat`·`Setup.sh`·`.vsconfig`도 대상).

옵션: `-Check`(`--check`, 점검만, 모두 통과면 종료 코드 0), `-Yes`(`--yes`, 묻지 않음), `-WithIde`(Windows), `-Android`(`--android`),
`-ToolchainRoot`(Windows), `-SelfTest`(`--self-test`, 스크립트 단위 시험). 묻지 못하는 세션(입력 없음)에서는 설치하지 않고 `-Yes`를 안내한다.

## 3. 근거

- UE 5.8 `Setup.bat`·`Setup.sh`·`Setup.command`(저장소 루트)가 받은 직후 한 번 실행하는 진입점이다. UE는 의존 파일 내려받기·git hook·
  재배포 패키지 설치를 하고, 실패하면 `pause`로 창을 남긴다. Rocks는 의존 파일을 저장소에 두므로 같은 자리에서 도구 모음 점검·설치를 한다.
- UE UnrealBuildTool은 솔루션 옆에 `.vsconfig`(`{"version": "1.0", "components": [...]}`)를 써서 Visual Studio가 빠진 구성 요소를
  안내하게 한다(`VCProjectFileGenerator.cs` 863행, `MicrosoftPlatformSDK.VisualStudio.cs` `GetVisualStudioSuggestedComponents`).
  Rocks는 솔루션 생성이 아직 없으므로([VS 프로젝트 파일 생성](ProjectFiles.ko.md)) 루트에 고정 파일로 두고 마법사와 Visual Studio가 함께 읽는다.
- 점검 규칙은 빌드와 같은 출처를 읽는다: `global.json`, `Build/Toolchain.json`(검증 목록·최소 Clang·NDK·clang-format), vswhere 조회 조건
  (`NativeEnvironment`), STL1000 조건(`ToolchainTable.ParseStlClangMinimum`과 같은 정규식).

## 4. 검증

- `Build/Setup.ps1 -SelfTest`: SDK 버전대, 검증 줄 대조, STL 조건 읽기, PATH 맨 앞 넣기(철자·`%VARIABLE%`·빈 항목), 레지스트리 쓰기
  (임시 `HKCU\Software\RocksSetupSelfTest-<guid>` 키에서 원문·`REG_EXPAND_SZ` 보존과 두 번째 쓰기 무변경), 인자 인용.
  CI Windows가 PowerShell 7과 5.1에서 돌린다.
- `Build/Setup.sh --self-test`: SDK 버전대, Clang 주 버전, JSON 값 읽기(실제 `global.json`·`Toolchain.json`), 시작 파일 블록 쓰기
  (임시 파일). CI macOS는 이어서 실제 `--check`를 돌린다(이미지에 필요한 것이 모두 있다).
- Windows `-Check`는 개발 PC에서 실제로 돌려 확인한다. CI Windows는 고정 LLVM이 그 job의 PATH에만 있어 `-Check`를 돌리지 않는다.

## 5. 한계

- 설치 단계(winget·Visual Studio Installer·`dotnet-install.sh`·Homebrew)는 시스템을 바꾸므로 자동 시험이 없다. 단위 시험은 설치
  명령 앞뒤의 판단과 환경 쓰기만 덮는다.
- 시스템 PATH에 다른 `clang-cl`이 있으면 사용자 PATH가 이길 수 없다. 마법사는 그 위치와 할 일을 알리고 시스템 PATH는 고치지 않는다(관리자 권한).
- macOS의 PowerShell 설치는 Homebrew가 있어야 자동이다. 없으면 설치 방법을 알린다. `dotnet-install.sh`는 해시를 고정하지 않은 Microsoft
  공식 스크립트를 받는다.
- Linux는 빌드 호스트가 아니므로([빌드 타깃](BuildTargets.ko.md) 3.7절) 점검만 한다. Linux 빌드 플랫폼(BS6)이 생기면 설치를 더한다.
- macOS·Linux 경로는 CI macOS의 `--check` 외에 로컬 실행 기록이 없다.

## 6. 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1 | 2026-11-21 | 첫 판. `Setup.bat`·`Build/Setup.ps1`·`Setup.sh`·`Build/Setup.sh`·`.vsconfig`, CI 자체 시험. 기본은 Build Tools(Windows)·Command Line Tools(macOS)이고 IDE는 `-WithIde` |
