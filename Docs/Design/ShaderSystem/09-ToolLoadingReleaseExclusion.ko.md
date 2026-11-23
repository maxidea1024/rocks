# 셰이더 시스템 설계 — 9. P4·P10 도구 로드와 Release 배제

[목차](README.ko.md)

## 9. P4·P10 도구 로드와 Release 배제

UE 5.8의 방식은 다음과 같다.
- DXC는 Windows에서 `GetDllHandle`로 전체 경로에서 열고(`DXCWrapper.cpp:69–81`) `dxil.dll`도 명시적으로 연다(`:100`).
- FXC는 동봉본을 전체 경로로 `LoadLibrary`한다(`D3DShaderCompiler.cpp:399–408`).
- ShaderConductor는 import library로 링크하되 DLL을 지연 로드하고 `RuntimeDependencies`로 복사한다. macOS·Linux는 전체 경로를 링크·지연 로드에 함께 넣는다(`ShaderConductor.Build.cs:47–71`, `:59–70`).
- Vulkan 로더는 런타임에 연다(1절).

Rocks는 P10 동적 라이브러리 API로 **모든 도구를 절대 경로에서 연다.** UE가 DXC·FXC에 쓰는 방식을 SPIRV-Cross·SPIRV-Tools에도 넓힌 것이며, 빌드는 도구를 링크하거나 실행 파일 옆에 복사하지 않는다.

- **쓰는 곳.** 도구를 여는 것은 `Programs/ShaderCompileWorker`와 그 안의 `Developer/ShaderFormat*`뿐이다. 메인 프로세스(`Developer/ShaderCompiler`)와 Runtime·게임 모듈은 도구를 열지 않는다.
- **도구 표.** 빌드가 Engine 루트 manifest의 `kind: binary` 항목에서 도구 표(이름·호스트별 파일 이름·SHA-256)를 생성 코드로 만든다(B7 `<out>/BuildInfo`와 같은 생성 방식). 표는 메타데이터만 가진 별도 생성 모듈(`Developer/ShaderToolTable`)이며 파일을 열지 않는다. 메인 프로세스(`Developer/ShaderCompiler`)는 이 표로 job 키를 계산하고(10절 3항), 워커는 이 표와 저장소 경로로 도구를 연다. 두 실행 파일이 같은 manifest에서 만든 표를 가지므로 키와 실제 도구가 어긋나지 않는다.
- **열기.** 워커는 저장소의 파일 SHA-256을 도구 표와 대조한 뒤 연다. 다르거나 없으면 bootstrap을 안내하는 오류로 job을 실패시킨다. `status: system`인 Apple 도구는 예외다: 도구 표에 SHA 없이 최소 버전만 있고, 공유 라이브러리로 열지 않고 P1 `RunProcess`로 실행하며, 지문은 도구 조회로 잰다(11절 3항).
- **API.** DXC `DxcCreateInstance`, FXC `D3DCompile`·`D3DReflect`, SPIRV-Cross `spvc_*`, SPIRV-Tools `spv*`를 함수 표로 얻는다.
- **정책 검사.**
  - 도구 표 모듈(`Developer/ShaderToolTable`)은 `Developer/ShaderCompiler`·워커·ShaderFormat 모듈만 참조한다. binary 파일을 여는 코드(P10 `Open`으로 저장소 경로를 여는 것)는 워커·`Developer/ShaderFormat*`에만 둔다. 표 참조와 바이너리 로드를 따로 검사한다.
  - Release 대상 실행 파일은 모듈 도달 집합에 `Developer` 모듈이 없고 링크 입력·시스템 라이브러리에 `d3dcompiler`·`dxcompiler`가 없어야 한다. SH2에서 D3D12Graphics의 `d3dcompiler` 링크(`D3D12Graphics.Build.cs:21–22`)와 런타임 `D3DCompile`(`D3D12Device.cpp:1122–1146`)을 지운다.
  - Release Windows 실행 파일은 OpenGLGraphics를 링크하지 않는다. `GLES31_PC`는 개발용 미리보기라 쿠킹 데이터를 쓰는 실행 파일에는 두지 않는다(UE와 같다, 2절). Android 실행 파일은 OpenGLGraphics(ES)를 링크한다.
- **런타임 의존 파일 복사.** 도구에는 필요 없다. 결정 3(SM 6.6, 2026-11-17)으로 Windows 10에서는 D3D12 Agility SDK가 Runtime 의존이 되었다. `D3D12Core.dll`을 Runtime 실행 파일 옆에 두고 `D3D12SDKVersion`·`D3D12SDKPath` export를 넣어야 하므로, 그때 Asan 런타임 복사(`ActionGraph.cs:329–337`)를 일반 `RuntimeDependencies` 필드로 넓힌다(UE `RuntimeDependencies` 대응). 이 DLL은 `kind: binary`의 별도 role(`redist`, 실행 파일 옆 복사만 허용)로 두어 위의 도구 로드 정책과 구분한다.
- **설치 SDK.** 워커와 도구 묶음 내보내기는 후속이다(지금 SDK는 C# 도구만 내보내고(`Programs/Rocks.Build.Cli/InstalledSdk.cs:132–135`) `PrebuiltLibraries`·추가 컴파일·링크 인자가 있는 모듈을 거부한다(`:69–74`)).

완료 기준:
- 깨끗한 `out/`과 저장소만으로 워커가 모든 도구를 연다(Windows·macOS·Asan). 파일 SHA가 다르면 열지 않는다.
- 현재 디렉터리·PATH에 둔 같은 이름 DLL을 열지 않는다(P10 음성 대조).
- 정책 음성 대조: Runtime 모듈의 도구 표·binary 참조, 워커 밖의 binary 로드, Release 대상의 `Developer` 모듈 도달, Release Windows 대상의 OpenGLGraphics 링크.
- SH2 뒤 Release Sandbox의 import 표에 컴파일러 DLL이 없다.

진척(2026-11-17, `feature/shader-tools`):
- [x] 도구 표 `Engine/Source/Developer/ShaderToolTable`(`ShaderToolTable/ShaderToolTable.h`). 규칙이 Engine 루트 `kind: binary` manifest에서 생성 TU를 만들고(`ShaderToolTableSource`, Rocks.Tooling.Common) 빌드가 `<out>/GeneratedSources/ShaderToolTable/ShaderToolTable.Generated.cpp`에 써서 컴파일한다(Rocks.Build `NativeModuleInput.GeneratedSources`, BuildInfo와 같은 방식: 출력 잠금 아래에서 내용이 바뀔 때만 쓴다). 내용: 이름·버전(system은 최소 버전)·출처·저장소 디렉터리 이름·빌드 대상 호스트의 파일(경로·SHA-256·크기·role). 호스트는 빌드 대상 하나이며(Android 대상은 파일 없음) 표는 파일을 열지 않는다.
- [x] 열기 계층 `Engine/Source/Developer/ShaderToolLoader`(워커 자리, `ShaderToolLoader/ShaderToolLoader.h`): `GetShaderToolStoreRoot`(bootstrap과 같은 위치), `ShaderToolLibrary::Open`(runtime 파일의 크기·SHA-256 대조 → `DynamicLibrary::Open`, 없으면 `NotBootstrapped`, 다르면 `DigestMismatch`이고 둘 다 bootstrap 명령을 안내), 함수 표 `DxcFunctions`(`DxcCreateInstance`)·`FxcFunctions`(`D3DCompile`·`D3DReflect`)·`SpirvCrossFunctions`(SH0 최소 12개: 버전·문맥·해석·컴파일러·옵션·출력)·`SpirvToolsFunctions`(14개: 버전·문맥·`spvValidateWithOptions`·최적화기·해제), 네 도구 묶음 `ShaderToolSet`(FXC는 이 호스트 파일이 있을 때만), `QueryDxcVersion`(Windows 구현).
- [x] `dxcapi.h`를 Windows 헤더와 함께 include할 때 경고 0(`Private/Win64/ShaderToolLoaderWin64.cpp`). 빌드가 `WIN32_LEAN_AND_MEAN`을 정의하므로 `Unknwn.h`·`objidl.h`를 함께 include해야 한다.
- [x] 정책 검사(`PolicyChecks.ShaderToolReferences`, `ReleaseExclusion`): `shader-tool-table`(도구 표 의존·include는 ShaderCompiler·워커·ShaderFormat\*·ShaderToolLoader·시험 runner만), `shader-tool-binary-load`(ShaderToolLoader·binary 라이브러리 이름 참조·binary 공개 헤더 include·`DynamicLibrary`의 절대 경로 `Open`은 워커·ShaderFormat\*·ShaderToolLoader·시험 runner만, `Open`은 PlatformBase·TestSupport와 모듈 로더의 manifest 적재 `ModuleManifest.cpp`도 허용 — [모듈 로더](../ModuleLoader.ko.md) 3.13절), Release 배제(`release-developer-module`·`release-shader-compiler`·`release-windows-opengl`). "Release 대상"은 `TargetRules.Type == Game`(UE TargetType 대응, 기본 Game, 도구·시험·벤치마크·Commandlet은 Program)인 타깃을 — [빌드 타깃](../BuildTargets.ko.md) BD20 구현 뒤에는 Game·Client·Server 타깃을, Server는 Windows x64·macOS arm64·Linux x64로 — Windows x64·macOS arm64·Android arm64 Release 규칙으로 평가한 도달 집합이다. Developer 모듈은 규칙 폴더가 `<계층>/Source/Developer/`인 모듈이다.
- [x] 알려진 Release 예외 2건(SH2까지): Sandbox가 닿는 D3D12Graphics의 `d3dcompiler` 링크, Sandbox의 Windows OpenGLGraphics 링크. 실패가 아니라 `policy` 출력의 `note: known Release exception until SH2` 줄로 보고하고, 맞는 위반이 없어지면 `release-exception-stale` 실패로 예외를 지우게 한다.
- [x] 음성 대조(ToolTests `Policy.ShaderToolReferenceControls`·`Policy.ReleaseExclusionControls`): Runtime 모듈의 도구 표 의존·include, Runtime 모듈의 binary 라이브러리 참조·`dxcapi.h` include·절대 경로 `Open`, 메인 프로세스(ShaderCompiler)의 로더 참조, 이름만 비슷한 `ShaderFormat` 폴더, Release Game 타깃의 Developer 모듈 도달·d3dcompiler·dxcompiler 링크, Windows Release 대상의 OpenGLGraphics(Android는 허용), 예외의 알림 전환과 낡은 예외.
- [x] 시험(EngineTests): 생성 표 내용(manifest SHA 대조), 인자 오류, 빈 저장소의 bootstrap 안내, bootstrap한 저장소만으로 네 도구 열기와 함수 호출(DXC 버전, `spirv-val` 양성·음성, SPIRV-Cross ESSL 320 출력), SHA-256·크기 불일치와 없는 파일을 열지 않음, 현재 디렉터리·PATH의 같은 이름 라이브러리를 쓰지 않음(자식 probe, P10 음성 대조와 같은 시험 라이브러리). 저장소가 없거나 도구가 bootstrap되지 않았으면 도구를 여는 시험은 `[SKIP]`이며 실패가 아니다(P10과 같은 관례). 기본 빌드·Foundation CI는 저장소에 기대지 않는다(EngineTests는 CI에 없다).
- [x] SH0 도구 스파이크(`Programs/ShaderToolSpike`)를 워커 자리 측정 프로그램으로 정책 허용 위치에 더했다(`shader-tool-table`·`shader-tool-binary-load`, 플랫폼 경계는 `Private/Win64`만 허용). 워커가 생기면 스파이크와 함께 지운다. SPIRV-Cross 함수 표에 `spvc_compiler_build_combined_image_samplers`를 더했다(ESSL에는 분리 texture·sampler가 없다).
- [ ] 런타임 의존 파일 복사: 결정 3(SM 6.6)으로 필요해졌다. 할 일은 Agility SDK `D3D12Core.dll`을 `kind: binary` `redist` 항목으로 등록하고, Asan 런타임 복사를 일반 `RuntimeDependencies`로 넓혀 Runtime 실행 파일 옆에 두며, `D3D12SDKVersion`·`D3D12SDKPath` export를 넣는 것이다. 구현은 P6b 또는 SH2에서 한다(그 전에는 `redist` 파일이 없다).
- [ ] macOS: Mac 저장소 루트 구현은 있으나 macOS 산출물(P3)이 없어 열기 시험은 SKIP이고 CI 미검증이다. `QueryDxcVersion`은 Windows만 구현했다.
