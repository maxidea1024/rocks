# 셰이더 시스템 설계 — 8. P3 ThirdParty 확장과 도구 빌드

[목차](README.ko.md)

## 8. P3 ThirdParty 확장과 도구 빌드

현재 ThirdParty 기반은 C 원본 라이브러리용이다. 루트가 `Core/Source/ThirdParty`로 고정되어 있고(`ThirdPartyManifest.cs:76`, 같은 상수를 `Build/Rules/ThirdPartyModuleRules.cs:37`·`Programs/Rocks.Tooling.Checks/ThirdPartyDirectiveProbe.cs:34`·`ThirdPartyManifestRefresh.cs:22`·`PolicyChecks.ThirdParty.cs:37`·`UnicodeTables.cs:40,75`가 쓴다), 번역 단위는 `.c`만 받으며(`ThirdPartyManifest.cs:357–364`), 모든 원본이 Rocks 경고·예외 규칙(Windows `/W4 /WX`, C++ `/GR- /EHs-c-`, POSIX `-Wall -Wextra -Wpedantic -Werror`, C++ `-fno-exceptions -fno-rtti`)으로 빌드되고 모듈이 이를 바꿀 수 없다(`Programs/Rocks.Build/ActionGraph.cs:390–436`, `InputValidation.cs:165–245`). `kind`는 `code`·`data`(CoreDataFoundations C1)뿐이고 미리 빌드된 바이너리를 적는 필드가 없다.

**도구 공급은 UE 방식이다(2026-11-13 사용자 결정 8).** UE 5.8의 구조는 다음과 같다.
- DXC와 ShaderConductor(SPIRV-Cross·SPIRV-Tools를 정적으로 포함)를 엔진 빌드(UBT) 밖의 스크립트로 빌드한다. Windows는 CMake의 Visual Studio 2022 생성기와 MSBuild를 쓰고 5.7부터 기본 toolset이 ClangCL이다(`ThirdParty/ShaderConductor/Build_ShaderConductor_Win64.bat:16–23,65,74–75`). macOS는 CMake·Ninja와 Python `BuildAll.py`를 쓰고 `install_name_tool -rpath` 교체와 `dsymutil`까지 한다(`Build_ShaderConductor_Mac.command:1–51`, `:46–47`).
- 결과 공유 라이브러리(Win64는 `ShaderConductor.dll`·`dxcompiler.dll`, 스크립트가 복사 `Build_ShaderConductor_Win64.bat:91–99`)를 `Engine/Binaries/ThirdParty/ShaderConductor/<플랫폼>`에 두고 GitDependencies로 배포한다. 같은 디렉터리의 `dxil.dll`은 빌드하지 않고 Microsoft 배포본을 GitDependencies로 받는다(`Engine/Build/Commit.gitdeps.xml:39124`, 받은 파일은 SHA-1로 검사, `Programs/GitDependencies/Program.cs:1315–1461`).
- 엔진 쪽 모듈은 `Type = ModuleType.External`이다(`ThirdParty/ShaderConductor/ShaderConductor.Build.cs:7–72`, External `:13`).

Rocks도 같은 구조로 한다. 그래서 Rocks.Build에 공유 라이브러리 모듈 종류나 C++17·예외 빌드 설정을 더하지 않는다. 각 도구는 자기 공유 라이브러리 안에서 예외·STL을 쓰고, 밖으로는 C 또는 COM 형태 API만 내보낸다. UE와 다른 점은 래퍼 없이 업스트림 공유 라이브러리를 쓰는 것이다(1절).
- DXC: `DxcCreateInstance`와 `IDxcCompiler3` 같은 COM 형태 인터페이스를 내보낸다. 오류는 HRESULT로 돌려준다. UE도 D3D12 DXIL 경로는 `IDxcCompiler3`로 직접 부른다(`Developer/Windows/ShaderFormatD3D/Private/D3DShaderCompilerDXC.cpp:559–560`).
- SPIRV-Cross: `spirv_cross_c.h` C API를 단일 공유 라이브러리로 만든다(업스트림 CMake `SPIRV_CROSS_SHARED`, `spirv-cross-c-shared`). 예외는 라이브러리 안에서 켜져 있고 C API가 예외를 오류 값으로 바꾼다(업스트림 `spirv_cross_c.cpp`의 `SPVC_BEGIN_SAFE_SCOPE`).
- SPIRV-Tools: `spirv-tools/libspirv.h` C API(`spvValidate`·최적화기 C API)를 공유 라이브러리로 만든다.

1. **Engine 쪽 루트.** `ThirdPartyRoot` 상수를 루트 목록(`Core/Source/ThirdParty`, `Engine/Source/ThirdParty`)으로 바꾼다.
   - 의존 방향은 그대로다. Core가 쓰는 라이브러리는 Core 루트에, 엔진·도구 전용은 Engine 루트에 둔다.
   - 바꿀 코드: 위 상수와 그것을 쓰는 다섯 곳, `.c` 위치 검사(`PolicyChecks.ThirdParty.cs:457–459`), lexical 제외(`PolicyChecks.cs:371,383`), SDK 라이선스 복사(`ThirdPartyManifest.cs:227`), 도구 시험, `.gitattributes:9–11`·`.editorconfig:38–47`(원본 byte 보존 규칙).
   - 문서 개정은 14절에 있다.
2. **미리 빌드된 바이너리(`kind: "binary"`).** 엔진 빌드가 컴파일하지 않고 워커가 실행 중에 여는 도구다(9절). manifest 항목:
   - `version`, 라이선스(`spdx`·고지 파일), 출처. 출처는 Rocks 도구 빌드가 게시한 archive의 URL·`sha256`과 그 빌드의 원본 태그·patch 목록이다. 게시 위치는 저장소의 GitHub Releases다(2026-11-16 사용자 결정). 수동 실행 도구 빌드 workflow(4항)가 Release에 올리고, 비공개 저장소이면 bootstrap이 `gh` 인증으로 받는다. 설치된 SDK에서 가져오는 도구는 그 SDK의 경로 규칙을 적는다.
   - 호스트별 파일 목록 `hosts.<플랫폼>-<아키텍처>[] = {path, sha256, size, role}`.
     - 호스트 키는 `windows-x64`·`macos-arm64`처럼 아키텍처를 포함한다(계획 §2.5: x64·ARM64 산출물을 섞지 않는다).
     - 지원 개발 호스트는 Windows·macOS뿐이다(`Programs/Rocks.Build/Toolchain.cs:21–25`).
     - 포매터 설치(`Build/InstallFormatter.ps1`, `out/toolchains`)는 빌드 입력이 아니므로 기존 관례를 유지한다.
   - `role` 값: `runtime`(워커가 여는 공유 라이브러리), `redist`(Runtime 실행 파일 옆에 복사만 하는 재배포 파일, 9절 Agility SDK), `symbols`(PDB·dSYM), `tool`, `notice`. 링크하지 않으므로 import library는 두지 않는다.
   - **공개 헤더는 저장소에 둔다.** 같은 버전의 `dxcapi.h`·`spirv_cross_c.h`·`libspirv.h`는 작은 텍스트 파일이므로 `Engine/Source/ThirdParty/<Lib>/Include/`에 커밋하고 manifest가 파일 SHA-256을 가진다(외부 include 옵션이 금지이므로 기존 `adapterHeaders`와 같은 방식으로 경고 0을 확인한다). 그래서 워커와 ShaderFormat 모듈은 bootstrap 없이 빌드되고, 기본 빌드와 Foundation CI는 사용자 저장소에 기대지 않는다. bootstrap이 필요한 것은 워커가 도구를 여는 실행·시험뿐이다.
   - **저장 위치.** 바이너리는 저장소와 `out/`에 두지 않는다.
     - 사용자 단위 내용 주소 저장소를 쓴다. Windows는 `%LOCALAPPDATA%\Rocks\ThirdParty\<archive sha256>\`, macOS는 `~/Library/Application Support/Rocks/ThirdParty/<archive sha256>/`이다. archive 단위로 풀어 둔다: 한 도구의 공유 라이브러리가 같은 디렉터리의 다른 라이브러리에 의존할 수 있기 때문이다(P10 `Open`이 같은 디렉터리 의존을 푼다). `Caches`는 OS가 지울 수 있어 쓰지 않는다. 선택 미러 경로도 둔다.
     - 명시적인 bootstrap 명령에서만 받는다. UE GitDependencies와 같은 역할이다(UE는 SHA-1, Rocks는 SHA-256).
     - 빌드 action과 워커 안에서는 받지 않는다. 없으면 bootstrap을 안내하는 오류로 실패한다. 그래서 깨끗한 `out/`도 네트워크 없이 빌드된다(계획 §27.9).
     - CI는 manifest SHA를 키로 저장소를 캐시한다(`.github/workflows/foundation.yml`에 캐시 단계 추가, 지금은 캐시 단계가 없다).
   - 받은 파일은 임시 경로에 쓰고 archive·파일 SHA-256을 검사한 뒤 옮긴다. 검사에 실패하면 오류다. macOS에서는 SHA 검사 뒤 quarantine 확장 속성(`com.apple.quarantine`)을 확인해 지운다(브라우저로 받은 미러 파일을 `dlopen`하면 Gatekeeper가 막는다). 저장소의 파일은 그 밖에는 고치지 않는다(install name 수정과 서명은 도구 빌드에서 끝낸다).
   - **로드 규칙.** binary 항목의 파일을 여는 것은 `Programs/ShaderCompileWorker`와 그 안의 `Developer/ShaderFormat*`뿐이다(9절).
   - **FXC.** System32의 `d3dcompiler_47.dll`은 OS 빌드마다 다르므로 쓰지 않는다.
     - 버전을 고정한 Windows SDK를 설치하고, bootstrap이 그 SDK의 버전별 사본 `bin/10.0.22621.0/x64/d3dcompiler_47.dll`을 SHA-256 검사 뒤 저장소로 복사한다(개정 22).
       - 버전 구분 없는 `Redist/D3D/x64/d3dcompiler_47.dll`은 쓰지 않는다. 나중에 설치한 SDK가 같은 경로를 덮어쓴다(2026-11-21 실측: SDK 10.0.26100을 설치한 기계에서 그 파일이 10.0.26100.9457, `1da540a0…`가 되어 bootstrap이 SHA 불일치로 실패했다).
       - 버전별 사본은 같은 파일 버전 10.0.22621.2428·크기 4,916,728 B이지만 SHA-256이 `Redist` 본과 다르다(`289767b4…` 대 `af7b99be…`). 고정은 버전별 사본 쪽으로 한다.
       - 버전별 사본은 REDIST.TXT 목록에 없다(목록은 `Redist\D3D\x64\` 경로로 적는다). bootstrap의 저장소 복사는 설치한 사람 자신의 기계에서 쓰는 것이라 재배포가 아니다. 개발 도구 묶음에 넣어 다른 사람에게 줄 때는 아래 재배포 조건대로 `Redist\D3D\x64` 본을 써야 하고, 그 묶음을 만들 때 그 본을 따로 고정한다.
     - Windows SDK는 archive가 아니라 설치기라서 다시 게시하지 않는다.
     - **재배포 조건(측정 S10, 2026-11-20, 개정 21).** SDK 사용 조건의 Distributable Code가 가리키는 REDIST.TXT 목록(`learn.microsoft.com/legal/windows-sdk/redist`)에 `Redist\D3D\x64\d3dcompiler_47.dll`이 있고, 고치지 않은 파일을 Classic Windows 응용과 함께만 재배포할 수 있다(UWP 앱 금지, 배포 요건은 5.4절 S10).
       - Runtime 실행 파일에는 넣지 않는다(Release 배제, 9절).
       - 개발 도구(편집기·쿠커·워커) 배포 묶음에는 응용 옆 복사본으로 넣을 수 있고, 그때 사용 조건의 배포 요건을 따른다. UE도 `Engine/Binaries/ThirdParty/Windows/DirectX/x64`에 동봉한다(10.0.19041.1).
       - DLL만 따로(저장소·GitHub Releases) 올리지 않는 지금 규칙은 그대로다. 목록은 프로그램의 일부로 배포하는 것을 허락하므로 DLL만 게시하는 것은 그 허락 밖이다.
       - 고정 버전은 SDK 10.0.22621.0의 10.0.22621.2428이다. bootstrap 원본(버전별 사본)의 SHA-256은 `289767b49dc8525bce262ea06fee4ef9fced2e131a55b25654ce871c1f0498b2`, S10 때 잰 `Redist` 본은 `af7b99be1b8770c0e4d18e43b04e81d11bdeb667fa6b07ade7a88f4c5676bf9a`다. 더 새 SDK로 올리면 도구 지문이 바뀌어 FXC job이 모두 다시 컴파일되므로 manifest 재고정과 함께 따로 한다.
     - UE도 동봉본(`Engine/Binaries/ThirdParty/Windows/DirectX/x64/d3dcompiler_47.dll`)을 전체 경로로 연다(`D3DShaderCompiler.cpp:399–408`).
   - **Apple `metal`·`metallib`.** 저장소로 받지 않는 시스템 도구다(Xcode Metal 도구 또는 Windows용 Metal Developer Tools).
     - manifest에 `kind: binary`, `status: system`으로 적고 최소 버전만 고정한다. 실행 파일 SHA는 고정하지 않는다: Xcode 26부터 Metal 도구는 별도 구성요소(`xcodebuild -downloadComponent metalToolchain`)라 갱신될 때마다 바뀌기 때문이다. 대신 워커가 실제 도구의 지문(`metal -v` 문자열과 `metal`·`metallib` SHA-256)을 재서 메인 프로세스에 알리고(11절 도구 조회), 그 값이 job 키에 들어간다(10절 3항). CI는 Xcode 버전을 고정하고 Metal 도구 설치 단계를 둔다.
     - UE 5.8은 Windows Metal Developer Tools 5.3(32023)을 기대하고 `metal-pack`·`metal-strip`을 쓴다(`MetalShaderFormat.cpp:724–725`, 사용 `:1060,1072`, 실행 파일 이름 `:704–709`).
3. **헤더 전용(`kind: "headers"`).** Vulkan-Headers·metal-cpp처럼 번역 단위 없이 헤더만 쓰는 라이브러리다. manifest는 파일 SHA-256과 include 디렉터리만 가지며, 소비 모듈의 Private include로만 들어가고 `adapterHeaders` 방식으로 경고 0을 확인한다. `code`는 번역 단위가 있어야 하고 `data`는 include 대상이 아니라서 새 종류가 필요하다.
4. **오프라인 도구 빌드(UE 방식).** DXC·SPIRV-Cross·SPIRV-Tools는 라이브러리마다 `Engine/Source/ThirdParty/<Lib>/Build/`의 스크립트(Windows `.ps1`·`.cmd`, macOS `.sh`)로 빌드한다.
   - 입력:
     - 고정한 원본 태그. DXC는 1.8.2505 이상이다(10절 서명).
     - 원본 SHA. 원본은 스크립트가 받아 SHA를 검사한다.
     - Rocks patch 목록. 예를 들어 10절 SPIRV-Cross 실수 출력 수정이나 7.2절 11항 GLES pass가 업스트림으로 안 되면 여기에 넣는다.
     - 저장소에는 스크립트·patch·manifest·공개 헤더만 둔다.
   - 도구: CMake·Ninja와 호스트 컴파일러(Windows clang-cl 또는 MSVC, macOS Xcode clang, 측정 S6). DXC와 SPIRV-Tools 빌드는 Python으로 표를 생성한다.
   - 버전 정합: SPIRV-Tools·SPIRV-Headers는 고정한 DXC 릴리스의 submodule 커밋으로 빌드한다. 그러면 `spirv-val`이 DXC가 만든 SPIR-V와 같은 버전이다.
   - 산출물:
     - Windows x64: DLL·PDB.
     - macOS arm64: dylib(install name `@loader_path/<이름>`이라 절대 경로로 열어도 같은 디렉터리의 의존을 찾는다, ad-hoc 서명)·dSYM.
     - 공용 헤더·라이선스 고지.
     - 이것을 archive로 묶어 SHA-256과 함께 게시하고, 2항 manifest에 등록한다.
     - UE처럼 Windows DXC도 같은 태그에서 직접 빌드한다. 두 호스트가 같은 원본·버전을 쓴다.
   - 실행 위치:
     - 수동으로 실행하는 별도 CI workflow(`.github/workflows/third-party-tools.yml`)가 두 호스트에서 빌드·게시한다. 개발자 기계에서도 같은 스크립트로 재현한다.
     - 이 workflow는 기본 빌드·시험 경로가 아니다. Python을 막는 정책 검사(`Programs/Rocks.Tooling.Checks/PolicyChecks.cs:313–349` `PythonFreeMainPath`)는 `Programs`·`Build`·`.github`만 보므로(`:316`) 이 workflow 파일만 명시 제외로 더한다. `Engine/Source/ThirdParty/*/Build/`는 검사 범위 밖이다.
     - 엔진 빌드·시험과 Foundation CI는 계속 Python 없이 돈다.
   - 경고·예외: 이 빌드는 업스트림 빌드 설정(예외·RTTI·경고)으로 컴파일하며 Rocks 빌드가 컴파일하는 대상이 아니다(UE의 External 모듈과 같다). CompilerDiagnostics는 "외부 원본도 엔진과 같은 경고 프로필로 컴파일한다"고 하므로 이 점은 15절 결정 7로 확인받는다. Rocks 코드는 공개 헤더만 include하므로 헤더의 경고 0을 확인한다(2항 저장소 공개 헤더). 게시한 archive 안의 공개 헤더 SHA-256이 저장소에 커밋한 헤더와 같은지 workflow가 확인한다.
5. **C 원본.** SPIRV-Reflect는 C이므로 기존 `code` 규칙 그대로 Engine 루트에 둔다.

완료 기준:
- 두 루트의 manifest·정책 시험, `binary`·`headers` 종류의 음성 대조(digest 불일치·호스트 누락).
- bootstrap의 정상 경로·SHA 불일치·중단 뒤 재시도, 그리고 저장소만으로 하는 오프라인 빌드 시험.
- 도구 빌드 workflow가 Windows·macOS 산출물을 게시하고, 그 SHA가 manifest와 일치(install name·서명·dSYM 검사 포함).
- FXC 고정본·Apple 도구 최소 버전 등록, macOS quarantine 속성 제거 시험.
- bootstrap 없이 기본 빌드·Foundation CI가 통과한다(헤더는 저장소, 바이너리는 실행 때만).
- SPIRV-Reflect가 Rocks 경고 규칙으로 Windows·macOS에서 빌드되고, Vulkan-Headers가 경고 0으로 include된다.

진척(2026-11-16, `feature/shader-thirdparty`):
- [x] 두 루트(`ThirdPartyManifest.ThirdPartyRoots`)와 `headers`·`binary` 종류의 manifest·정책 시험. 음성 대조: 커밋한 헤더 digest 불일치, 호스트 누락(`ThirdPartyBinary.Host`), 모르는 필드(최상위·호스트·호스트 파일·build), asset 이름 규칙, 루트 사이 이름 중복(`Policy.ThirdPartyEngineRoot`·`Policy.ThirdPartyHeadersLibrary`·`Policy.ThirdPartyBinaryLibrary`·`Policy.ThirdPartyBinarySdkAndSystem`).
- [x] CompilerDiagnostics 미리 빌드한 바이너리 승인 절·셰이더 컴파일 경고 절, RepositoryLayout·CoreDataFoundations·Compression 개정.
- [x] Vulkan-Headers(``kind: headers``)·SPIRV-Reflect(``kind: code``)를 Vulkan SDK 1.4.363.0 태그(DXC v1.9.2609 submodule과 같은 SDK 태그)로 Engine 루트에 반입. SPIRV-Reflect가 Rocks 경고 규칙으로 빌드되고 두 헤더가 경고 0으로 include됨을 EngineTests(``Programs/FoundationTests/Engine/ThirdPartyShaderToolTests.cpp``)로 확인(Windows. macOS는 CI에 EngineTests가 없어 미검증).
- [x] bootstrap 명령(``Rocks.Build.Cli bootstrap``·``/t:ThirdPartyBootstrap``, ``ThirdPartyStore``): 임시 경로 → archive·파일 SHA-256·크기 검사 → 이동, 미러·``gh`` 인증 다운로드, 잠금(기다리지 않음), 빌드·워커 경로에서 받지 않음(정책 시험). 시험: 정상·SHA 불일치·중단 뒤 재시도·취소·오프라인·호스트 누락·손상 저장소·archive 경로 탈출과 링크(``ThirdPartyStore.*``). FXC 고정본(Windows SDK 10.0.22621.0의 ``d3dcompiler_47.dll`` 10.0.22621.2428)·Apple 도구 최소 버전(``MetalToolchain``, 32023) 등록. quarantine 제거는 macOS에서만 실제로 돌며 Windows 실행은 함수가 아무것도 하지 않음만 확인(macOS 미검증). CI 저장소 캐시(``foundation.yml``)는 CI가 bootstrap을 쓰는 단계(SH0)에서 더한다.
- [x] 오프라인 도구 빌드: `Engine/Source/ThirdParty/<Lib>/Build/Build.ps1`·`Build.sh`(공용 함수 `Build/ThirdPartyTools/`), 수동 실행 workflow `.github/workflows/third-party-tools.yml`(정책 `PythonFreeMainPath`는 이 파일만 제외). 고정 태그는 DXC v1.9.2609(2026-09-29 최신 안정 릴리스), SPIRV-Cross·SPIRV-Tools·SPIRV-Headers는 그 submodule과 같은 Vulkan SDK 1.4.363.0. 공개 헤더(`dxcapi.h`·`WinAdapter.h`·`spirv_cross_c.h`·`spirv.h`·`libspirv.h`)는 커밋하고 스크립트가 빌드 원본과 byte 대조한다. SPIRV-Tools는 업스트림 공유 빌드가 Windows에서 링크되지 않아 C API(검증·최적화기)를 `SPIRV-Tools-c` 하나로 묶는다(CMake 래퍼 `Build/SpirvToolsC.cmake`).
- [x] Windows x64 archive를 실제로 만들어 manifest `hosts.windows-x64`에 SHA-256을 기록하고 미러 bootstrap으로 검사했다. 빌드한 DXC가 서명된 DXIL(hash 0이 아님)과 SPIR-V를 만든다. `spirv_cross_c.h`·`libspirv.h`는 EngineTests가 경고 0으로 include한다.
- [x] Release 게시(2026-11-17 확인): `third-party-DirectXShaderCompiler-1.9.2609-r1`·`third-party-SpirvCross-1.4.363.0-r1`·`third-party-SpirvTools-1.4.363.0-r1` Release 세 개가 있고, 각 Windows x64 archive의 SHA-256(GitHub asset digest)이 manifest `hosts.windows-x64.archive.sha256`과 같다.
- [ ] macOS arm64 산출물(`Build.sh` 미검증: install name·ad-hoc 서명·dSYM 포함), CI 저장소 캐시. (`dxcapi.h` 경고 0은 P4에서 확인했다, 9절 진척.)
