# 셰이더 시스템 설계

기준일: 2026-11-14, 개정 7(초안). 셰이더 원문을 HLSL 하나로 쓰고 오프라인에서 D3D12·D3D11·Vulkan·Metal·OpenGL ES용 코드로 바꾸는 구조와, 그 구현에 앞서 갖춰야 할 **선행 작업**을 정한다. 개정 7은 현재 코드베이스(HEAD `0be1e64`, 2026-11-14)와 다른 설계 문서의 상태를 다시 조사해 선행 작업과 순서를 새로 세웠다(4–6절).

UE 근거는 UE 5.8.4(EpicGames/UnrealEngine `release` 브랜치 commit `f45729c0b6f097038e72b52d366a62baeca3275b`)의 원본이다. 2026-11-14 사용자 지시로 기준을 5.6에서 5.8로 올렸고, 개정 6까지의 5.6 근거를 모두 5.8.4로 다시 확인했다(17절). 경로는 `Engine/Source/` 기준이며 `Engine/Shaders`·`Engine/Config`·`Engine/Build`는 그대로 적는다. UE 밖 도구(DXC·SPIRV-Cross·SPIRV-Tools·Vulkan)에 관한 사실은 그 출처를 따로 적는다. 개정 6까지의 조사 문서(UE 5.6 기준)는 [Rocks 셰이더 시스템](https://claude.ai/artifact/U5uNh2EWGvXKQiNYXnuc1Q)이다.

[엔진 구현 계획](EngineImplementationPlan.ko.md)·[엔진 런타임](EngineRuntime.ko.md)·[에셋 시스템](AssetSystem.ko.md)과 충돌하는 부분은 이 문서 기준으로 바꾸고 그 문서들도 같이 고친다(14절). 개정은 이 문서가 확정될 때 함께 하되, 에셋 시스템 단계(A0-G·A1·A3·A4)에 넣을 셰이더 요구는 그 단계 착수 전에 반영해야 한다(6절).

상태 표기는 AssetSystem과 같다: **구현**(동작·시험), **계약**(API·형식이 있고 미지원 값은 명시 오류), **후속**(API도 없음). 이 문서 전체는 아직 **설계**이며 P1만 구현되었다. 엔진 기반이 준비될 때까지 이 문서는 스펙으로만 두고 다른 선행 작업과 SH 단계는 구현하지 않는다(2026-11-13 사용자 지시). 착수 위치는 6절, 착수 전에 정할 것은 15절이다.

## 1. 결정

| 항목 | 결정 | 근거 |
|---|---|---|
| 셰이더 언어 | HLSL 하나. backend별 원문(현재 `ShaderSource{hlsl, glsl}`, `Engine/Source/Runtime/Graphics/Public/Graphics/DeviceTypes.h:140–143`)을 두지 않는다 | UE도 `.usf`·`.ush` HLSL을 쓰고(공유 디렉터리는 `.h`도 허용) 형식별 ShaderFormat 모듈이 바꾼다(`Runtime/RenderCore/Private/ShaderCore.cpp:1620–1670` `CheckVirtualShaderFilePath`, `.h`·`/ThirdParty/` 예외 `:1646–1659`) |
| 변환 도구 | DXC(HLSL → DXIL·SPIR-V, 1.8.2505 이상), SPIRV-Cross(SPIR-V → MSL·ESSL·D3D11용 HLSL 5.0), SPIRV-Tools(`spirv-val`·최적화 pass, 필수), SPIRV-Reflect, FXC(버전을 고정한 `d3dcompiler_47.dll`), Apple `metal`·`metallib` | UE 5.8도 같은 도구를 쓴다. 다만 UE는 SPIRV-Cross·SPIRV-Tools를 자기 래퍼 ShaderConductor 공유 라이브러리에 정적으로 묶고(`ThirdParty/ShaderConductor/ShaderConductor/Source/Core/CMakeLists.txt:56,82–95`) 두 도구에 UE 수정을 넣었다(7.2절 11항). ShaderConductor 원 저장소는 2023-01 보관되었고 UE 사본·수정은 UE 원본이라 가져올 수 없으므로, Rocks는 업스트림 SPIRV-Cross·SPIRV-Tools의 C API 공유 라이브러리를 직접 부른다. DXC 1.8.2505 이상은 UE(1.8.2403 기반 수정본과 `dxil.dll` 동봉, `ThirdParty/ShaderConductor/ShaderConductor/External/DirectXShaderCompiler/utils/version/latest-release.json`)를 넘어서는 Rocks 결정이다(10절 서명) |
| 도구 공급 | UE처럼 엔진 빌드 밖 스크립트로 도구를 빌드해 바이너리로 배포하고, 엔진은 받은 바이너리를 쓰기만 한다(2026-11-13 사용자 결정 8, 8절) | UE `ThirdParty/ShaderConductor/Build_ShaderConductor_*`, `Engine/Build/Commit.gitdeps.xml` |
| 필수 backend | D3D12, D3D11, Vulkan, Metal, **OpenGL ES** | 2026-11-13 사용자 결정(앞의 넷)에 2026-11-14 사용자 결정 "구형 모바일 때문이라도 opengles는 지원해야함", "언리얼이 다루는 방식으로 맞추면됨"을 더했다. 데스크톱 OpenGL SM5(GL 4.5 출시 대상)는 제외한다: UE 5.8에도 데스크톱 GL SM5 형식이 없다(`GLSL_430_REMOVED`, `Developer/ShaderFormatOpenGL/Public/ShaderFormatOpenGL.h:30`). OpenGL ES는 UE처럼 Android 출시 형식과 Windows 개발용 미리보기 형식 둘이다(2절) |
| 두 번째 검증 backend | Vulkan. VulkanGraphics 최소 구현을 SH2에 넣는다. OpenGLGraphics는 지우지 않고 SH2에서 GLES 미리보기 형식을 받는 backend로 바꾼다(결정 9 개정) | SH2가 backend별 원문을 없애므로 OpenGLGraphics도 쿠킹된 코드를 받아야 한다. GLES 필수 결정으로 OpenGLGraphics는 UE `OpenGLDrv`처럼 Windows 미리보기와 Android 실행을 맡는다(P11) |
| 런타임 | 쿠킹된 코드만 받는다. Release(UE Shipping에 해당) 실행 파일에 셰이더 컴파일러를 넣지 않는다(9절 정책 검사) | UE RHI 셰이더 생성은 코드 바이트를 받는다(`Runtime/RHI/Public/DynamicRHI.h:247` `RHICreatePixelShader(const FRHICreateShaderDesc&)`, 코드·소유자·라이브러리 `Runtime/RHI/Public/RHIResources.h:789–813`) |
| 컴파일 실행 | 메인 프로세스가 입력 수집·**전처리**·키 계산을 하고, 별도 ShaderCompileWorker 프로세스가 컴파일한다(11절). 메인 프로세스는 도구 바이너리와 ShaderFormat 모듈을 쓰지 않는다 | UE는 전처리를 job 제출 때 메인 프로세스에서 하고 워커에서는 하지 않는다(`Runtime/RenderCore/Public/ShaderCompilerCore.h:645`, `Runtime/Engine/Private/ShaderCompiler/ShaderCompilerJobCache.cpp:1544–1545`). 워커는 `Programs/ShaderCompileWorker`. 전처리기 구현은 15절 결정 6 |
| 도구 로드 | 워커가 도구 공유 라이브러리를 사용자 단위 저장소에서 **절대 경로로 연다**(P10 동적 라이브러리 API, 9절). Vulkan 로더도 런타임에 연다(P9) | UE도 Windows에서는 DXC·FXC를 전체 경로로 연다(`Developer/ShaderCompilerCommon/Private/DXCWrapper.cpp:69–81`, `Developer/Windows/ShaderFormatD3D/Private/D3DShaderCompiler.cpp:399–408`. macOS의 DXC는 링크와 지연 로드, `ThirdParty/ShaderConductor/ShaderConductor.Build.cs:59–70`). Vulkan 로더는 `LoadLibraryW("vulkan-1.dll")`(`Runtime/VulkanRHI/Private/Windows/VulkanWindowsPlatform.cpp:66`)·`dlopen("libvulkan.so")`(`Runtime/VulkanRHI/Private/Android/VulkanAndroidPlatform.cpp:314`)로 연다. 로더를 링크하면 Vulkan 런타임이 없는 기계에서 실행 파일이 시작하지 못한다 |
| 캐시 | 에셋 너겟(AssetSystem 9절), 공통 키 규칙 그대로(2026-11-13 사용자 결정 10, 10절) | UE의 job 캐시·DDC 역할이다 |

이 문서가 정한 규칙: 바인딩·상수 버퍼·단계 사이 변수(7절), ThirdParty 종류·오프라인 도구 빌드(8절), 도구 로드·Release 배제(9절), 셰이더 job 키(10절), 워커 통신(11절). 사용자가 정한 것: 확장자 `.rsf`·`.rsh`(결정 1), 도구 공급 UE 방식(결정 8), SH2 범위(결정 9, 2026-11-14 GLES 결정에 따라 개정), job 키 공통 규칙(결정 10), OpenGL ES 필수와 UE 방식(2026-11-14). 남은 사용자 결정(15절): HLSL 2021 범위, D3D12 셰이더 모델, 파라미터 선언, 생산 호스트, 전처리기, 경고 정책(오프라인 도구 빌드·셰이더 경고), 셰이더 구현 착수 위치.

## 2. 형식과 변환 경로

| 형식 id | backend | 경로 | UE 5.8 대응 |
|---|---|---|---|
| `D3D12_SM6` | D3D12 | DXC → DXIL(DXC 내장 validator 서명, 10절) | `PCD3D_SM6`: SM 6.6, 워크 그래프 6.8(`Developer/Windows/ShaderFormatD3D/Private/ShaderFormatD3D.cpp:102–110`). `PreferredShaderCodeFormat`이 DXIL이면 SM 6.0(`:120`) |
| `D3D11_SM5` | D3D11 | FXC → DXBC SM 5.0. HLSL 2021 셰이더와 FXC 1차 실패 셰이더는 DXC → SPIR-V → SPIRV-Cross HLSL 5.0 → FXC | `PCD3D_SM5`. DXC 선행 경로(`D3DShaderCompiler.cpp:802–995`, 조건 `:806–807`·`:840–841`) |
| `VULKAN_SM5` | Vulkan | DXC `-spirv`(target env `vulkan1.1`) → binding·location 재배치 → `spirv-val` | `SF_VULKAN_SM5`(Vulkan 1.1, `Developer/VulkanShaderFormat/Private/VulkanShaderCompiler.cpp:96–99`)의 비-bindless 경로. `SF_VULKAN_SM6`은 1.3(`:88–91`)·scalar layout·bindless(`Engine/Config/Windows/BaseWindowsEngine.ini:57–63`) |
| `METAL_SM5` | Metal | DXC `-spirv` → 재배치 → `spirv-val` → SPIRV-Cross MSL → `metal`·`metallib` | `SF_METAL_SM5`. bindless면 DXIL → Metal Shader Converter(`Developer/Apple/MetalShaderFormat/Private/MetalShaderCompiler.cpp:1113–1120`, Rocks는 후속) |
| `GLES31_ANDROID` | OpenGL ES | DXC `-spirv`(target env `vulkan1.1`) → SPIRV-Tools 최적화 → 재배치 → `spirv-val` → SPIRV-Cross ESSL `#version 320 es` → 상수 버퍼 에뮬레이션 재작성(7.2절 4항) | `GLSL_ES3_1_ANDROID`: ES3.1 feature level, ESSL 320(`Developer/ShaderFormatOpenGL/Private/OpenGLShaderCompiler.cpp:2883–2908`), target env 기본 1.1(`Developer/ShaderCompilerCommon/Public/ShaderConductorContext.h:118`). ES 3.2 기기 필수(`Runtime/OpenGLDrv/Private/Android/AndroidOpenGL.cpp:453–485`. ES 지원을 켜고 패키징하면 manifest `glEsVersion=0x00030002`, `Programs/UnrealBuildTool/Platform/Android/AndroidToolChain.cs:279–284`, 기록 `UEDeployAndroid.cs:3049`) |
| `GLES31_PC` | OpenGL ES(Windows 미리보기) | `GLES31_ANDROID`와 같고 클립 공간 보정만 다르다(7.2절 8항). 개발용이며 출시 프로필에 넣지 않는다 | `GLSL_150_ES31`(`SP_OPENGL_PCES3_1`): Windows GL 4.3 core(`Runtime/OpenGLDrv/Private/Windows/WindowsOpenGLPlatform.cpp:200–205`), 쿠킹 데이터를 쓰는 실행 파일에는 등록하지 않는다(`Runtime/RHI/Private/Windows/WindowsDynamicRHI.cpp:628–633`) |
| `VULKAN_ANDROID`·`METAL_IOS` | 모바일 | 자리만 둔다(Unsupported). `VULKAN_ANDROID`는 SH6 | `SF_VULKAN_ES31_ANDROID`(`Runtime/RHI/Public/RHIShaderFormatDefinitions.inl:35`), `SF_METAL_SM5_IOS`·`SF_METAL_SM6_IOS`(`:25–27`) |

GLES 기기 요구는 UE와 같다: OpenGL ES 3.2 기기만 실행하고 ES 3.0·3.1만 지원하는 기기는 받지 않는다. GLES 형식의 셰이더 단계는 VS·PS·CS다. UE GL 컴파일러는 VS·PS·GS·CS를 받지만 ES3.1 feature level은 GS를 빼고 테셀레이션은 받지 않는다(`OpenGLShaderCompiler.cpp:2998–3004`, `Runtime/RHI/Public/DataDrivenShaderPlatformInfo.h:1087–1089`).

장치 요구 수준이 다르면(예: Vulkan 최소 보장보다 큰 descriptor 한도) 다른 형식 id로 둔다. 상한과 기능 요구는 형식 정의가 가진다(12절).

SM 6 기능(웨이브 연산 등)이 필요한 셰이더: UE는 이런 셰이더(`ShaderFormatD3D.cpp:113–122`)를 DXIL로 컴파일하고 `EShaderOptionalDataKey::ShaderModel6`을 붙이며(`D3DShaderCompilerDXC.cpp:1195–1199`), D3D11 RHI는 이 표시가 있는 셰이더를 만들지 않는다(`Runtime/Windows/D3D11RHI/Private/D3D11Shaders.cpp:54–66`). Rocks는 셰이더 타입이 요구 기능을 선언하고, 형식 정의의 지원 기능 목록에 없으면 그 형식에서 쿠킹하지 않는다(12절). 런타임에 없는 셰이더를 찾으면 `IncompatibleCook`이다.

## 3. 구조 요약

```text
Engine/Shaders/{Public,Private,Shared}             .rsf(엔트리)·.rsh(include)·.h(C++·HLSL 공용)
Engine/Config/ShaderFormats/<id>.json              형식 정의(12절)
Engine/Config/TargetProfiles/<id>.json             graphicsBackends·shaderFormats(12절, 파일은 AssetSystem 10절)
Engine/Source/ThirdParty/<Lib>/                    DXC·SPIRV-Cross·SPIRV-Tools·FXC(binary manifest·오프라인 빌드 스크립트), SPIRV-Reflect(C 원본), Vulkan-Headers(headers)(8절)
Engine/Source/Runtime/ShaderCore                   셰이더 타입·퍼뮤테이션 ID·파라미터 메타데이터·셰이더 전용 정렬 타입·라이브러리 로드
Engine/Source/Developer/ShaderCompiler             ShaderCompileManager·전처리기·키·에셋 너겟·워커 통신(메인 프로세스)
Engine/Source/Developer/ShaderToolTable            ThirdParty manifest에서 생성한 도구 표(이름·SHA-256, 메타데이터만, 9절)
Engine/Source/Developer/ShaderFormat{D3D,Vulkan,Metal,OpenGL}   워커 안의 형식별 컴파일·재배치·리플렉션·GLES 재작성
Programs/ShaderCompileWorker                       워커 프로세스
```

ShaderCore만 Runtime 계층에 두고(런타임이 쿠킹된 라이브러리를 연다), 컴파일러 쪽은 Developer·Programs에 둔다.

가상 경로는 `/Engine/{Public,Private,Shared,Generated}`·`/Plugin/<이름>`·`/Project`이고, 규칙은 UE `CheckVirtualShaderFilePath`(`Runtime/RenderCore/Private/ShaderCore.cpp:1620–1670`)를 따르되 확장자는 `.rsf`·`.rsh`, 공유 디렉터리만 `.h`다(UE의 `/ThirdParty/` 예외는 두지 않는다). 키는 두 층이다: 셰이더 맵 키(엔트리 파일과 include 폐포의 내용 SHA-256, 낡음 판정)와 job 키(10절, 너겟 키). UE는 원본 파일 해시에 SHA-1(`ShaderCore.cpp:3282–3409`), 셰이더 맵 ID와 코드 해시에 XxHash64(`ShaderCore.cpp:1536–1551`, `Runtime/Engine/Private/Materials/MaterialShader.cpp:1341–1344`), job 키에 BLAKE3(`ShaderCore.cpp:4612–4671`)를 쓴다. Rocks는 모두 Core SHA-256이다.

## 4. 현재 코드베이스(2026-11-14)

HEAD `0be1e64` 기준이다. 셰이더 시스템이 쓸 기반과 바꿀 대상, 없는 것을 모은다.

| 영역 | 있는 것 | 셰이더 시스템에 부족한 것 → 선행 작업 |
|---|---|---|
| Graphics API | `Backend{D3D12, OpenGL, Vulkan, Metal}`(Vulkan·Metal은 Unsupported, `DeviceTypes.h:14–21`), `ShaderSource{hlsl, glsl}`(`:140–143`), 파이프라인 생성이 컴파일까지 한다. 바인딩은 고정(상수 b0, 텍스처 t0·s0, `:145–148`), `DrawConstants` 80 byte(`float transform[16]`·`float color[4]`, `:161–164`) | 컴파일된 코드를 받는 API·파이프라인 layout → SH2. D3D11·Vulkan·Metal backend와 OpenGL의 GLES 전환 → P9·P11·P12·SH2·SH5 |
| D3D12 | root signature 하나(32-bit 상수 20개 b0, SRV·샘플러 테이블 각 1개는 PS만, `Engine/Source/Runtime/D3D12Graphics/Private/Win64/D3D12Device.cpp:1083–1120`), 런타임 `D3DCompile` `vs_5_1`·`ps_5_1`(`:1122–1146`), 모든 구성에서 `d3dcompiler` 링크(`D3D12Graphics.Build.cs:21–22`), 입력 시맨틱 `TEXCOORD`+location(`:703–704`), 디버그 레이어(`:323–330`) | 파이프라인별 root signature(7.2절 4항) → SH2, `d3dcompiler` 제거 → P4·SH2 |
| OpenGL | Windows WGL 4.5 core(`Engine/Source/Runtime/OpenGLGraphics/Private/Win64/OpenGLDevice.cpp:394–422`), GL 함수 선언을 직접 두고 `wglGetProcAddress`로 얻음(`:144–146`), 런타임 GLSL 450 컴파일(`:921–935`), uniform 이름 조회(`:653–654`), DSA(`:574–585`·`:996`), `glClipControl`(`:888–893`) | GLES 3.2 공통 API 부분집합과 쿠킹된 ESSL → P11·SH2 |
| Renderer | 내장 셰이더를 C++ 문자열로 가진다(HLSL `Engine/Source/Runtime/Renderer/Private/Renderer.cpp:14–46`, GLSL `:48–71`), 파이프라인 하나(텍스처×정점색×항목색) | `.rsf` 원문과 쿠킹된 라이브러리 → SH1·SH2 |
| Sandbox | self-test가 D3D12 하드웨어·WARP·OpenGL을 대조(허용 오차 채널 ±8·2%, `Programs/Sandbox/Private/GraphicsSelfTest.cpp:409–415`, 대상 `:427–438`). `--graphics=` 인자는 없다 | Vulkan·GLES 미리보기 대조 → SH2 |
| GraphicsTypes | RGBA8 2종, 2D, 필터 4종(`PixelFormat.h:12–16`, `Sampler.h:11–20`) | A0-G(다른 문서, 5.2절 요구) |
| 수학 타입 | `Matrix4`·`Vector3`는 정렬 지정 없음(`Core/Source/Runtime/Core/Public/Core/Math/Matrix4.h:56–61`, `Vector3.h:19–24`) | 셰이더 전용 정렬 타입(7.2절 5항) → SH2 |
| 플랫폼 | `Platform/Process.h`(P1 구현), `Platform/DirectoryWatch.h:88`(A0-P 구현). 동적 라이브러리 API 없음(PlatformLayer P1 표 "동적 모듈" 미구현). PlatformWindow는 Win32만이고 다른 플랫폼은 Unsupported(`PlatformWindow.Build.cs`) | P10, P12, P13 |
| Core | SHA-256(`Core/Encoding/Sha256.h:23,40`), JSON(`Core/Text/Json.h`), 결정적 수학 버전 `rocks::deterministic::Version`(`DeterministicMath/DeterministicMath.h:31`), 툴체인 지문·도구 빌드 ID(`Core/Build/BuildInfo.h:24,30`), Archive·Record·RecordList, `WorkerScheduler`, `CommandletRegistry` | 그대로 쓴다(5.2절) |
| 리플렉션 | A0-B B1–B8 구현(`ROCKS_STRUCT`·identity·배열·지정자·World 타입, `e405c37`) | 셰이더 파라미터 종류 → P7 |
| ThirdParty | 루트 `Core/Source/ThirdParty` 하나(`Programs/Rocks.Tooling.Common/ThirdPartyManifest.cs:76`), `kind`는 `code`·`data`(`:28`), `code`는 `.c` 번역 단위만(`:357–364`), 미리 빌드된 바이너리 필드 없음(`:132–136`, 모르는 필드는 오류 `:45`) | P3 |
| 빌드 | 모듈 종류는 StaticLibrary·Executable(`Programs/Rocks.Build/NativeBuildModels.cs:38`), 언어는 C·C++(`:57`), `PrebuiltLibraries`(`:147`), 런타임 파일 복사는 Asan DLL만(`ActionGraph.cs:329–337`), rpath 처리 없음, 생성 코드 `<out>/Generated/<Module>`·`<out>/BuildInfo` | P4·P12 |
| CI | `.github/workflows/foundation.yml` 하나(Windows·macOS), 캐시 단계 없음 | P3(저장소 캐시), P9(Vulkan 장치) |
| 에셋 | A0-O·A0-G·A1–A6 미착수(AssetRuntime·AssetNugget 모듈 없음), `ProjectLayout`은 이름만(AssetSystem 1절 A2) | A1–A4 → P5·P6b |
| 셰이더 원문·설정 | `Engine/Shaders`·`Engine/Config` 없음, 셰이더 파일 없음 | SH1·P6a·P14 |

다른 설계 문서 상태: A0-P·A0-C·A0-B는 구현되었고 A0-O·A0-G·A1–A6은 착수 전이다. "화면 마일스톤"은 AssetSystem 12.2·12.4.1절에 이름만 있고 범위가 정의되어 있지 않다.

## 5. 선행 작업

### 5.1 목록

| ID | 작업 | 담당 문서 | 상태 | 막는 단계 | 의존 | 상세 |
|---|---|---|---|---|---|---|
| A0-C | SHA-256·JSON·결정적 수학 버전 | CoreDataFoundations | **구현** | — | — | 5.2 |
| A0-P | 파일 감시·원자적 교체·no-replace 이동·시각 설정 | AssetSystem A0-P | **구현**(macOS 미검증) | — | — | 5.2 |
| B1–B8 | 리플렉션(`ROCKS_STRUCT`·identity·배열), B7 툴체인 지문 | ReflectionFoundations | **구현** | — | — | 5.2 |
| A0-G | 형식·차원·swizzle·샘플러 확장 | AssetSystem A0-G | 미착수 | P11, SH2 | — | 5.2 G1–G3 |
| A1 | AssetRuntime, `UnsupportedCapability`, 매니페스트 reader | AssetSystem A1 | 미착수 | P6b | A0-O | 5.2 |
| A2 | `ProjectLayout` | AssetSystem A2 | 미착수 | P5, SH1 | A1 | 5.2 |
| A3 | 에셋 너겟 | AssetSystem A3 | 미착수 | P5 | A2 | 5.2 N1–N3 |
| A4 | Cooker·TargetProfile·쿠킹 매니페스트 | AssetSystem A4 | 미착수 | P6b | A3 | 5.2 K1–K3 |
| P1 | 자식 프로세스·pipe | PlatformLayer | **구현** | SH1 | — | 5.3 |
| P2 | 바인딩 규약(GLES 포함) | 이 문서 | 설계 | SH2 | — | 7절 |
| P3 | ThirdParty Engine 루트·`binary`·`headers` 종류·저장소·오프라인 도구 빌드 | 이 문서 | 설계 | P4, P9, P12, SH0 | — | 8절 |
| P4 | 도구 표 생성·binary 의존 정책·Release 배제 검사 | 이 문서 | 설계 | SH0, SH2 | P3, P10 | 9절 |
| P5 | 셰이더 job 키 | 이 문서 | 설계 | SH1 | A2, A3, P6a | 10절 |
| P6a | 형식 정의 스키마와 파일, 엔진 컴파일 프로필 | 이 문서 | 설계 | P5, P14, P6b, SH1 | A0-C(JSON), 결정 2·3·5 | 12.1절 |
| P6b | TargetProfile 필드·매니페스트 v2·엔진 내장 셰이더 라이브러리 | 이 문서 | 설계 | SH2 | A1, A4, P6a, P14 | 12.3절 |
| P7 | MetaCompiler 셰이더 파라미터 | 이 문서 | 설계 | SH3 | B1–B8 | 13절 |
| P8 | 다른 문서 개정 | 이 문서 | 목록 | 모든 단계의 기준 | 결정별 기한(15절) | 14절 |
| P9 | Vulkan 선행 | 이 문서·EngineRuntime | 설계 | SH2 | P3, P10 | 5.3 |
| P10 | 동적 라이브러리 API | PlatformLayer | 미착수 | P4, P9, P13, SH0, SH2 | — | 5.3 |
| P11 | OpenGL ES 선행(OpenGLGraphics를 UE `OpenGLDrv` 방식으로) | EngineRuntime·이 문서 | 설계 | P13, SH2, SH6 | A0-G | 5.3 |
| P12 | macOS 실행 기반(Objective-C++ 빌드·Cocoa 창·Metal layer) | PlatformLayer·Rocks.Build | 미설계 | P15, SH5 | P3 | 5.3 |
| P13 | Android 실행 기반(앱·창·EGL surface·수명·APK) | PlatformLayer | 미설계 | SH6 | P10, P11, AssetSystem APK 읽기(VFS) | 5.3 |
| P14 | 셰이더 타입·퍼뮤테이션·라이브러리 형식 | 이 문서 | 설계 | SH2, P6b | P6a | 12절 |
| P15 | D3D11Graphics·MetalGraphics 설계 | EngineRuntime | 미설계 | SH5 | P12(Metal) | 5.3 |

### 5.2 다른 문서가 맡는 선행 작업과 셰이더 요구

**구현된 것.** 셰이더 시스템은 아래를 그대로 쓰고 같은 기능을 따로 만들지 않는다.

- A0-C: SHA-256(`Core/Source/Runtime/Core/Public/Core/Encoding/Sha256.h:23,40`), JSON(`Core/Text/Json.h`, 결정적 출력), 결정적 수학 버전 `rocks::deterministic::Version`(`DeterministicMath/DeterministicMath.h:31`, 결과 비트가 바뀌면 올리며 너겟 키·cookKey에 들어간다).
- B7: 툴체인 지문·도구 빌드 ID `rocks::build::ToolchainFingerprint()`·`ToolBuildId()`(`Core/Build/BuildInfo.h:24,30`). 정규 텍스트는 컴파일러 버전·컴파일러와 링커 SHA-256·대상 triple·FP 모델이다(ReflectionFoundations 8.1절).
- B4·B5: `ROCKS_STRUCT`(struct 스키마는 클래스와 같은 `TypeSchema`, ReflectionFoundations 4.2절)·identity 명세(`<Module>.rocksids.json`)·배열 필드. P7의 바탕이다.
- A0-P: `Platform/DirectoryWatch.h:88`(SH4), `File::Flush`·`FileSystem::Replace`·no-replace `Move`·`SetModifiedTime`(너겟 기록·GC, 워커 출력).
- Core 직렬화(Archive·Record·RecordList, 워커 메시지), `WorkerScheduler`(워커 묶음 배분), `CommandletRegistry`(`shader-compile` 명령).

**미착수 단계에 넣을 셰이더 요구.** 그 단계를 설계·구현할 때 함께 해야 하므로 AssetSystem에 먼저 반영한다(14절, 6절 2항).

- **A0-G**
  - G1. 새 형식·차원·swizzle·샘플러 타입은 backend 중립으로 정의한다. D3D12 `Shader4ComponentMapping`, Vulkan `VkComponentMapping`, Metal `MTLTextureSwizzleChannels`, GLES `TEXTURE_SWIZZLE_R/G/B/A`로 옮길 수 있어야 한다. backend별 형식·필터 지원 표를 두고, 지원하지 않는 조합은 `UnsupportedCapability`다.
  - G2. A0-G의 OpenGL 구현은 OpenGL ES 3.2 core와 공통인 API를 기본으로 하고, 데스크톱 4.5 전용 호출(DSA `glTextureParameter*`·`glBindTextureUnit` 등)을 새로 늘리지 않는다. OpenGLGraphics는 SH2에서 GLES 미리보기, SH6에서 Android GLES로 이어지기 때문이다(P11). ES 3.2 core에 없는 A0-G 항목은 확장을 조건으로 한다: R16·RG16·RGBA16 UNORM(`GL_EXT_texture_norm16`), BGRA8(`GL_EXT_texture_format_BGRA8888`), 이방성 필터(`GL_EXT_texture_filter_anisotropic`), 32-bit float 선형 필터(`GL_OES_texture_float_linear`). 확장이 없으면 `UnsupportedCapability`로 실패하거나 Android 프로필 형식 표에서 뺀다. swizzle은 채널별 `TEXTURE_SWIZZLE_R/G/B/A`로 설정한다(`TEXTURE_SWIZZLE_RGBA`는 ES에 없다).
  - G3. SH2의 VulkanGraphics는 A0-G 범위(형식·차원·swizzle·샘플러)를 받아야 완료다.
- **A1**: Core `ErrorCode::UnsupportedCapability`(AssetSystem 1절 A1에서 추가)를 만들 수 없는 셰이더 형식에도 쓴다. 쿠킹 매니페스트 reader는 v2의 "에셋이 아닌 산출물" 항목을 읽는다(P6b).
- **A2**: `ProjectLayout`이 셰이더가 쓰는 위치를 준다. 엔진·프로젝트·플러그인의 `Shaders` 디렉터리(3절 가상 경로), 너겟 위치, 워커 작업 디렉터리 `<Project>/Intermediate/ShaderWorkers/`다. 프로젝트 없이 엔진만 쓸 때(Sandbox·엔진 내장 라이브러리)는 엔진 루트 아래 같은 이름을 쓴다. `ProjectLayout`은 AssetSystem에 이름만 있고 정의가 없다(1절 A2, 4.2절).
- **A3**
  - N1. 에셋이 아닌 처리기도 공통 키 항목을 쓰게 한다. AssetSystem 9절 키 항목(번호는 이 문서 10절 표가 붙인 것) 가운데 1–5·8–9항(형식 버전, 처리기 Guid·버전 Guid, 외부 도구, 결정적 수학 버전, 툴체인 지문, 설정, 프로파일)은 공통 머리로 쓰고, 에셋 전용인 6·7·10항(TypeId·입력 bulk·의존 목록)은 처리기가 자기 입력으로 바꿔 쓸 수 있게 한다. 에셋이 아닌 처리기는 AssetId·Build 의존이 없으므로 SCC 식(구성원 key = SHA-256(SCC key, AssetId))을 쓰지 않고 키 = SHA-256(`BuildKey` 바이트)로 한다. 셰이더의 대응은 10절 표다.
  - N2. 값은 여러 부분(`.rnug` 부분 구조)이다: 바이트코드·정규화 바인딩 layout·GLES 복사 표·진단·디버그 심볼.
  - N3. Rockpile이 요구한 AssetSystem 9절 개정(`Rockpile.ko.md` 17절: 키의 에셋 너겟 세대 Guid 11.4절, `.rnug` v2 5.2절, 일괄·비동기 API 12.2절)이 A3에 반영되면 셰이더 키·값도 그대로 따른다. 셰이더 bucket은 `shader`다(`Rockpile.ko.md` 5.1절).
- **A4**
  - K1. TargetProfile에 `graphicsBackends`(복수, 순서 = 런타임 선택 순서)·`shaderFormats`(backend마다 선호 순서가 있는 형식 목록) 필드와 엔진 기본 `MacOS` 프로필을 둔다(12절). 지금 AssetSystem 10절은 "그래픽 backend"를 이름 없는 단일 항목으로 적고 기본 프로필은 Windows·Android뿐이다.
  - K2. 쿠킹 매니페스트 v2에 에셋이 아닌 산출물 항목을 둔다(12절). v1 항목은 AssetId 단위다(AssetSystem 10절).
  - K3. 엔진 내장 셰이더 라이브러리를 프로젝트·너겟·네트워크 없이 제공하는 경로를 둔다. 계획 9.6절이 "에셋 너겟·네트워크·프로젝트 없는 설치에서도 최소 자원을 제공한다"고 요구한다. 라이브러리를 만들 때 쓰는 엔진 컴파일 프로필은 P6a가 정하고(12.1절), A4는 설치물에 라이브러리를 두고 여는 경로만 맡는다(12.3절).

### 5.3 셰이더 전용 선행 작업

규칙이 긴 작업(P2–P7)은 해당 절에 상세를 두고 여기에는 요약만 둔다.

**P1 자식 프로세스·pipe — 구현(2026-11-13).**
- `Platform/Process.h`: 시작·대기·종료 코드·자손까지 강제 종료(Windows job object, macOS 프로세스 그룹)·시간 제한 pipe·`RunProcess`·`CurrentProcessId`·`IsProcessRunning`·입력 pipe EOF lifeline([플랫폼 계층](PlatformLayer.ko.md) "자식 프로세스와 pipe").
- 검증: Windows Debug·Asan, macOS CI(`b1d316f`), Android 에뮬레이터(Unsupported 경로).
- 남은 제약: Android `Start`는 Unsupported다(워커는 개발 호스트에서만 돈다). 환경 변수 지정이 없어 부모 환경을 물려준다(필요해지면 PlatformLayer 후속).

**P2 바인딩 규약 — 설계(7절).**
- 산출물: 7.2절 규칙, `ShaderBindingLayout` 형식, GLES 상수 버퍼 에뮬레이션 규약.
- 완료 기준: 측정에 기대지 않는 규칙이 확정되고 7.3절 시험이 SH2·SH3·SH5·SH6 완료 기준에 들어 있다. 행렬 옵션(7.2절 6항)과 backend별 앞면 매핑(8항)은 측정 S5로 정하는 SH2 산출물이다.

**P3 ThirdParty 확장과 오프라인 도구 빌드 — 설계(8절).**
- 산출물: Engine 루트, `kind: binary`·`kind: headers`, 저장소에 커밋하는 도구 공개 헤더, 사용자 단위 내용 주소 저장소와 bootstrap 명령, 라이브러리별 오프라인 빌드 스크립트와 수동 CI workflow, FXC 고정본, Apple 도구 최소 버전, SPIRV-Reflect(C 원본)·Vulkan-Headers(headers).
- 막는 것: SH0.

**P4 도구 표·binary 의존 정책·Release 배제 — 설계(9절).**
- 산출물: manifest에서 생성한 도구 표 모듈(메인 프로세스 키 계산·워커 로드 공용), binary 로드 정책, Release 배제 정책 검사.
- 의존: P3, P10. 막는 것: SH0(도구 표), SH2(Release 배제 판정).

**P5 셰이더 job 키 — 설계(10절).** 의존: A2, A3(N1–N3), P6a, A0-C·B7(구현). 막는 것: SH1.

**P6a 형식 정의와 엔진 컴파일 프로필 — 설계(12.1절).** 형식 정의 스키마와 `Engine/Config/ShaderFormats/<id>.json` 파일, 호스트별 엔진 컴파일 프로필(`Engine-<호스트>.json`)이다. job 키 3·9항이 형식 정의 SHA-256과 프로필을 쓰고 워커가 target env·ESSL 버전·상한을 읽으므로 P5·SH1 앞이다. 의존: A0-C JSON(구현), 결정 2·3·5.

**P6b TargetProfile 필드·매니페스트 v2·엔진 내장 라이브러리 — 설계(12.3절).** 의존: A1, A4(K1–K3), P6a, P14. 막는 것: SH2(런타임이 쿠킹된 라이브러리를 연다).

**P7 MetaCompiler 셰이더 파라미터 — 설계(13절, 15절 결정 4를 MetaCompiler 방식으로 고를 때).** 의존: B1–B8(구현). 막는 것: SH3. P7 전에는 손으로 쓴 레이아웃 표를 쓴다(7.2절 5항).

**P8 다른 문서 개정 — 목록(14절).** AssetSystem 쪽(5.2절 요구)은 해당 단계(A0-G·A1·A2·A3·A4) 착수 전에, 나머지는 15절의 결정별 기한에 맞춰 한다. 측정 뒤에 정하는 결정(3·5·6)에 걸린 행은 그 결정 뒤에 고친다.

**P9 Vulkan 선행 — 설계.**
- Vulkan-Headers: `kind: headers`(P3)로 Engine 루트에 둔다.
- 로더: import library를 링크하지 않고 런타임에 연다(P10, UE와 같다, 1절). Windows `vulkan-1.dll`은 드라이버·Vulkan 런타임이 설치한다. 없으면 VulkanGraphics 생성이 `UnsupportedCapability`로 실패하고 다른 backend는 그대로 동작한다.
- 검증 레이어: D3D12 디버그 레이어에 대응하며 같은 조건(EngineRuntime 10.1, 장치 생성의 `enableValidation`)에서 켠다. 공급(Khronos Vulkan-ValidationLayers 릴리스를 binary 항목으로 받거나 오프라인 빌드)은 측정 S4로 정한다.
- surface: Windows `VK_KHR_win32_surface`에 PlatformWindow의 네이티브 창 핸들을 쓴다. Android surface는 P13이다.
- VulkanGraphics 설계: EngineRuntime에 절을 더한다(디바이스·큐·메모리·파이프라인 layout(7.2절 4항)·검증 오류를 `ValidationErrorCount`로 집계).
- CI: GPU 없는 runner의 소프트웨어 ICD(lavapipe·SwiftShader)는 측정 S4다. 안 되면 CI에서 **NOT RUN**으로 보고하고 로컬 결과를 기록한다.
- macOS는 Metal을 쓰므로 Vulkan(MoltenVK 등)은 범위 밖이다.
- 완료 기준: Vulkan-Headers가 Rocks 경고 규칙으로 include되고, 로더 동적 로드와 검증 레이어 켜기가 Windows에서 시험되며, VulkanGraphics 설계 절이 확정된다.

**P10 동적 라이브러리 API — 미착수(PlatformLayer P1 표의 "동적 모듈" 항목).**
- 현재: 공개 로더가 없고 private 코드가 OS 함수를 직접 부른다(XInput, `Engine/Source/Runtime/PlatformWindow/Private/Win64/Win64Window.cpp:553`). 계획 §2.5의 "동적 loader를 Foundation 선행 조건으로 만들지 않는다"는 native plugin 모듈 이야기다. 이 API는 Foundation이 아니라 셰이더 워커와 VulkanGraphics의 선행 조건이고, 엔진 모듈을 동적으로 싣는 로더가 아니다.
- 산출물 `Platform/DynamicLibrary.h`:
  - `Open(absolutePath)`: 절대 경로만 받는다. Windows `LoadLibraryExW`(`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`, 현재 디렉터리·PATH를 찾지 않고 같은 디렉터리의 의존 DLL은 찾는다), macOS·Android `dlopen(RTLD_NOW | RTLD_LOCAL)`.
  - `OpenSystem(name)`: OS가 공급하는 라이브러리만 이름으로 연다(Windows System32의 `vulkan-1.dll`·XInput, Android `libvulkan.so`).
  - `Find(symbol)`, 수명, 오류 변환(파일 없음 `NotFound`, 심볼 없음, 잘못된 경로 `InvalidArgument`). 수명은 PlatformLayer "동적 모듈" 검증 기준("참조 중 unload 거부")을 따른다: 얻은 심볼 표를 lease로 세고, lease가 남아 있으면 `Close`가 `Busy`로 거부한다.
- 완료 기준: 파일 없음·심볼 없음 오류, 현재 디렉터리에 둔 같은 이름 DLL을 열지 않는 음성 대조, 같은 디렉터리 의존 DLL 해결, Windows Debug·Asan·macOS CI·Android 에뮬레이터 시험. XInput 직접 로드를 이 API로 옮긴다.
- 막는 것: SH0(워커 도구 로드), SH2(Vulkan 로더).

**P11 OpenGL ES 선행 — 설계.** OpenGLGraphics를 UE `OpenGLDrv`처럼 Windows 미리보기와 Android ES를 함께 맡는 backend로 바꾸는 설계다.
- 현재: 4절 OpenGL 행(WGL 4.5 core, 런타임 GLSL 450, uniform 이름 조회, DSA).
- 공통 코드는 OpenGL ES 3.2와 데스크톱 GL 4.3 core가 함께 가진 API를 쓴다(UE는 공통 기반 `Runtime/OpenGLDrv/Private/OpenGLBase.h` 위에 Windows `FOpenGL4 : FOpenGL3`(`OpenGL4.h:17`, `Windows/WindowsOpenGL.h:9–19`)와 Android `OpenGLES.h:21`을 둔다). 플랫폼별·확장 의존 항목은 따로 목록으로 관리한다.
  - 데스크톱 전용: `glClipControl`(`GL_ARB_clip_control`), `GL_FRAMEBUFFER_SRGB`와 WGL sRGB 픽셀 형식(현재 `OpenGLDevice.cpp:384`·`:943`), DSA(지금 코드가 쓰며 공통 경로에서 뺀다).
  - ES 확장 의존: 5.2절 G2의 형식·필터 확장, sRGB 기본 framebuffer(`EGL_KHR_gl_colorspace`), 이방성 필터(지금 코드 `:585`).
  - sRGB 표시: Windows 미리보기는 offscreen sRGB 형식 target에 그린 뒤 blit하고, Android는 `EGL_KHR_gl_colorspace`가 있으면 sRGB 창 surface, 없으면 offscreen sRGB target에서 변환해 표시한다.
- Windows 미리보기: GL 4.3 core 이상과 `GL_ARB_clip_control`·`GL_ARB_ES3_2_compatibility`를 요구하고 `GLES31_PC` 코드를 받는다. `#version 320 es` 원문을 데스크톱 GL이 받으려면 `GL_ARB_ES3_2_compatibility`가 있어야 한다(4.3 core는 `300 es`, 4.5 core는 `310 es`까지). 드라이버 지원은 측정 S12이며, 없으면 `GLES31_PC`만 `#version 310 es`로 내는 방안을 그때 정한다. 창 표시는 offscreen에 그린 뒤 Y 반전 blit이다(7.2절 8항). UE는 이 확장이 있으면 쓰고(`Runtime/OpenGLDrv/Private/OpenGLDrv.cpp:470–474`) 없을 때 셰이더 쪽 대체 경로가 없으므로(컬 방향만 뒤집는다, `OpenGLShaders.cpp:959–968`, `OpenGLState.cpp:50–51`) Rocks는 필수로 한다. 데스크톱 드라이버가 `#version 320 es` 원문을 받는지는 측정 S12다(UE 5.8도 데스크톱에서 버전 줄을 고치지 않는다, `Runtime/OpenGLDrv/Private/OpenGLShaders.cpp:785–846`).
- Android: EGL ES 3.2 context(UE `Runtime/OpenGLDrv/Private/Android/AndroidEGL.cpp:730–820`, 3.2가 안 되면 3.1로 다시 시도 `:803–808`), NDK 시스템 라이브러리 `EGL`·`GLESv3`, surface는 P13이다.
- 셰이더 생성: 쿠킹된 ESSL과 바인딩 머리(7.2절 4항)를 받아 `glCreateProgram`·`glAttachShader`·`glLinkProgram`으로 만든다. UE도 separate shader object를 쓰지 않는다(`Runtime/OpenGLDrv/Private/OpenGLBase.h:283–287`). 프로그램 바이너리 캐시는 후속이다(UE는 Android만 기본으로 켠다, `OpenGLProgramBinaryFileCache.cpp:39–48`).
- 완료 기준: EngineRuntime OpenGL 절 개정이 확정되고, Windows 미리보기 컨텍스트에서 기존 self-test가 GLES 공통 API만으로 통과한다(셰이더까지는 SH2).
- 의존: A0-G(G2). 막는 것: SH2(미리보기), SH6(Android).

**P12 macOS 실행 기반 — 미설계.**
- 현재: PlatformWindow는 Win32만 있다. Rocks.Build 언어는 C·C++뿐이다(`NativeBuildModels.cs:57`, `ActionGraph.cs:373–374`). framework 링크 필드는 있다(`NativeBuildModels.cs:145`). macOS는 CI에서만 빌드·시험한다.
- UE 5.8: Apple 대상 C++ 원본을 Objective-C++로 컴파일하고(`Programs/UnrealBuildTool/Platform/Apple/AppleToolChain.cs:461,474`), MetalRHI는 `.cpp`에서 metal-cpp(`ThirdParty/MetalCPP`)를 쓰며, 창은 Cocoa다(`Runtime/ApplicationCore/Private/Mac/CocoaWindow.cpp`).
- 산출물: Rocks.Build의 Apple Objective-C++ 컴파일(정책 검사와의 관계 포함), metal-cpp 공급(`kind: headers`), PlatformWindow Mac backend(Cocoa 창·`CAMetalLayer`·입력), macOS 실행 검증 경로(실기기 또는 CI의 Metal 장치).
- 완료 기준: macOS에서 창 생성·이벤트 펌프·크기 변경·닫기 시험, `CAMetalLayer`에 지운 색 한 장 표시, Objective-C++ 원본의 경고 0과 정책 검사 통과, macOS CI 실행.
- 막는 것: SH5의 MetalGraphics(P15).

**P13 Android 실행 기반 — 미설계(PlatformLayer 모바일 절).**
- 현재: Android는 시험 실행 파일을 에뮬레이터에 올려 실행만 한다(PlatformLayer 모바일 절). 앱 껍데기·창·surface·수명·APK 패키징이 없고, APK 안 데이터 읽기(VFS)는 AssetSystem 2절에서 후속이다.
- 산출물: 앱 진입(NativeActivity 계열), `ANativeWindow` surface와 생성·소실·재생성 수명, APK 패키징(manifest `glEsVersion` 0x00030002, UE와 같다), 에뮬레이터·실기기 실행 경로(측정 S11). APK 안 쿠킹 데이터 읽기(VFS)는 AssetSystem이 맡는 선행 작업이다(14절).
- 완료 기준: 앱 시작·종료, 백그라운드 전환 때 surface 소실·재생성 시험, EGL ES 3.2 context로 지운 색 한 장 표시, 에뮬레이터 또는 실기기 실행 기록.
- 막는 것: SH6.

**P14 셰이더 타입·퍼뮤테이션·라이브러리 형식 — 설계(12절).**
- 현재: 셰이더 타입 등록, 퍼뮤테이션, 셰이더 라이브러리 파일 형식, 런타임 조회 규칙이 없다(Renderer 내장 셰이더는 문자열 하나다).
- UE 5.8: 셰이더 타입은 `IMPLEMENT_GLOBAL_SHADER`로 정적 등록하고(`Runtime/RenderCore/Public/GlobalShader.h:433`), 퍼뮤테이션은 차원 타입(`SHADER_PERMUTATION_BOOL` 등, `Runtime/RenderCore/Public/ShaderPermutation.h:629`)을 묶은 `TShaderPermutationDomain`(`:272`)이 정하며, 타입마다 `ShouldCompilePermutation`으로 형식·퍼뮤테이션을 거른다(`GlobalShader.h:100–102`). 라이브러리는 셰이더 맵 hash·셰이더 hash 표와 항목 표를 가진다(`FSerializedShaderArchive`, `Runtime/RenderCore/Public/ShaderCodeArchive.h:206–230`, 항목 `:48`·`:70`).
- 산출물: 셰이더 타입 등록(이름·`.rsf` 경로·엔트리·단계·요구 기능, 2절), 퍼뮤테이션 차원 선언과 ID 계산, 형식·퍼뮤테이션 거르기 규칙, 라이브러리 항목 키(셰이더 타입·퍼뮤테이션 ID·단계 → 코드 SHA-256)와 파일 형식·버전, 런타임 조회와 빠졌을 때의 `IncompatibleCook`.
- 완료 기준: 등록·퍼뮤테이션 ID의 결정성 시험(등록 순서와 무관), 같은 코드의 중복 제거, 라이브러리 왕복·손상 검출, 빠진 항목 조회의 `IncompatibleCook`.
- 의존: P6a. 막는 것: P6b, SH2.

**P15 D3D11Graphics·MetalGraphics 설계 — 미설계(EngineRuntime).**
- 산출물: VulkanGraphics 절(P9)과 같은 수준의 backend 설계 절(장치·스왑체인·자원·파이프라인 layout(7.2절 4항)·검증 레이어·self-test 대상)이다. D3D11은 Windows, Metal은 P12 위에 선다.
- 막는 것: SH5.

### 5.4 측정

결과에 따라 설계가 갈리는 값은 짐작하지 않고 SH0(또는 해당 단계)에서 잰다.

| ID | 측정 | 결과가 정하는 것 |
|---|---|---|
| S1 | DXC 1.8.2505 내장 validator 서명(Windows·macOS) | 10절 서명 검사, `D3D12_SM6` 생산 호스트 |
| S2 | 개발 기계 GPU와 WARP의 D3D12 최대 셰이더 모델, Windows 10 Agility SDK 요건 | 결정 3 |
| S3 | 결정성: 같은 호스트 반복, Debug·Release 워커, 작업 디렉터리 변경, FMA3 유무, OS 버전, **Windows·macOS 사이**(DXIL·SPIR-V·ESSL·MSL 텍스트)에서 형식별 바이트 동일 | 10절 결정성 |
| S4 | Vulkan 검증 레이어 공급, GPU 없는 CI의 소프트웨어 ICD | P9 |
| S5 | D3D12·Vulkan·GLES 미리보기 픽셀 대조(행렬 옵션·앞면·Y 반전·루트 상수 대응) | 7.2절 6·8항 |
| S6 | 도구 빌드 컴파일러(clang-cl·MSVC)·빌드 시간·바이너리 크기 | 8절 4항 |
| S7 | 자체 전처리 결과로 컴파일한 출력 = DXC 직접 전처리 출력(`#line` 포함) | 결정 6 구현의 정합성(SH1 완료 기준) |
| S8 | 워커 기동·DLL 로드 비용, SHA-256 처리량 | 11절 묶음 크기 |
| S9 | Apple `metal` 재배포 조건, Windows용 Metal Developer Tools(UE 5.8 기대 버전 5.3, `Developer/Apple/MetalShaderFormat/Private/MetalShaderFormat.cpp:724–725`) | 결정 5 |
| S10 | `d3dcompiler_47.dll` 재배포 조건과 고정 버전 | 8절 2항 FXC |
| S11 | Android 에뮬레이터·CI의 OpenGL ES 3.2 context | SH6 검증 경로 |
| S12 | (SH0) 데스크톱 GL 드라이버의 `#version 320 es` 수용(`GL_ARB_ES3_2_compatibility` 유무), (SH6) UE 수정 pass 없이 업스트림 SPIRV-Tools·SPIRV-Cross만으로 만든 ESSL이 Android 기기 드라이버에서 도는지 | P11, 7.2절 11항 |

## 6. 전체 순서

고정 순서(사용자 지시): A0-B → A0-O → A0-G → A1–A6 → 화면 마일스톤. A0-B는 B8b(`e405c37`)로 끝났다. 셰이더 시스템은 이 순서를 바꾸지 않고 다음 자리에 들어간다.

1. **지금**: 이 문서를 스펙으로만 둔다.
2. **A0-G·A1·A2·A3·A4 착수 전**: 5.2절 셰이더 요구(G1–G3, A1 ErrorCode, A2 `ProjectLayout` 위치, N1–N3, K1–K3)를 AssetSystem에 반영한다(P8의 AssetSystem 부분). A0-O에 넣을 셰이더 요구는 없으므로 가장 가까운 기한은 A0-G 착수 전이다. 반영하지 않고 그 단계를 구현하면 셰이더 착수 때 그 단계를 다시 고쳐야 한다.
3. **셰이더 구현 착수**: A6 뒤다. 의존만 보면 A4 뒤면 되지만(P5가 A2·A3을, P6b가 A1·A4를 요구한다) 고정 순서가 A1–A6을 한 묶음으로 두기 때문이다. 화면 마일스톤 앞인지 뒤인지는 15절 결정 11이다.
4. **셰이더 구현 안의 순서**: P2·P8(문서, 15절 결정별 기한) → P10 → P3 → P4 → SH0 → P6a → P5 → SH1 → P14 → P9·P11 → P6b → SH2 → P7 → SH3 → SH4 → P12·P15 → SH5 → P13 → SH6.

## 7. P2 바인딩 규약

지금은 backend 사이 바인딩 규약이 없다. D3D12는 장치 하나에 root signature 하나(32-bit 상수 20개 b0, SRV 테이블 t0 하나와 샘플러 테이블 s0 하나는 PS만, `D3D12Device.cpp:1083–1120`)이고, OpenGL은 uniform을 이름으로 찾는다(`OpenGLDevice.cpp:653–654`). 교차 컴파일 전에 아래 규칙을 고정한다.

### 7.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 선언 | 일반 파라미터는 register를 적지 않는다(`cbuffer Name {}`). 슬롯은 컴파일러가 정하고 리플렉션으로 읽어 이름 → (슬롯, 오프셋, 크기) 표에 둔다. 정적 UB·루트 상수·진단 버퍼·정적 샘플러는 예약 space를 register로 명시한다. 정적 UB는 `UB_CB_DEFINITION_START_REG`·`UB_CBO_*_REG`(`ConstantBuffer<T>`)로 선언한다 | `Engine/Shaders/Public/Platform.ush:522–523`(일반), `:538–562`(정적 UB), 생성기 `Runtime/RenderCore/Private/ShaderParameters.cpp:460–493`, `Engine/Shaders/Public/Platform/D3D/D3DRootConstants.ush:12`, `Engine/Shaders/Public/Platform/D3D/D3DCommon.ush:23–28·40`, `Runtime/RenderCore/Public/ShaderCore.h:285–382`, `Developer/ShaderCompilerCommon/Private/ShaderCompilerCommon.cpp:743–760` |
| 예약 register space | 0 기본, 1·2 레이 트레이싱·워크 그래프, 3 루트 상수, 4 정적 셰이더 바인딩, 999 진단, 1001 NV 확장. 1000 정적 샘플러(D3D12) | `Engine/Shaders/Shared/HLSLReservedSpaces.h:19–54`, 1000은 `Runtime/D3D12RHI/Private/D3D12RootSignature.cpp:54–61`·`D3DCommon.ush:23–28` |
| D3D12 | Windows는 양자화된 셰이더 조합(QBSS)마다 root signature를 만들어 캐시한다(`USE_STATIC_ROOT_SIGNATURE 0`). QBSS와 root signature는 1:1이고, 표 크기를 양자화하므로 여러 조합이 하나를 공유한다(SRV·Sampler는 Tier 1만 2의 거듭제곱 올림이고 Tier 2 이상은 최대값 64/32, UAV와 16개를 넘는 CBV는 Tier 1–2에서 2의 거듭제곱 올림이고 Tier 3은 16). 단계 PS·VS·GS·MS·AS 순으로 SRV·(넘친 CBV)·Sampler·UAV 테이블, 상수 버퍼는 단계별 root CBV 최대 16개(`MAX_ROOT_CBVS = MAX_CBS = 16`)이고 넘치는 것은 CBV 테이블로 간다. 루트 상수는 4 DWORD(b0, space3). 12 DWORD를 넘으면 Verbose 로그만 남기고 64 DWORD 한도는 따로 검사하지 않는다(넘으면 직렬화 실패가 Fatal) | `Runtime/D3D12RHI/Private/Windows/WindowsD3D12RHIDefinitions.h:29`, `Runtime/D3D12RHI/Private/D3D12Util.cpp:1169–1210`(양자화), `:1383–1470`(1:1 `:1414`), `D3D12RootSignature.cpp:166–176`(단계 순서), `:298–308`(넘친 CBV), `:434`(루트 상수), `:463–466`(경고), `:584–587`(Fatal), `D3D12RHIDefinitions.h:66`, `Runtime/D3D12RHI/Public/D3D12RHI.h:17–20` |
| Vulkan | bindless가 아닐 때(`SF_VULKAN_SM5`) descriptor set = 단계(VS 0, PS 1, GS 2…). binding은 SPIRV-Reflect로 0부터 연속 재배치(`$Globals` UB 0 → UB → input attachment → UAV → SRV → sampler). 구조화 버퍼 SRV는 storage buffer라 UAV 묶음에 들어간다. bindless일 때는 set이 sampler 0·resource 1·single-use UB 2이고 UB binding은 단계별 기준(PS 0, VS·MS 16, GS·AS는 31부터 아래로)에 순번을 더한다. bindless는 descriptor buffer(기본)·descriptor heap(선택)을 쓴다. push constant는 비-bindless layout에서 쓰지 않고 bindless RayGen의 정적 UB 인덱스에만 쓴다. SM6는 scalar layout도 쓴다 | `Runtime/VulkanRHI/Public/VulkanCommon.h:27–51`(단계 set), `:98–156`(bindless), `Developer/VulkanShaderFormat/Private/SpirVShaderCompiler.inl:534–550`, `:931–968`(binding 순서), `:556–558`(구조화 SRV), `:741,775`(bindless UB), `:1396–1397`(scalar), `Runtime/VulkanRHI/Private/VulkanBindlessDescriptorManager.cpp:25–33`, `Runtime/VulkanRHI/Private/VulkanLayout.cpp:36–44`, `Engine/Shaders/Public/Platform/Vulkan/VulkanCommon.ush:170–177` |
| Metal(SPIRV-Cross) | UAV를 먼저 배정하며 같은 번호를 buffer·texture 공간에서 함께 차지한다. buffer는 그 뒤 (간접 인자 버퍼를 쓰면 그 슬롯) → UB → 구조화 SRV → 가속 구조 순, 버퍼 크기 side table은 남은 가장 낮은 buffer 인덱스. 정점 버퍼는 위에서부터(`(MaxMetalStreams-1)-i`, Mac 31) | `Developer/Apple/MetalShaderFormat/Private/MetalCompileShaderSPIRV.cpp:515–566`(UAV), `:569–684`(간접 인자 버퍼), `:686`(UB), `:737`(구조화 SRV), `:752–765`(가속 구조), `:928–939`(side table), `Runtime/Apple/MetalRHI/Private/MetalRHIPrivate.h:64–128`(Mac 31 `:70`) |
| D3D11 | 단계별 슬롯 상한 min(D3D11 한도, UE MAX_*), 단계당 상수 버퍼 14 | `Developer/Windows/ShaderFormatD3D/Private/D3DShaderCompiler.cpp:1091–1095`, `Runtime/Windows/D3D11RHI/Private/D3D11RHIPrivate.h:657–658` |
| OpenGL ES | OpenGL 플랫폼이면 상수 버퍼를 늘 에뮬레이션한다(`NoEmulatedUniformBuffer` 표시 UB만 진짜 UBO). SPIRV-Cross 출력의 `Name.member`를 텍스트로 바꿔 단계(v·p·c)와 기본 타입(h float·m mediump·i int·u uint)별 `vec4`·`ivec4`·`uvec4` uniform 배열(`$Globals`는 `pu_h` 등, UB는 `pc<n>_h` 등)과 `#define`으로 잇는다(구조체는 1단까지 처리). 런타임은 복사 표대로 바뀐 구간만 `glUniform4fv/4iv/4uiv`로 올린다. 진짜 UBO는 std140과 `glBindBufferRange`다. 텍스처는 combined sampler(텍스처마다 샘플러 하나, 이름 `ps<n>`)이고, ESSL 320 출력은 `layout(binding)`을 내며(ESSL 310이면 생략) 런타임이 링크 뒤 `glUniform1i`로 유닛을 정한다(PS 0부터, VS는 `MaxTextureImageUnits`부터). 셰이더당 샘플러 16개를 넘으면 컴파일 오류다. UAV는 CS·PS만 | `Runtime/RenderCore/Private/Shader.cpp:1695–1703`, `Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp:3856–3863`, `Developer/ShaderFormatOpenGL/Private/OpenGLShaderCompiler.cpp:1027–1037, 1137–1240, 1300–1333, 1471–1500, 1517–1548, 1797–1890`(이름·묶음·구조체), `:1505`(진짜 UBO), `:2252–2311`(샘플러 이름), `:868–876`(16개), `Runtime/OpenGLDrv/Private/OpenGLShaders.cpp:1986–2056, 3226–3410`(업로드), `:1175–1208`(UBO), `OpenGLBase.h:129–133`·`OpenGLShaders.cpp:1950–1958, 2064–2108`(유닛), `:2162`(UAV), `ThirdParty/ShaderConductor/ShaderConductor/External/SPIRV-Cross/spirv_glsl.cpp:2220–2225`(binding 버전 조건) |
| 상수 버퍼 레이아웃 | C++ 쪽 정렬(4/8/16, 구조체·배열 원소 16)과 static_assert, 생성 HLSL에 명시 padding, 스칼라 배열은 float4로 묶는다(`SHADER_PARAMETER_SCALAR_ARRAY`). SPIR-V는 UE가 수정한 DXC의 `-fvk-ue5-layout`(cbuffer 엄격한 std140, DXC 기본은 vector-relaxed std140). Vulkan SM6·레이 트레이싱만 scalar, GLES는 scalar block layout을 끈다 | `Runtime/RenderCore/Public/ShaderParameterMacros.h:858–1130, 1966–1975`, `Engine/Shaders/Public/Platform.ush:1534–1536`, `ShaderParameters.cpp:210–447`(padding `:240–257`), `ThirdParty/ShaderConductor/ShaderConductor/Source/Core/ShaderConductor.cpp:1334–1336`, UE 동봉 DXC `tools/clang/lib/SPIRV/SpirvEmitter.cpp:682–688`, `OpenGLShaderCompiler.cpp:2773–2774` |
| 정점 입력 | 시맨틱 `ATTRIBUTEn`. VS 입력만 시맨틱 번호로 location을 다시 배정하고 단계 사이 변수는 다시 쓰지 않는다. GLES도 `ATTRIBUTEn`을 location n으로 쓴다 | `Engine/Shaders/Private/LocalVertexFactory.ush:52–66`, `Developer/ShaderCompilerCommon/Private/SpirvReflectCommon.cpp:228–242`, `SpirVShaderCompiler.inl:561–565`, `MetalCompileShaderSPIRV.cpp:845–921`, `OpenGLShaderCompiler.cpp:1718–1750` |
| 좌표 | 셰이더는 D3D 규약(NDC y 위, z [0,1]). Vulkan은 음수 viewport 높이로 뒤집는다. HLSL을 뺀 모든 대상(SPIR-V·MSL·ESSL)에 `-fvk-use-dx-position-w`를 넘긴다(원 DXC에도 있는 옵션). GLES Android는 SPIRV-Cross 업스트림 옵션 `fixup_clipspace`·`flip_vert_y`로 VS 끝에서 z·y를 보정하고 컬 방향을 뒤집으며 표시 때 Y를 뒤집는다. 데스크톱 GLES 미리보기는 두 옵션을 끄고(`force_glsl_clipspace`는 ShaderConductor가 이 두 옵션을 끄라는 표시로 UE가 더한 필드다) `glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)`을 쓰며, clip control이 없을 때 셰이더 쪽 대체 경로는 없다(컬 방향만 뒤집는다. 런타임이 붙이는 `HLSLCC_DX11ClipSpace` 정의는 옛 hlslcc만 읽고 5.8의 SPIRV-Cross 경로에서는 쓰는 곳이 없다) | `Runtime/VulkanRHI/Private/VulkanPendingState.cpp:500–506`, `ShaderConductor.cpp:1337–1340`, `:1542–1551`(GLES 보정), `:1621–1626`·`spirv_glsl.hpp:199–201`(데스크톱), `Runtime/OpenGLDrv/Private/OpenGLState.cpp:50–51`, `AndroidEGL.cpp:36–42`, `OpenGLDrv.cpp:470–474`, `OpenGLShaders.cpp:959–968`(대체 경로 없음) |

### 7.2 Rocks 규약

1. **선언.** 셰이더 원문은 register·`[[vk::*]]`를 적지 않는다(UE의 일반 파라미터와 같다). 예외는 `Platform.rsh`가 형식별로 전개하는 매크로 둘이다: 루트 상수(2항)와 storage image 형식 `ROCKS_IMAGE_FORMAT(<형식>)`. DXC는 `RWTexture2D<float4>`의 SPIR-V 형식을 원소 타입으로 추론하므로(예: Rgba32f) 실제 형식이 다르면 Vulkan 검증 오류가 나고, ESSL image는 형식 한정자가 필요하다. 이 매크로는 SPIR-V 형식에서 DXC의 image format 지정으로, D3D 형식에서는 빈 것으로 전개한다. DXC가 지정을 지원하는지는 SH0에서 확인하고, 안 되면 워커가 SPIR-V `OpTypeImage`의 형식 operand를 고친다. Vulkan 형식 정의는 `shaderStorageImageWriteWithoutFormat`을 요구하지 않는다. 정적 샘플러 같은 다른 예약 space는 후속이다. 상수는 `cbuffer`·`ConstantBuffer<T>`·루트 상수로만 선언하고, 이름 없는 전역 uniform(`$Globals`가 되는 전역 변수)은 컴파일 오류다. UE는 느슨한 파라미터를 `$Globals`에 두지만, Rocks 파라미터는 언제나 이름 있는 버퍼다(13절).
2. **루트 상수(그리기별 상수).** `ROCKS_ROOT_CONSTANTS(Type, Name)` 하나로 선언한다. 크기는 최대 128 byte(32 DWORD, Vulkan push constant 최소 보장)이고 멤버는 5항 규칙을 따른다. 현재 `DrawConstants`(80 byte)가 첫 사용자다.

   | 형식 | 전개 | 실제 위치 |
   |---|---|---|
   | `D3D12_SM6` | `ConstantBuffer<Type> Name : register(b0, space3)` | root 32-bit constants(UE 예약 space 3과 같은 번호) |
   | `D3D11_SM5`(FXC) | `cbuffer` `register(b13)`(SM 5.0에는 space가 없다) | 그리기마다 갱신하는 상수 버퍼, 단계별 마지막 슬롯 |
   | `D3D11_SM5`(SPIRV-Cross 경로) | 아래 Vulkan 전개로 SPIR-V를 만든 뒤 `spvc_compiler_hlsl_set_root_constants_layout`으로 b13 | 위와 같다 |
   | `VULKAN_SM5` | `[[vk::push_constant]] Type Name` | push constant. DXC는 push constant에 구조화 버퍼 규칙(`sBufferLayoutRule`, 기본 vector-relaxed std430)을 쓴다(업스트림 DXC `tools/clang/lib/SPIRV/DeclResultIdMapper.cpp`. 줄 번호는 업스트림 판마다 달라 적지 않고 SH0에서 리플렉션 오프셋으로 확인한다, 7.3절 (a)) |
   | `METAL_SM5` | Vulkan 전개와 같다 | SPIRV-Cross push constant 자원(`SPVC_MSL_PUSH_CONSTANT_DESC_SET`·`SPVC_MSL_PUSH_CONSTANT_BINDING`)을 buffer 0에 지정하고 `set{Vertex,Fragment}Bytes`로 올린다 |
   | `GLES31_*` | Vulkan 전개와 같다 | SPIRV-Cross가 일반 uniform으로 내보낸 것을 4항 GLES 에뮬레이션의 `<단계>u` 배열(PS는 `pu_h` 등)로 묶고 그리기마다 올린다(UE `$Globals`의 `pu_h`와 같은 자리) |

   UE는 비-bindless Vulkan layout에서 push constant를 쓰지 않는다(7.1절). Rocks는 그리기별 상수를 위해 쓰며 7.3절 시험 (a)·(d)로 확인한다.
3. **슬롯 정규화.** 컴파일 후 형식별 리플렉션(DXIL `ID3D12ShaderReflection`, SPIR-V SPIRV-Reflect)을 Rocks 공통 `ShaderBindingLayout`으로 바꾼다. 단위는 (단계, 종류, 단계 안 순번)이며 이름 hash·CB 크기·멤버 오프셋·텍스처와 샘플러의 짝(9항)을 함께 둔다(UE `FShaderParameterMap`·`FVulkanShaderHeader` 대응). 종류는 Vulkan descriptor 종류를 기준으로 나눈다: 상수 버퍼, SRV 텍스처(typed buffer SRV 포함), 구조화·raw 버퍼(SRV·UAV), storage image(UAV 텍스처·typed buffer UAV), 샘플러.
4. **상한과 backend별 실제 위치.**
   - **상한 검사 위치.** 형식 정의의 상한(장치 요구 수준별)과 프로필 공통 상한(프로필에 든 형식들의 최소값)은 job 안에서 검사한다. 형식 정의와 프로파일 출력 설정이 모두 키에 있으므로(10절) 프로필에 형식을 더해 상한이 내려가면 키가 바뀌어 다시 검사된다. 파이프라인 합계 한도(D3D12 Tier 1 UAV, Vulkan `maxDescriptorSet*`)는 파이프라인 layout을 만들 때(쿠킹 때 알려진 쌍, 런타임 생성) 검사한다.
   - **데스크톱 공통 상한(초기값).** Vulkan 값은 핵심 최소 보장이다. 더 큰 장치 요구는 다른 형식 id로 둔다(2절).

     | 종류(단계당) | D3D12(Tier 1) | D3D11(FL 11.0) | Vulkan 최소 | Metal(macOS) | 데스크톱 공통 |
     |---|---|---|---|---|---|
     | 상수 버퍼 | 14 | 14(Rocks는 13 + 루트 상수) | 12(uniform buffer) | buffer 공간 | 12 |
     | SRV 텍스처·typed buffer SRV | 128 | 128 | 16(sampled image) | 128 | 16 |
     | 구조화·raw 버퍼(SRV·UAV 합) | SRV 128, UAV는 아래 | SRV 128, UAV는 아래 | 4(storage buffer) | buffer 공간 | 4 |
     | storage image(UAV 텍스처·typed buffer UAV) | UAV는 아래 | UAV는 아래 | 4 | texture 공간 | 4 |
     | 샘플러 | 16 | 16 | 16 | 16 | 16 |
     | UAV 단계 제약 | Tier 1은 모든 단계 합 8(FL 11.0), FL 11.1 이상 64 | PS·CS만, PS는 render target과 출력 슬롯 8개를 나눈다 | VS·PS 쓰기는 선택 기능(`vertexPipelineStoresAndAtomics`·`fragmentStoresAndAtomics`), PS는 `maxFragmentCombinedOutputResources`(최소 4)에 color attachment가 들어간다 | — | **UAV는 CS만**. VS·PS UAV는 형식 정의의 기능 요구로 후속 |

     근거: D3D12 [하드웨어 지원 tier](https://learn.microsoft.com/en-us/windows/win32/direct3d12/hardware-support)(Tier 1 UAV는 "모든 단계의 모든 descriptor table" 합), Vulkan 사양 필수 한도(`maxPerStageDescriptor*`, Vulkan-Docs `chapters/limits.adoc`).
   - **모바일 상한.** Android 프로필(`GLES31_ANDROID`, SH6 이후 `VULKAN_ANDROID`)의 상한도 형식 정의가 가진다. GLES 형식 정의는 OpenGL ES 3.2 사양의 최소 보장과 UE 상한(셰이더당 샘플러 16, `OpenGLShaderCompiler.cpp:868–876`. 런타임 PS·VS 16, 합계 32, `Runtime/OpenGLDrv/Private/OpenGLES.cpp:160–163`) 가운데 작은 값을 P6a에서 정해 다음 항목을 가지며 워커가 검사한다(P11은 기기 값으로 확인만 한다): 단계별 샘플러 수, 단계별 에뮬레이션 `vec4` 합계(상수 버퍼·루트 상수 크기 합 ÷ 16을 `GL_MAX_VERTEX_UNIFORM_VECTORS`·`GL_MAX_FRAGMENT_UNIFORM_VECTORS`·compute uniform 한도와 대조), VS·PS storage block·image 수(0), 단계 사이 varying 벡터 수(`GL_MAX_VARYING_VECTORS`), MRT 수(`GL_MAX_DRAW_BUFFERS`. 넘는 `SV_Target`은 GLES 형식에서 컴파일 오류).
   - **공통 상한의 범위.** 프로필 공통 상한은 그 셰이더 타입이 그 프로필에서 컴파일되는 형식들(P14 거르기 뒤)의 형식 정의 상한 가운데 최소값이다. 그래서 GLES 형식을 거르는 데스크톱 전용 셰이더 타입에는 GLES 상한이 걸리지 않고, 엔진 내장 셰이더처럼 GLES에서도 쓰는 타입에는 걸린다. 이 계산에 든 형식 정의들의 SHA-256이 job 키 9항에 들어간다(10절).
   - **D3D12.** 파이프라인마다 VS·PS layout을 합쳐 root signature를 만들고 캐시한다. UE QBSS 구조를 따르되 처음에는 개수를 양자화하지 않는다(root signature 수가 문제가 되면 UE 양자화를 들인다). 순서는 단계별 테이블(SRV·Sampler·UAV, 필요하면 CBV) → root CBV → 루트 상수다. 예산식 `루트 상수 DWORD + 테이블 수 + 2 × root CBV 수 ≤ 64`를 만족하도록 root CBV를 채우고, 넘치는 CBV는 높은 슬롯부터 그 단계의 CBV 테이블로 내린다(UE의 넘친 CBV 테이블과 같은 방식, UE는 16개에서 자른다). Rocks 그래픽 파이프라인 단계는 VS·PS이고 테이블은 단계당 최대 4개라, 루트 상수 32 + 테이블 8 = 40이므로 root CBV는 최대 12개이고 나머지는 CBV 테이블로 간다. CS 파이프라인은 테이블 4개라 root CBV 14개까지다. 어느 경우도 항상 만들 수 있다.
   - **Vulkan.** set = 단계(VS 0, PS 1, CS 0. 최소 보장 `maxBoundDescriptorSets` 4 안이다), binding = 종류 순서(상수 버퍼 → 구조화·raw 버퍼 → storage image → SRV 텍스처 → 샘플러)로 0부터 연속. 워커가 DXC 출력의 decoration을 SPIRV-Reflect로 고친다(UE `SF_VULKAN_SM5`의 비-bindless 경로와 같은 방식, `-fvk-*-shift`는 쓰지 않는다).
   - **Metal.** buffer 인덱스: 0은 루트 상수, 1부터 UAV(UE처럼 buffer·texture 공간에서 같은 번호를 함께 차지한다) → 상수 버퍼 → 구조화 SRV → 버퍼 크기 side table(UE 5.8 순서. Rocks에는 간접 인자 버퍼와 가속 구조가 없다). texture 인덱스: UAV가 차지한 번호(1부터)를 비우고 SRV 텍스처는 0과 남은 가장 낮은 번호부터 쓴다. sampler는 0부터다. 정점 버퍼는 30부터 아래로 쓴다(UE와 같은 방향). 최대 정점 스트림을 8로 고정하고, VS·PS는 셰이더 buffer 인덱스 ≤ 22(정점 버퍼 8개가 30–23을 차지한다)를 컴파일 때 검사한다. 공통 상한에서 VS·PS의 최대는 루트 상수 1 + 구조화 SRV 4 + 상수 버퍼 12 + side table 1 = 18이라 들어간다. 실제 정점 버퍼 수와의 겹침은 파이프라인 생성 때 검사한다. CS는 정점 버퍼가 없으므로 0–30을 쓰고, 최대는 루트 상수 1 + 구조화·raw 버퍼 4 + storage image 4(UAV라 buffer 번호도 차지) + 상수 버퍼 12 + side table 1 = 22다.
   - **D3D11.** FXC 경로는 FXC가 정한 단계별 슬롯을 그대로 쓰고 상한을 넘으면 오류다. PS UAV는 공통 상한에서 금지되므로 FXC가 u0을 주어 render target 0과 겹치는 일이 없다. SPIRV-Cross 경로는 종류 구분 없이 매긴 SPIR-V binding을 `spvc_compiler_hlsl_add_resource_binding`으로 종류별 register(b·t·u·s, 단계별 0부터)에 다시 배정하고(그대로 두면 s24처럼 범위 밖 register가 나온다), 정점 입력은 `spvc_compiler_hlsl_add_vertex_attribute_remap`으로 `ATTRIBUTEn` 시맨틱을 유지한다(SPIRV-Cross 기본은 `TEXCOORD{location}`).
   - **OpenGL ES(UE 방식).**
     - 상수 버퍼와 루트 상수는 모두 에뮬레이션한다(UE도 OpenGL이면 늘 에뮬레이션한다, 7.1절). 워커가 SPIRV-Cross `emit_uniform_buffer_as_plain_uniforms`로 일반 uniform이 된 버퍼의 `Name.member` 참조를 텍스트로 바꿔, 단계(`v`·`p`·`c`)와 기본 타입(`h` float·`i` int·`u` uint)별 `uniform highp vec4 <단계>c<순번>_h[N]`·`ivec4 …_i[N]`·`uvec4 …_u[N]` 배열과 `#define`으로 잇는다. 루트 상수는 `<단계>u_<타입>`(UE `$Globals`의 `pu_h` 자리)이다. 이름 규칙은 UE와 같다. 바꾼 뒤 원래 버퍼 이름이 남아 있으면(블록 전체 load 등) 컴파일 오류다.
     - 5항이 허용하는 타입은 4 byte 원소이고 3·4성분과 행렬이 16 byte 정렬이므로 멤버는 `vec4` 경계를 넘지 않고, `float4x4`는 연속한 원소 4개다. 5항이 16 byte 슬롯 하나에 기본 타입을 섞지 않게 하므로 슬롯마다 타입이 하나이고, 슬롯을 타입별 배열에 차례로 빽빽하게 넣는다. 그래서 타입별 배열 크기의 합은 버퍼 크기 ÷ 16이다(UE는 멤버마다 `vec4`를 새로 시작한다, `OpenGLShaderCompiler.cpp:1297`).
     - 쿠킹 결과에 복사 표(버퍼 순번, 바이트 범위 → 배열·원소)를 둔다. 런타임은 그리기 때 바뀐 버퍼만 복사 표대로 `glUniform4fv/4iv/4uiv`로 올린다(UE `UniformBuffersCopyInfo`).
     - mediump 배열은 없다. 5항이 half(`min16*`)를 막으므로 실수는 모두 highp다(UE는 RelaxedPrecision을 `m` 배열로 나눈다, `OpenGLShaderCompiler.cpp:1117–1124`).
     - 진짜 UBO(std140, `glBindBufferRange`)는 후속이다(UE `NoEmulatedUniformBuffer` 표시에 해당).
     - 텍스처는 9항의 짝마다 combined sampler 하나(SPIRV-Cross `build_combined_image_samplers`)이고 이름은 `<단계>s<순번>`(UE `ps<n>`)이다. Vulkan 재배치(set = 단계, binding 0부터, 종류를 이어서 번호)를 그대로 ESSL에 내면 VS와 PS 샘플러가 같은 binding이 되고 GLES의 종류별 binding 공간(텍스처 유닛·image 유닛·storage block binding)과도 맞지 않으므로, 워커가 GLES용 binding을 종류마다 다시 매긴다. 텍스처 유닛은 PS·CS 0부터, VS 16부터다(셰이더당 샘플러 16개 이하이고 ES 3.2의 `GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS` 최소 96 안이다). image 유닛과 storage block binding은 0부터다. UE는 binding을 낸 뒤 런타임이 `glUniform1i`로 VS 유닛을 `MaxTextureImageUnits`부터 정한다(7.1절). Rocks는 컴파일 때 고정해 런타임 조회를 없앤다. 샘플러 상태는 샘플러 객체(`glBindSampler`)다.
     - storage image는 CS만이다(4항 공통 상한). 구조화·raw 버퍼 SRV도 GLES 형식에서는 CS만이다: ES 3.2의 VS·PS storage block 최소 보장이 0이기 때문이며, 모바일 상한(VS·PS storage block 0)으로 워커가 검사한다.
     - 정점 입력은 `layout(location = n) in`(`ATTRIBUTEn`), PS 출력은 SPIRV-Cross 이름(`out_var_SV_TargetN`)에 `layout(location = n)`이다.
     - 바인딩 머리(attribute 마스크, 배열 이름과 크기, 샘플러 이름과 유닛, 복사 표)는 쿠킹 결과의 별도 부분이다. 런타임은 링크 때 `glGetUniformLocation`으로 배열 위치를 한 번 찾는다.
5. **상수 버퍼 레이아웃.** D3D cbuffer, DXC 기본 cbuffer 규칙(vector-relaxed std140), push constant 규칙(vector-relaxed std430), GLES 에뮬레이션 배열이 같은 오프셋을 내도록 허용 타입을 좁히고, 워커가 실제 오프셋을 검사한다.
   - 멤버 타입: 4 byte 스칼라(float·int·uint), 그 2–4성분 벡터, `float4x4`만 허용한다. bool·double·half(`min16*`)와 다른 행렬(`float3x3` 등)은 컴파일 오류다. `float3x4`·`float4x3`는 6항의 행렬 저장 결정 뒤 별도 시험으로 허용 여부를 정한다.
   - 정렬: 스칼라 4, 2성분 8, 3·4성분·행렬 16. 이 정렬이 요구하는 자리에 명시 padding 멤버를 둔다(UE와 같다).
   - 배열: 원소 크기가 16 byte 배수일 때만 허용한다. 스칼라·2·3성분 배열은 float4 배열로 묶는다(UE `SHADER_PARAMETER_SCALAR_ARRAY`·`GET_SCALAR_ARRAY_ELEMENT`와 같은 방식).
   - 기본 타입 슬롯: 16 byte 슬롯 하나 안에 float·int·uint를 섞지 않는다(예: `float a; int b;`는 b 앞에 padding을 두어 다음 슬롯으로 보낸다). GLES 에뮬레이션이 슬롯을 타입별 배열에 나눠 담기 때문이다(4항).
   - 구조체: 16 byte 정렬로 시작하고 크기를 padding으로 16 byte 배수로 맞춘다. 상수 버퍼 안의 구조체는 1단까지이고 구조체 배열은 금지한다. 상수 버퍼 전체를 한 번에 읽는 것(`Type t = Name;`, 함수 인자로 버퍼 전체를 넘기기)도 금지한다. GLES 에뮬레이션이 멤버 참조를 텍스트로 바꾸기 때문이며(UE도 1단까지만 처리한다, `OpenGLShaderCompiler.cpp:1517–1548`), 한 원문을 모든 형식에 쓰므로 모든 형식에서 같은 규칙이다.
   - **워커 검사.** 모든 형식의 리플렉션 오프셋이 위 규칙으로 계산한 오프셋과 같아야 하며, 다르면(암묵 padding·다른 배치) 컴파일 오류다. GLES 복사 표는 이 오프셋으로 만든다. 손으로 쓴 구조체(SH0–SH2의 `ROCKS_ROOT_CONSTANTS`, `DrawConstants`)도 같다.
   - **손으로 쓴 레이아웃 표.** P7(13절) 전에는 손으로 쓴 구조체마다 C++ 쪽에 레이아웃 표(멤버 이름·타입·오프셋·크기)를 두고 `static_assert`로 C++ 오프셋과 맞춘다. 이 표의 SHA-256이 파라미터 레이아웃 서명(job 키 8항, 10절)이고, 워커는 이 표를 받아 리플렉션 오프셋과 대조한다(7.3절 (a)). P7 뒤에는 MetaCompiler가 같은 형식의 표를 만든다.
   - 반례(이 규칙이 막는 것): `float a[2]; float b;`는 D3D에서 b가 20(마지막 원소의 남는 자리에 채움), std140에서 32이고, 명시 padding으로도 두 규칙을 함께 맞출 수 없다. `float3x3 m; float s;`는 D3D 44, std140 48이다. push constant의 스칼라 배열은 std430 stride 4, D3D root constant는 16이다. `struct S { float4 v; }; struct R { float a; S s; }`의 s는 D3D·std140 16, push constant(std430, 구조체 정렬 4) 4다. 명시 padding이 없으면 워커 검사가 거부한다.
   - C++ 타입: Core `Matrix4`·`Vector3`는 정렬 지정이 없다(`Matrix4.h:56–61`, `Vector3.h:19–24`). 그래서 ShaderCore에 셰이더 전용 정렬 타입(`alignas(8)` 2성분, `alignas(16)` 3·4성분·`float4x4`)을 두고, 셰이더 파라미터 구조체는 이 타입만 쓴다(13절에서 MetaCompiler가 강제). `DrawConstants`의 C++ `float transform[16]`(`DeviceTypes.h:161–164`)은 이 행렬 타입으로 바꾼다.
   - Rocks는 수정되지 않은 DXC를 쓰므로 scalar block layout을 쓰지 않는다(Android Vulkan 1.1·GLES와 호환).
6. **행렬.** Rocks는 열 벡터·열 우선 저장(EngineRuntime 10.1)이므로 UE의 `-Zpr`(`D3DShaderCompilerDXC.cpp:190`, `D3DShaderCompiler.cpp:1339`)를 쓰지 않는다. 형식별 DXC 행렬 옵션은 비대칭 행렬을 쓴 픽셀 대조 시험(SH2, 측정 S5)으로 확정하고 이 문서에 기록한다. 시험 전에는 옵션을 단정하지 않는다.
7. **정점 입력과 단계 사이 변수.** DXC `-spirv`는 입출력 location을 시맨틱 번호가 아니라 선언 순서로 준다(기본 `-fvk-stage-io-order=decl`). D3D11·D3D12는 VS 출력과 PS 입력을 register 위치로 맞추고 register는 선언 순서로 정해진다. 그래서 두 단계를 따로 컴파일해도 맞도록 다음을 지킨다.
   - 정점 입력 시맨틱은 `ATTRIBUTEn`이다(현재 `TEXCOORDn`, `DeviceTypes.h:130–136`). n은 `VertexAttribute::location`이다.
   - 단계 사이 변수는 하나의 struct로 선언하고 VS 출력과 PS 입력이 같은 struct를 쓴다(공용 `.rsh`). struct 멤버 순서는 `SV_Position` 다음 `TEXCOORDn`을 n 오름차순으로 둔다. 그러면 선언 순서 = location 순서라 FXC 경로(선언 순서)와 SPIRV-Cross 경로(location 순서)의 signature가 같다. PS 입력에 다른 struct를 쓰려면 VS 출력의 앞부분(같은 순서)이어야 한다.
   - 허용 시맨틱: 단계 사이는 `TEXCOORDn`(n ≤ 15, GLES 형식은 4항 모바일 상한의 varying 수 안). 시스템 값은 `SV_Position`·`SV_Target0–7`(GLES 형식은 MRT 상한 안)·`SV_Depth`·`SV_VertexID`·`SV_InstanceID`·`SV_IsFrontFace`와 CS의 `SV_DispatchThreadID`·`SV_GroupID`·`SV_GroupThreadID`·`SV_GroupIndex`다. 형식별 기능이 필요한 시스템 값(PS의 `SV_PrimitiveID`는 Vulkan Geometry capability, VS의 `SV_RenderTargetArrayIndex`·`SV_ViewportArrayIndex`는 선택 기능)은 형식 정의의 기능 요구로 후속이다. 그 밖은 컴파일 오류다.
   - `SV_VertexID`·`SV_InstanceID`에 base vertex·base instance가 들어가는지는 backend마다 다르다(D3D는 빼고 Vulkan·Metal은 넣으며, Vulkan 1.1의 draw parameters는 선택 기능이다). Graphics API가 base vertex·base instance를 0으로만 그리므로 지금은 차이가 없다. base 값을 도입하면 루트 상수로 넘기고 시스템 값에 기대지 않는다.
   - 워커가 SPIR-V 입력·출력 location을 시맨틱 번호 n으로 다시 쓴다. UE는 VS 입력(`ATTRIBUTEn`)만 시맨틱 번호로 다시 배정하고(7.1절) 단계 사이 변수는 다시 쓰지 않는다. 단계 사이 변수 재작성은 Rocks가 더하는 규칙이다. Metal의 `[[attribute(n)]]`·`[[user(locnN)]]`과 GLES의 `layout(location = n)`(SPIRV-Cross는 ESSL 310 이상에서 단계 사이 변수에도 location을 쓴다)도 이 location에서 나온다. GLES는 단계 사이 struct를 SPIRV-Cross `force_flattened_io_blocks`로 펼친다(UE와 같다).
   - 다시 쓴 SPIR-V는 `spirv-val`로 검증한다. 그래서 SPIRV-Tools는 필수다.
   - VS·PS 쌍의 연결은 DXIL·DXBC signature와 SPIR-V location을 대조해 검사한다(쿠킹 때 알려진 쌍, 파이프라인 생성 때 Development 검사).
   - 셰이더가 쓰지 않는 attribute는 리플렉션 입력 마스크로 거른다(UE `InOutMask`, `Runtime/VulkanRHI/Public/VulkanShaderResources.h:100`).
8. **좌표와 앞면.** 셰이더는 D3D 규약으로 쓰고 backend 분기를 두지 않는다(계획 11.5 "한 번만 적용").
   - Vulkan은 음수 viewport 높이(Vulkan 1.1 core)로 뒤집고, HLSL을 뺀 모든 대상(SPIR-V·MSL·ESSL)에 `-fvk-use-dx-position-w`를 넘긴다(UE와 같다).
   - `GLES31_ANDROID`는 SPIRV-Cross 업스트림 옵션 `fixup_clipspace`·`flip_vert_y`로 VS 끝에서 z를 [-1,1]로 바꾸고 y를 뒤집으며, backend는 컬 방향을 뒤집고 표시 때 Y를 뒤집는다(UE와 같다).
   - `GLES31_PC`는 두 옵션을 끄고 창·RenderTarget 모두 `glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)`으로 그린다(UE도 결과가 같다: UE가 SPIRV-Cross Options에 더한 `force_glsl_clipspace` 필드를 보고 ShaderConductor가 두 옵션을 끈다. Rocks는 업스트림 옵션을 직접 지정한다). 그래서 창에는 offscreen color target에 그린 뒤 Y를 뒤집는 blit으로 표시한다(UE 데스크톱도 표시 blit에서 Y를 뒤집는다, `Runtime/OpenGLDrv/Private/Windows/WindowsOpenGLPlatform.cpp:511–513`). 지금 OpenGLGraphics가 창에는 `GL_LOWER_LEFT`, RenderTarget에는 `GL_UPPER_LEFT`를 쓰는 규칙(EngineRuntime 10.1)은 SH2에서 이것으로 바뀐다.
   - Graphics API의 앞면 감기 방향은 framebuffer(y 아래) 기준 하나로 정한다. backend별 `frontFace` 매핑은 컬링 픽셀 대조(SH2 Vulkan·GLES 미리보기, SH5 Metal·D3D11, SH6 Android)로 확정한다.
9. **텍스처와 샘플러.** 텍스처마다 샘플러를 정하는 현재 규칙(`DeviceTypes.h:126–127`, `Sampler.h:42`)을 유지한다. HLSL은 둘을 따로 선언하므로 짝 규칙을 둔다: 텍스처 `X`는 같은 단계의 `SamplerState XSampler`와 짝이다. 짝이 없는 텍스처·샘플러와, 한 샘플러를 여러 텍스처가 쓰는 경우는 컴파일 오류다. 이 규칙으로 GLES combined sampler가 짝마다 하나로 정해진다(UE는 텍스처마다 샘플러 하나로 합치려고 SPIRV-Cross를 고쳤다, `ShaderConductor.cpp:1745–1758`). 공유·정적 샘플러(UE space1000)는 후속이다.
10. **bindless.** 기본 프로필에 넣지 않는다. 후속 bindless 프로필은 D3D12 `ResourceDescriptorHeap`(SM 6.6), Vulkan bindless(UE 5.8은 mutable descriptor 기반 set 3개와 descriptor buffer, 7.1절), Metal Shader Converter로 UE 구조를 따른다.
11. **GLES 출력과 UE 수정 도구.** UE GLES 경로는 UE가 고친 SPIRV-Tools pass 셋(`--reduce-const-array-to-struct`, `--convert-composite-to-op-access-chain-pass`, `--adv-interface-variable-scalar-replacement=skip-matrices`, `ThirdParty/ShaderConductor/ShaderConductor/External/SPIRV-Tools/source/opt/optimizer.cpp:554–562, 674–688`, 사용 `OpenGLShaderCompiler.cpp:2835–2853`)과 SPIRV-Cross 수정(UBO padding `pad_ubo_blocks`, PC 형식의 `force_glsl_clipspace` 표시, 텍스처당 샘플러 결합, ES binding 버전 조건, ESSL `offset` 생략, `ThirdParty/ShaderConductor/ShaderConductor/External/SPIRV-Cross/spirv_glsl.hpp:195–201`, `ShaderConductor.cpp:1745–1748`, `spirv_glsl.cpp:2220–2225, 2242–2245`)에 기댄다. 상수 버퍼를 `vec4` 배열로 묶는 일은 UE ShaderFormatOpenGL의 텍스트 재작성이 한다(`OpenGLShaderCompiler.cpp:1137–1240, 1797–1890`). UE 수정은 가져올 수 없으므로 Rocks는 업스트림 옵션과 워커 재작성으로 같은 결과를 낸다.
    - 클립 공간은 업스트림 `fixup_clipspace`·`flip_vert_y`(`spirv_glsl.hpp:218,221`, UE 수정 표시 밖)를 형식별로 지정한다(8항).
    - `$Globals`는 1항이 금지하고, 상수 버퍼 묶음은 워커가 4항대로 텍스트를 바꾼다. UBO padding은 진짜 UBO가 후속이라 필요 없다.
    - 샘플러 결합은 9항 짝 규칙으로 1:1이다.
    - binding은 ESSL 320에서 내되 워커가 4항대로 다시 매긴다. UE도 ESSL 320에서는 binding을 내고 ESSL 310에서만 생략한다(`spirv_glsl.cpp:2220–2225`). 업스트림 C API `spvc_compiler_unset_decoration`(`spirv_cross_c.h:913`)과 `spvc_compiler_set_decoration`으로 바꾼다.
    - UE 최적화 pass 셋의 대응(업스트림 pass 유무)은 기기 호환성 측정 S12 뒤 정한다. 업스트림으로 안 되는 것은 추적 가능한 Rocks patch(8절 4항)로 둔다.

### 7.3 시험과 완료 기준

픽셀 대조는 Sandbox self-test의 허용 오차(채널 ±8, 2%, `GraphicsSelfTest.cpp:409–415`) 안 일치를 뜻한다. 행렬 전치·attribute 뒤바뀜이 허용 오차를 크게 넘도록 fixture를 만든다.

| 시험 | 내용 | 시기 |
|---|---|---|
| (a) | 형식마다 리플렉션 오프셋 = 규칙 오프셋 = C++ 오프셋(상수 버퍼와 루트 상수), GLES는 복사 표까지 | SH2(손으로 쓴 구조체), SH3(생성 구조체) |
| (b) | 한 셰이더 안 binding 충돌 0건(GLES는 VS·PS 합쳐서, 텍스처·image·storage block 공간별로), 형식 상한 초과는 컴파일 오류, 프로필 공통 상한 초과는 쿠킹 오류, 다른 형식 정의의 상한만 바꿨을 때 키 무효화 | SH2(형식), P6b(프로필) |
| (c) | 비대칭 행렬·attribute 순서를 바꾼 정점 입력·단계 사이 변수·컬링의 픽셀 대조 | D3D12·Vulkan·GLES 미리보기는 SH2. D3D11·Metal은 SH5, Android GLES는 SH6까지 **계약(미검증)** |
| (d) | 루트 상수 128 byte 경계, D3D12 64 DWORD 예산 초과 시 CBV 테이블로 내림, GLES 루트 상수 배열 갱신 | SH2 |
| (e) | Metal 셰이더 buffer 인덱스 상한 음성 대조(컴파일), 정점 스트림 겹침 음성 대조(파이프라인 생성) | SH2(컴파일), SH5(파이프라인) |
| (f) | 5항 반례(`float a[2]; float b;`, `float3x3`, bool, 스칼라 배열, padding 없는 구조체), 상수 버퍼 안 구조체 배열·2단 중첩 구조체·버퍼 전체 load, 이름 없는 전역 uniform, `ROCKS_IMAGE_FORMAT` 없는 storage image의 컴파일 거부 | SH2 |
| (g) | location 재작성 뒤 `spirv-val` 통과, 선언 순서를 바꾼 VS·PS 쌍의 연결 거부·일치(SPIR-V·DXIL·ESSL) | SH2. D3D11 두 경로 혼합은 SH5 |
| (h) | 텍스처·샘플러 짝 규칙의 음성 대조, GLES combined sampler 이름·유닛 | SH2 |
| (i) | GLES 모바일 상한(샘플러 16, 에뮬레이션 `vec4` 합계, VS·PS storage block, varying 수, MRT 수) 초과와 한 슬롯에 타입을 섞은 상수 버퍼의 컴파일 오류, 바뀐 버퍼만 올리는지(호출 수 계측) | SH2(미리보기), SH6(기기) |

P2 완료 기준: 이 절의 규칙과 `ShaderBindingLayout` 형식이 확정되고, 위 시험이 SH2·SH3·SH5·SH6 완료 기준에 들어 있다. 6항 행렬 옵션과 8항 backend별 앞면 매핑은 측정 S5로 정하는 SH2 산출물이며 P2 완료를 막지 않는다.

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
   - `version`, 라이선스(`spdx`·고지 파일), 출처. 출처는 Rocks 도구 빌드가 게시한 archive의 URL·`sha256`과 그 빌드의 원본 태그·patch 목록이다. 설치된 SDK에서 가져오는 도구는 그 SDK의 경로 규칙을 적는다.
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
     - 버전을 고정한 Windows SDK를 설치하고, bootstrap이 그 SDK의 `Redist/D3D/x64/d3dcompiler_47.dll`을 SHA-256 검사 뒤 저장소로 복사한다(재배포 조건은 측정 S10).
     - Windows SDK는 archive가 아니라 설치기라서 다시 게시하지 않는다.
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
- **런타임 의존 파일 복사.** 도구에는 필요 없다. D3D12 Agility SDK를 고르면(결정 3) `D3D12Core.dll`을 Runtime 실행 파일 옆에 두고 `D3D12SDKVersion`·`D3D12SDKPath` export를 넣어야 하므로, 그때 Asan 런타임 복사(`ActionGraph.cs:329–337`)를 일반 `RuntimeDependencies` 필드로 넓힌다(UE `RuntimeDependencies` 대응). 이 DLL은 `kind: binary`의 별도 role(`redist`, 실행 파일 옆 복사만 허용)로 두어 위의 도구 로드 정책과 구분한다.
- **설치 SDK.** 워커와 도구 묶음 내보내기는 후속이다(지금 SDK는 C# 도구만 내보내고(`Programs/Rocks.Build.Cli/InstalledSdk.cs:132–135`) `PrebuiltLibraries`·추가 컴파일·링크 인자가 있는 모듈을 거부한다(`:69–74`)).

완료 기준:
- 깨끗한 `out/`과 저장소만으로 워커가 모든 도구를 연다(Windows·macOS·Asan). 파일 SHA가 다르면 열지 않는다.
- 현재 디렉터리·PATH에 둔 같은 이름 DLL을 열지 않는다(P10 음성 대조).
- 정책 음성 대조: Runtime 모듈의 도구 표·binary 참조, 워커 밖의 binary 로드, Release 대상의 `Developer` 모듈 도달, Release Windows 대상의 OpenGLGraphics 링크.
- SH2 뒤 Release Sandbox의 import 표에 컴파일러 DLL이 없다.

## 10. P5 에셋 너겟과 셰이더 job 키

AssetSystem A3(9절 설계)을 그대로 구현하고, 셰이더 job도 AssetSystem 9절의 **공통 키 규칙을 그대로 따른다**(2026-11-13 사용자 결정 10). 의존: A2 `ProjectLayout`(너겟 위치 `<Project>/Intermediate/AssetNuggets/`, AssetSystem 9절), A3(5.2절 N1–N3), B7(구현). 프로젝트 없이 엔진 셰이더만 컴파일할 때(Sandbox·self-test·엔진 내장 라이브러리)는 엔진 루트의 `Intermediate/AssetNuggets/`를 같은 설정 규칙으로 쓴다.

**job 키.** 메인 프로세스가 계산한다(도구를 열지 않으므로 형식별 값은 형식 정의와 도구 표에서 읽는다). 처리기 `BuildKey(KeyWriter&)`가 아래 순서·타입 태그로 기록한 바이트의 SHA-256이다. 셰이더 job에는 AssetId·Build 의존 순환이 없으므로 AssetSystem의 SCC 식을 거치지 않는다(5.2절 N1). 항목은 AssetSystem 9절 키 항목에 1:1로 대응한다(번호는 이 문서가 붙인 것이다).

| AssetSystem 9절 항목 | 셰이더 job |
|---|---|
| 1 `"RocksAssetNugget"` 형식 버전 | 같다 |
| 2 처리기 Guid·처리기 버전 Guid | 셰이더 job 처리기 |
| 3 외부 도구의 이름·버전·설정 | 형식 id와 형식 정의 파일(`Engine/Config/ShaderFormats/<id>.json`) SHA-256, **형식 정의가 선언한 모든 도구**의 도구 표 SHA-256 목록(정적 목록. 예: `D3D11_SM5`는 FXC 실패 뒤에야 DXC·SPIRV-Cross 경로를 타므로 메인 프로세스가 미리 알 수 없어 FXC·DXC·SPIRV-Cross·SPIRV-Tools를 모두 넣는다), SPIRV-Reflect 원본 manifest SHA-256, Apple 도구 지문(워커가 잰 값, 11절). 도구 빌드의 원본 태그·patch·컴파일러가 바뀌면 바이너리 SHA가 바뀐다 |
| 4 결정적 수학 버전 | `rocks::deterministic::Version` |
| 5 툴체인 지문 | `rocks::build::ToolchainFingerprint()`(메인 프로세스와 워커는 같은 빌드에서 나온다) |
| 6 에셋 TypeId | 단계·엔트리 이름 |
| 7 입력 bulk SHA-256 | 전처리 결과 SHA-256(가상 경로만 담긴 `#line`) |
| 8 결과에 영향을 주는 설정 전체 | 컴파일 옵션(출력에 영향을 주는 정의·플래그의 정렬 목록, 기본값 포함), 파라미터 레이아웃 서명(P7 전에는 손으로 쓴 레이아웃 표의 SHA-256, 7.2절 5항. P7 뒤에는 13절) |
| 9 대상 프로파일 id와 출력 설정 전체 | 같다. 여기에 프로필 공통 상한 계산에 든 형식 정의들의 SHA-256 정렬 목록을 더한다(7.2절 4항. 다른 형식의 상한만 바뀌어도 키가 바뀐다). 프로젝트 프로필이 없는 컴파일(엔진 내장 라이브러리, Development 필요 시 컴파일)은 호스트별 엔진 컴파일 프로필(`Engine-Windows`·`Engine-MacOS`, 12.1절)을 쓴다 |
| 10 의존 목록 | 없다. include 폐포는 전처리 결과에 들어 있다 |

Rockpile 개정(키의 에셋 너겟 세대 Guid 등, 5.2절 N3)이 AssetSystem에 반영되면 같은 자리에 넣는다.

공통 규칙을 따른 결과:
- 툴체인 지문에 대상 triple(아키텍처·OS·배포 대상)이 들어가므로 Windows x64와 macOS arm64는 같은 셰이더도 키가 다르다. 셰이더 너겟은 호스트 사이에 공유하지 않는다(계획 §2.5 "host가 다르면 우선 분리"와 같은 방향). 키가 달라도 결정성 규칙(아래)은 그대로 적용한다.
- 한 호스트만 만들 수 있는 형식(Windows의 DXBC, 15절 결정 5의 metallib)은 다른 호스트에서 해당 형식 job이 `UnsupportedCapability`로 실패한다(계획 §27.9). 원격 워커(계획 §2.5 "macOS는 같은 action을 Windows worker에 전달", §27.7)가 생기면 그쪽으로 보낸다. 이때 키는 요청한 호스트의 것이다.
- 프로파일의 출력 설정(텍스처 LOD 그룹, 셰이더 형식 목록 등)이 바뀌면 그 프로파일의 셰이더 키가 모두 바뀐다. 반대로 셰이더 형식 목록을 바꾸면 같은 프로파일의 텍스처 키도 바뀐다. 이것은 공통 규칙의 비용으로 받아들인다.

Apple 도구 지문: `metal -v --target=…`의 버전·대상 문자열(UE도 이 출력을 해석한다, `MetalShaderFormat.cpp:575–660`, 호출 `:882–920`)과 `metal`·`metallib` 실행 파일의 SHA-256이다.

참고로 UE 5.8의 셰이더 job 키(BLAKE3)는 엔진을 빌드한 컴파일러·호스트를 넣지 않는다. 캐시 버전 GUID(`Runtime/RenderCore/Private/ShaderCompilerCore.cpp:5`), 형식 버전, 대상, 엔트리, 컴파일 환경, 전처리 결과, 루트 파라미터 레이아웃 서명, `PreferredShaderCodeFormat`·`bBindlessEnabled`·셰이더 파이프라인 여부·메타데이터 지시문 등을 넣는다(`ShaderCore.cpp:4612–4671`, `:4627`·`:4634`·`:4636`·`:4645–4652`). 형식 버전에 넣는 도구 버전 해시(DXC는 `dxcompiler.dll`·`dxil.dll` 이미지의 xxhash, ShaderConductor도 같은 방식)는 Windows가 아니면 상수라(`DXCWrapper.cpp:28–67, 109–116, 137–139`, `:64–66`), macOS에서 만드는 Metal·Vulkan·GL 형식은 도구가 바뀌어도 UE 키가 바뀌지 않는다(`Developer/Apple/MetalShaderFormat/Private/MetalShaderFormat.cpp:117,560`, `Developer/VulkanShaderFormat/Private/VulkanShaderFormat.cpp:65,117`, `ShaderFormatOpenGL.cpp:145`). Rocks는 모든 호스트에서 도구 SHA를 키에 넣고, 공통 규칙을 우선해 툴체인 지문도 넣는다.

**값과 결정성.** AssetSystem §10 결정성 규칙(같은 도구 빌드에서 호스트 무관하게 같은 바이트)을 셰이더에도 적용한다.

- **호스트 사이 동일.** 두 호스트가 모두 만드는 형식(DXIL·SPIR-V·ESSL·MSL 텍스트)은 같은 원본 태그·patch로 빌드한 도구에서 Windows와 macOS가 같은 바이트를 내야 한다. 도구 바이너리는 호스트마다 다르므로 SH0에서 잰다(측정 S3). 다르면 원인(도구 빌드 차이, 호스트 libm, 출력 형식화)을 고칠 방법을 정하기 전에는 그 형식을 공유 쿠킹에 쓰지 않고, 방법은 사용자 결정으로 올린다. 한 호스트만 만드는 형식(DXBC·metallib)은 해당 없음이다.
- 값은 바이트코드, 정규화된 바인딩 layout, GLES 바인딩 머리·복사 표, 진단, 디버그 심볼을 부분으로 나눈다(`.rnug` 부분 구조, 5.2절 N2).
- 경로 누출 금지: 입력은 메모리 버퍼와 가상 이름으로 넘기고 `#line`에는 가상 경로만 둔다. 디버그 정보는 별도 부분으로 빼고 Apple 도구에는 prefix map을 쓴다. "작업 디렉터리를 바꿔도 같은 바이트" 시험을 둔다.
- SPIRV-Cross의 실수 출력(`%.32g`)에서 locale 소수점은 SPIRV-Cross가 보정하므로(업스트림 `spirv_common.hpp`의 `fixup_radix_point`) CRT `%g` 구현 차이만 남는다. 같은 호스트 키 안에서도 OS의 CRT 버전은 다를 수 있다. 워커를 C locale로 고정하고, 극값(비정규수·최대값·17자리 이상 필요한 값) fixture로 출력이 같은지 시험한다. 다르면 추적 가능한 SPIRV-Cross patch(8절 4항)로 고정한다. MSL·ESSL·HLSL 5.0 모두 해당한다.
- **상수 접기.** DXC(LLVM)와 SPIRV-Tools 최적화기가 초월 함수 상수식을 호스트 libm으로 접으면, 같은 호스트 키 안에서도 CPU(예: UCRT의 FMA3 분기, [핵심 데이터 기반](CoreDataFoundations.ko.md))에 따라 바이트가 다를 수 있다. SH0에서 상수 초월 함수 fixture를 FMA3 유무와 OS 버전을 바꿔 비교한다(측정 S3). 다르면 결정성 규칙을 지킬 방법(도구 빌드 patch, 해당 최적화 끄기 등)을 정하기 전에는 그 형식을 공유 캐시에 올리지 않고, 방법은 사용자 결정으로 올린다.
- **dxil 서명.** UE 5.8은 DXC 1.8.2403 기반 수정본을 쓰고 `dxil.dll`을 명시적으로 열며, 이 DLL이 없으면 서명되지 않은 DXIL이 조용히 나온다고 적는다(`DXCWrapper.cpp:100`, `:111–113`). DXC 1.8.2505부터는 외부 `dxil.dll`을 찾지 않고 내장 validator가 서명한다(업스트림 DXC `docs/ReleaseNotes.md`, 1.8.2502에서 validator hash 공개. UE 동봉본(1.8.2403)으로는 확인할 수 없어 측정 S1로 확인한다). Rocks는 DXC를 1.8.2505 이상으로 고정하고 `dxil.dll`을 쓰지 않는다. 결과 DXIL container 헤더의 hash digest가 0이거나 preview용 우회 값(`PreviewByPassHash`, 0x02×16, 업스트림 `include/dxc/DxilContainer/DxilContainer.h`)이면 job을 실패시킨다. macOS DXC에서도 서명되는지 SH0에서 실측하고(측정 S1), 서명되지 않으면 `D3D12_SM6`의 생산 호스트를 형식 정의에서 Windows로 고정한다.

완료 기준: A3 시험(적중·무효화·손상·동시 기록·GC)에 더해 다음을 시험한다.
- job 키의 입력별 무효화: 옵션, 형식 정의, 도구 SHA, 툴체인 지문, 결정적 수학 버전, 레이아웃 서명, 프로파일 출력 설정을 하나씩 바꾼다. 경로·시각·locale만 바꾸면 적중한다.
- 만들 수 없는 형식의 `UnsupportedCapability`(macOS의 DXBC).
- 실수 출력·상수 접기 fixture.

## 11. SH1 워커 통신

UE 5.8의 방식(파일 기반):
- 메인 프로세스가 `ShaderCompileWorker <작업 디렉터리> <부모 PID> <스레드 번호> <입력 파일> <출력 파일> -communicatethroughfile -TimeToLive=<초>`로 워커를 띄운다(`Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp:1687–1696`, 워커 쪽 `Programs/ShaderCompileWorker/Private/ShaderCompileWorker.cpp:642,647`).
- 입력 파일은 입력 형식 버전(`ShaderCompileWorkerInputVersion`, `Runtime/RenderCore/Public/ShaderCompilerCore.h:35`)으로 시작하고, 압축 형식 이름과 (압축했으면) 압축된 본문이 뒤따르며, 형식별 버전 표는 본문 안에 있다. 워커는 둘 중 하나라도 자기 것과 다르면 끝난다(`Runtime/RenderCore/Private/ShaderCompileWorkerUtil.cpp:942–955, 970–1017`).
- 워커는 입력 파일을 읽고 지우며(`ShaderCompileWorker.cpp:407–419`), 결과를 임시 파일에 쓴 뒤 출력 이름으로 옮긴다(`:425–433`, `:227–231`). 그래서 메인 프로세스가 덜 쓴 출력을 읽지 않는다.
- 부모 PID로 부모 생존을 확인하고(`:460–469` macOS·Linux, `:483–507` Windows), 새 입력이 TimeToLive(기본 20초, 빌드 머신 600초, `ShaderCompiler.cpp:303–304`) 동안 없으면 끝난다.

Rocks 규칙:
1. **파일 기반**(UE와 같다). 작업 디렉터리는 `<Project>/Intermediate/ShaderWorkers/<부모 PID>/<워커 번호>/`이다(A2, 프로젝트가 없으면 엔진 루트). 메시지는 Core Record 기반 schema로 크기·정렬·endianness가 정의된 직렬화 메시지다(계획 §2.5 "프로세스/worker 경계에는 … 직렬화 메시지 또는 검증된 C ABI만 사용한다").
2. **입력 머리.** 메시지 형식 버전, 형식별 ShaderFormat 처리기 버전 Guid 표, 도구 표 SHA. 워커는 자기 것과 다르면 정해진 종료 코드로 끝내고, 메인 프로세스는 "워커를 다시 빌드하라"는 진단을 낸다.
3. **도구 조회.** 메인 프로세스는 세션마다 처음 한 번 워커에 도구 조회 요청을 보낸다. 워커는 저장소 도구의 SHA가 도구 표와 같은지와 Apple 도구 지문(`metal -v` 문자열, `metal`·`metallib` SHA-256)을 돌려준다. 메인 프로세스는 도구를 직접 실행하지 않고 이 값을 job 키 3항(10절)에 쓴다. 도구 조회 결과가 세션 중에 바뀌면(시스템 도구 갱신) 남은 job을 실패시키고 다시 조회한다.
4. **job.** 키, 형식 id, 단계, 엔트리, 전처리된 원문(가상 경로 `#line`), 컴파일 옵션, 파라미터 레이아웃 표(7.2절 5항)다. 워커는 원본 파일 경로를 받지 않는다(10절 경로 누출 금지). 한 입력 파일에 job 여러 개를 묶으며, 묶음 크기(job 수·원문 바이트 상한)는 측정 S8로 정해 설정으로 둔다.
5. **출력.** 임시 파일에 쓰고 no-replace `Move`(A0-P)로 출력 이름에 옮긴다. 입력 파일은 읽은 뒤 지운다. 디버그용으로 입력을 남기는 선택(UE `-KeepInput`)을 둔다.
6. **부모 감시.** Windows는 P1 job object가 부모 비정상 종료 때 워커를 끝낸다. macOS는 프로세스 그룹이 부모 종료에 따라 끝나지 않으므로 워커가 입력 pipe EOF(P1 lifeline)로 부모 종료를 알고 끝난다. 끝나기 전에 자기가 띄운 자식(Apple `metal` 등, P1에서 자기 프로세스 그룹을 가진다)을 `Process::Close`로 끝낸다. 워커가 강제 종료되어 이 정리를 못 하면 남은 자식은 `RunProcess` 시간 제한으로 끝난다. PID 확인(`IsProcessRunning`)은 PID 재사용 때문에 보조로만 쓴다.
7. **유휴 종료.** 새 입력이 TimeToLive(기본 20초, CI 600초, UE 값) 동안 없으면 끝난다.
8. **캐시.** 워커는 너겟을 읽거나 쓰지 않는다(`Rockpile.ko.md` 4절. 계획 §27.7 개정은 Rockpile 17절에 있다). 메인 프로세스가 조회·게시한다.
9. **실패.** 워커가 비정상 종료하면 그 묶음의 job을 종료 종류·코드와 함께 실패로 보고하고 다른 묶음은 계속한다.

## 12. P6a·P14·P6b 형식 정의·셰이더 타입·TargetProfile·쿠킹 매니페스트

엔진 컴파일 프로필은 형식 정의와 함께 P6a(12.1절)가, TargetProfile 필드는 A4가 쓰는 프로필 파일에 P6b(12.3절)가 더한다.

### 12.1 P6a 형식 정의와 엔진 컴파일 프로필

- **형식 정의.** `Engine/Config/ShaderFormats/<id>.json`에 둔다: 이 형식이 쓰는 도구 목록(10절 키 3항의 정적 목록), 언어 버전 기본값, 컴파일러(DXC·FXC·SPIRV-Cross 경로), 셰이더 모델·target env·ESSL 버전, 바인딩 규약 버전, ShaderFormat 처리기 버전, 상한(7.2절 4항)·지원 기능, 생산 호스트 규칙. UE에서는 기능·형식 이름 부분이 `DataDrivenPlatformInfo.ini`의 `[ShaderPlatform]`(`Language`·`MaxFeatureLevel`·`ShaderFormat`·기능 플래그, `Engine/Config/Windows/DataDrivenPlatformInfo.ini:78–139`)에 있고, SM 선택은 ShaderFormat 코드에 있다(`ShaderFormatD3D.cpp:102–150`, FXC·DXC 분기 `D3DShaderCompiler.cpp:1520–1522` `DoesShaderModelRequireDXC`). Rocks는 둘 다 형식 정의에 둔다. 형식 정의 SHA-256이 job 키에 들어간다(10절). 형식 정의에는 결정 2(언어 버전 기본값)·3(셰이더 모델)·5(생산 호스트)가 들어가므로 P6a는 그 결정 뒤다(15절 기한).
- **엔진 컴파일 프로필.** 프로젝트 프로필 없이 하는 컴파일(엔진 내장 라이브러리, Development 필요 시 컴파일)이 쓰는 호스트별 프로필이다. 런타임 backend 선택이 없는 컴파일 전용이라 TargetProfile과 스키마를 나눈다: `Engine/Config/ShaderFormats/Engine-<호스트>.json` = `{ "id", "formats": [...] }`. `Engine-Windows`는 `D3D12_SM6`·`D3D11_SM5`·`VULKAN_SM5`·`GLES31_PC`·`GLES31_ANDROID`, `Engine-MacOS`는 `METAL_SM5`·`GLES31_ANDROID`다. 그 호스트에서 쓰거나 그 호스트가 빌드하는 대상(Android 포함)이 쓰는 형식 가운데 그 호스트가 만들 수 있는 것만 넣는다(10절, 15절 결정 5에 따라 바뀐다). job 키 9항과 프로필 공통 상한이 이 프로필에서 나온다. SH1이 쓰므로 P6a에서 만든다.
- 완료 기준: 스키마 검증(모르는 속성 거부, 도구 목록이 도구 표에 있는지, 엔진 컴파일 프로필의 형식이 형식 정의에 있는지), 형식 정의 변경의 키 무효화(10절).

### 12.2 P14 셰이더 타입·퍼뮤테이션·라이브러리 형식

- **셰이더 타입.** 이름, `.rsf` 가상 경로, 엔트리, 단계, 요구 기능(2절), 퍼뮤테이션 도메인, 형식·퍼뮤테이션 거르기 함수를 가진다. 정적 등록 표로 모은다(UE `IMPLEMENT_GLOBAL_SHADER`, `Runtime/RenderCore/Public/GlobalShader.h:433`). 등록 표의 순서는 결과에 영향을 주지 않는다(이름으로 정렬해 쓴다).
- **퍼뮤테이션.** 차원(bool·정수 범위·열거)을 선언하고 차원 값의 조합에서 퍼뮤테이션 ID를 계산한다. 각 차원은 전처리 정의 하나로 원문에 들어간다(UE `SHADER_PERMUTATION_BOOL`·`TShaderPermutationDomain`, `ShaderPermutation.h:272,629`). 형식과 퍼뮤테이션마다 컴파일 여부를 거른다(UE `ShouldCompilePermutation`, `GlobalShader.h:100–102`).
- **라이브러리 형식.** 머리(형식 버전, 형식 id, 항목 수), 항목 표(셰이더 타입 이름 hash·퍼뮤테이션 ID·단계 → 코드 SHA-256), 코드 표(코드 SHA-256 → 오프셋·크기, 같은 코드는 한 번만), 코드 부분(바이트코드·바인딩 layout·GLES 바인딩 머리). UE `FSerializedShaderArchive`(셰이더 맵 hash 표·셰이더 hash 표·항목 표, `Runtime/RenderCore/Public/ShaderCodeArchive.h:206–230`)에 해당한다.
- **런타임 조회.** (셰이더 타입, 퍼뮤테이션 ID, 단계)로 찾는다. 등록된 타입이 요구하는 퍼뮤테이션이 라이브러리에 없으면 `IncompatibleCook`이다.
- 완료 기준: 퍼뮤테이션 ID의 결정성(등록 순서와 무관), 같은 코드의 중복 제거, 라이브러리 왕복·손상 검출(`CorruptEntry`), 빠진 항목 조회의 `IncompatibleCook`.

### 12.3 P6b TargetProfile·쿠킹 매니페스트·엔진 내장 라이브러리

- **TargetProfile**(AssetSystem 10절)은 backend와 형식 id 목록만 갖는다. 언어 버전·target env·생산 호스트·장치 요구를 프로필에 두면 출처가 둘이 되기 때문이다. `graphicsBackends`는 런타임이 시도하는 순서다. `shaderFormats`는 backend마다 선호 순서가 있는 형식 목록이며(형식의 backend는 형식 정의가 정한다), 런타임은 장치가 지원하는 첫 형식을 고른다(UE `D3D12TargetedShaderFormats` 같은 backend별 형식 목록, `Runtime/RHI/Private/Windows/WindowsDynamicRHI.cpp:624–626`). 그래서 장치 요구 수준이 다른 형식(2절)이나 결정 3의 D3D12 형식 두 개를 함께 둘 수 있다. 목록에 형식이 없는 backend, 목록에 없는 backend의 형식, 형식 정의의 backend가 키와 다른 형식은 프로필 검증 오류다. backend 이름은 구현 여부와 무관한 고정 어휘다(`D3D11`은 SH5에서 구현된다). 런타임은 구현되지 않았거나 장치가 없는 backend를 `UnsupportedCapability`로 건너뛰고 다음 backend를 시도한다.

```json
"graphicsBackends": ["D3D12", "D3D11", "Vulkan"],
"shaderFormats": { "D3D12": ["D3D12_SM6"], "D3D11": ["D3D11_SM5"], "Vulkan": ["VULKAN_SM5"] }
```

- 엔진 기본 프로필: `Windows`(위), `MacOS`(`Metal`: `METAL_SM5`, 새로 더함), `Android`(`OpenGL`: `GLES31_ANDROID`, SH6에서 `Vulkan`: `VULKAN_ANDROID`를 앞에 더함). 현재 엔진 기본은 `Windows`·`Android`뿐이다(AssetSystem 10절). backend 이름 `OpenGL`은 OpenGL ES backend다(UE `EWindowsRHI::OpenGL`과 같은 이름).
- 엔진 컴파일 프로필은 12.1절(P6a)이 정한다.
- `GLES31_PC`는 출시 프로필과 엔진 내장 라이브러리에 넣지 않는다. Development 실행 파일이 필요할 때 컴파일한다(SH1). UE도 쿠킹 데이터를 쓰는 실행 파일에는 이 플랫폼을 등록하지 않는다(2절). Release Windows 실행 파일은 OpenGLGraphics를 링크하지 않는다(9절 정책).
- **쿠킹 산출물.** (프로필, 형식)마다 셰이더 라이브러리 파일 하나를 만든다. 파일 이름은 패키지처럼 내용 주소(AssetSystem 4.2절 규칙, 형식 id와 내용 SHA-256)라 게시된 파일을 덮어쓰지 않는다. 게시와 정리는 `Packages/`와 같다: staging → no-replace `Move` → `SyncDirectory` → 매니페스트 `Replace`, 전체 쿠킹은 새 매니페스트가 가리키지 않는 라이브러리 파일을 지우고 부분 쿠킹은 지우지 않는다(AssetSystem 10절). UE는 라이브러리(Global·프로젝트·플러그인/청크)×형식마다 `ShaderArchive-<Library>-<Format>-<Platform>.ushaderbytecode`를 쿠킹하고(`Runtime/RenderCore/Private/ShaderLibrary/ShaderCodeLibraryUtilities.cpp:84–96`, `<Format>-<Platform>` 문자열 `ShaderCodeLibrary.cpp:1482–1488`), 배포는 IoStore나 네이티브 라이브러리로 연다(ushaderbytecode는 배포 형식이 아니라는 주석, `ShaderLibraryInstance.cpp:27–43`). Rocks도 라이브러리 파일은 쿠킹 단위이고, 배포 컨테이너로 옮기는 일은 패키징 단계가 맡는다. 라이브러리 분할(플러그인·청크)은 후속이다.
- **엔진 내장 셰이더 라이브러리.** 엔진 셰이더(`Engine/Shaders`)의 라이브러리는 엔진 설치물을 만드는 단계(설치 SDK 내보내기·패키징)에서 `shader-compile --engine` 명령으로 그 호스트의 엔진 프로필 형식(`GLES31_PC` 제외)마다 만든다. 빌드 그래프 action이 아니다: Rocks.Build에는 빌드한 프로그램을 실행하는 action이 없다(action 종류는 Compile·Archive·Link·Copy, `ActionGraph.cs`). 그 호스트가 만들 수 없는 형식은 다른 호스트의 설치물 단계가 만든다(macOS는 DXBC를 만들지 않는다. 계획 §27.9는 호스트가 지원하는 target의 로컬 빌드가 되어야 한다고 요구한다). 설치물에서는 프로젝트·너겟·네트워크 없이 연다(계획 9.6, 5.2절 K3). 개발 트리에서는 Development 필요 시 컴파일이 같은 역할을 한다. UE의 Global 라이브러리에 해당한다. 프로젝트 쿠킹은 프로젝트·플러그인 셰이더 라이브러리만 만든다.
- 쿠킹 매니페스트 v1은 AssetId 단위라서 v2에 "에셋이 아닌 산출물" 항목(이름, 형식 id, 파일 SHA-256·크기, cookKey)을 더한다. 라이브러리 cookKey 입력은 포함된 job 키 정렬 목록과 라이브러리 형식 버전이다. 게시는 기존 쿠킹 게시와 같다(staging → `SyncDirectory` → `Replace`).
- 런타임은 실행 장치의 backend에 맞는 형식의 라이브러리만 연다. 없거나 필요한 셰이더가 빠졌으면 `IncompatibleCook`이다.

완료 기준: 프로필 스키마 검증(모르는 속성 거부, 프로필에 형식 옵션을 적으면 오류, backend·형식 목록 검사), 장치 기능에 따른 형식 선택, 프로필 공통 상한 초과의 쿠킹 오류, 매니페스트 v2 왕복, 형식이 빠진 쿠킹을 연 런타임의 `IncompatibleCook`, 프로젝트 없이 엔진 내장 라이브러리를 여는 시험.

## 13. P7 MetaCompiler 셰이더 파라미터

15절 결정 4의 권장안(MetaCompiler 방식)을 고를 때의 내용이다. 바탕인 A0-B(B1–B8)는 구현되었다.

- **선언.** `ROCKS_SHADER_PARAMETERS(Name)` 구조체 종류를 MetaCompiler에 더한다. 파서·descriptor는 B4 `ROCKS_STRUCT`([리플렉션 기반](ReflectionFoundations.ko.md) 4절)를 바탕으로 한다. 멤버는 ShaderCore 셰이더 전용 정렬 타입(7.2절 5항)과 셰이더 자원 값 타입(텍스처·샘플러·버퍼 SRV·UAV)만 받고, 다른 타입(Core `Matrix4`·`Vector3` 포함)은 MetaCompiler 오류다.
- **identity와 등록.** 공통 규칙대로 셰이더 파라미터 구조체도 identity 명세(`<Module>.rocksids.json`)에 항목을 두고, `ROCKS_STRUCT`와 같은 경로로 `TypeSchema`·TypeRegistry에 등록한다. 셰이더 파라미터 종류는 그 위에 허용 타입 제약과 HLSL 생성만 더한다. 셰이더 자원 멤버를 담을 리플렉션 값 종류는 `ValueKind`에 더한다. `ValueKind`는 닫힌 열거라 값을 덧붙이고 TypeRegistry 경계를 올리는 방식이다(ReflectionFoundations 3절).
- **생성물.**
  - C++: 멤버별 offset·크기 static_assert, `ShaderBindingLayout`과 대조할 레이아웃 표.
  - HLSL: `<out>/Generated/<Module>/Shaders/<Name>.rsh`(기존 `<out>/Generated/<Module>` 규칙). 이름 있는 `cbuffer`로 낸다(7.2절 1항). 가상 경로 `/Engine/Generated/<Module>/<Name>.rsh`(플러그인·프로젝트는 각자의 `Generated`)로 연결하며, UE `/Engine/Generated/`에 해당한다(`ShaderCore.cpp:2490,3081,3096`). 생성 HLSL은 셰이더 맵 키의 include 폐포에 들어간다. 설치 SDK로 내보내는 경로는 후속이다.
- **레이아웃 서명.** 정규화한 레이아웃(멤버 이름·타입·offset·크기, 자원 종류·순번)의 SHA-256이다. job 키 8항(10절)이다.
- `DrawConstants`는 이 종류로 옮긴다.

완료 기준: MetaCompiler 음성 대조(금지 타입), identity 명세 검증(일반 구조체와 같은 규칙), 7.3절 시험 (a)·(f)를 생성 구조체로 통과.

## 14. P8 다른 문서 개정

이 문서 확정과 함께 아래를 고친다. AssetSystem 행은 해당 단계 착수 전에 먼저 고친다(6절 2항). 다른 세션이 문서를 계속 고치므로 위치는 절 번호와 인용 문구로 적는다(줄 번호는 2026-11-14 HEAD 기준 참고값).

| 문서 | 위치 | 현재 | 개정 |
|---|---|---|---|
| EngineImplementationPlan | 서문(:27), §2.2 그래픽 행(:122) | "D3D11/D3D12/Vulkan/Metal/OpenGL", 공통 material IR과 backend별 shader toolchain | OpenGL을 OpenGL ES(Android ES 3.2, Windows 개발 미리보기)로. HLSL 단일 원문(Material IR은 HLSL을 만드는 앞단). "5개 backend"·"5개 RHI"·"5종 RHI" 표현(:11·:185·:203·:208·:1101·:1121·:1128·:1376·:1381·:2521·:2532·:2533·:2770)은 개수가 같으므로 그대로 두고 서문에서 OpenGL이 OpenGL ES임을 한 번 밝힌다 |
| 〃 | §2.5 표(:187–193) | D3D11 "HLSL→FXC/D3DCompile→DXBC", Metal "typed IR→MSL"(:192), OpenGL "IR→GLSL"(:193), Vulkan Windows/Linux | 2절 표의 경로. OpenGL 행은 OpenGL ES(ESSL 320). Linux Vulkan 검증은 이 문서 범위 밖이다(Linux는 서버·headless 대상이고 지금 빌드 대상이 아니다, `NativeBuildModels.cs:4`) |
| 〃 | §2.5(:197) | Windows용 Metal 제작 도구가 있으므로 Metal cook을 macOS 전용으로 단정하지 않는다 | 유지한다(15절 결정 5와 같은 방향) |
| 〃 | §3.1(:234), §3.2(:262), §14.6(:1276), §14.9(:1341), §27.7(:2287) | ShaderWorker | ShaderCompileWorker(§14.6의 의존 대상은 `Developer/ShaderCompiler` 라이브러리) |
| 〃 | §3.2(:259) | `Engine/Source/ThirdParty/ Adapters` | 외부 라이브러리 원본·manifest·바이너리 manifest·adapter(8절) |
| 〃 | §11.5(:1107) | Vulkan "1.2급" | `VULKAN_SM5`는 target env 1.1(UE `SF_VULKAN_SM5`와 같다, 2절) |
| 〃 | §11.5(:1108·:1109·:1117) | Metal "typed IR→SPIR-V/MSL 변환→Apple offline library", OpenGL "core 4.5급; IR→SPIR-V→GLSL", GL clip 규약 | Metal은 HLSL→SPIR-V→MSL. OpenGL ES 3.2(Android)와 Windows 개발 미리보기(GL 4.3 core 이상 + clip control), HLSL→SPIR-V→ESSL 320, 상수 버퍼 에뮬레이션. clip은 7.2절 8항 |
| 〃 | §14.1(:1212), §14.7(:1319) | DXBC·DXIL·SPIR-V·MSL·GLSL 경로, OpenGL의 target GLSL compile/link와 driver pipeline compilation | GLSL을 ESSL로. 쿠킹된 ESSL의 program link, 프로그램 바이너리 캐시는 후속 |
| 〃 | §27.9(:2315) | `CapabilityUnavailable` | `UnsupportedCapability`(AssetSystem A1의 Core ErrorCode) |
| 〃 | S71(:2965) | SPIRV-Cross "MSL·GLSL" | "MSL·ESSL·HLSL 5.0" |
| 〃 | §2.1(:114) | ThirdParty의 예외·RTTI 옵션은 별도 빌드, 예외가 엔진 경계를 넘지 않게 변환 | 유지한다(8절 4항의 오프라인 도구 빌드가 이 규칙의 사례) |
| CoreObjectInputDetailedPlan | :1301 | 5개 graphics API, D3D11/D3D12/Vulkan/OpenGL/Metal | OpenGL을 OpenGL ES로 |
| EngineRuntime | :3·:14·:38·:39·:220·:222·:236·:238·:239·:243·:255·:266·:274 | 범위 "D3D12·OpenGL로 화면에", OpenGL 4.5 core, Vulkan·Metal "자리만", `Backend{D3D12, OpenGL, Vulkan, Metal}`, `CreateDevice`(코드에 없음), 런타임 `D3DCompile`·GLSL 4.50, "다른 backend 구현은 범위 밖", `--graphics=d3d12\|opengl`(코드에 없음), OpenGL self-test, "Vulkan은 미구현", 창 `GL_LOWER_LEFT`·RenderTarget `GL_UPPER_LEFT` 원점 규칙(:238), 디버그 레이어(:239) | OpenGLGraphics를 OpenGL ES backend(Windows 미리보기·Android, P11)로 다시 정의하고(원점은 7.2절 8항, 디버그 레이어 규칙은 Vulkan 검증 레이어에도 적용), 셰이더 원문 규칙을 이 문서 참조로 바꾼다. VulkanGraphics 절(P9, SH2 전)과 파이프라인 래스터(컬링·앞면)·깊이 상태(지금 `PipelineDesc`에 없다, `DeviceTypes.h:145–148`, SH2), D3D11·Metal 절(P15, SH5 전), `Backend`에 D3D11 추가(`DeviceTypes.h:14–21`). 코드에 없는 `CreateDevice`·`--graphics` 서술 정리 |
| AssetSystem | 1절 A0-G(:15), A1(:16), A2(:17) | OpenGL swizzle·샘플러 포함, ErrorCode 추가, `ProjectLayout`은 이름만 | G1–G3(5.2절), A1 ErrorCode를 셰이더에도 쓴다, A2 `ProjectLayout`에 셰이더 디렉터리·`ShaderWorkers` 작업 디렉터리·프로젝트 없는 엔진 위치 규칙(5.2절) |
| 〃 | 2절 사전 조사 "VFS(APK) → 후속" | APK 안 데이터 읽기 후속 | P13(SH6)의 선행 작업으로 단계를 정한다 |
| 〃 | 4.6절 진단 표(:115) | `IncompatibleCook`은 부분 쿠킹 병합 불가·런타임 프로파일 불일치 | 필요한 셰이더 형식·셰이더가 쿠킹 결과에 없음을 더한다(2절, 12절) |
| 〃 | 9절(:267–292) | 에셋 중심 키(TypeId·입력 bulk·의존 목록, SCC 식) | N1–N3(5.2절) |
| 〃 | 10절 TargetProfile(:296)·cookKey(:299)·매니페스트(:307)·결정성(:311) | 그래픽 backend 단일 항목, 기본 프로필 Windows·Android, 매니페스트 v1 | K1–K3(5.2절), 12절 `graphicsBackends`·`shaderFormats`·`MacOS` 프로필. 결정성 규칙은 그대로 셰이더에 적용한다(10절) |
| ReflectionFoundations | 3절 값 종류, 4절 struct | — | 셰이더 파라미터 종류가 `ROCKS_STRUCT` 경로를 쓰고 셰이더 자원 값 종류를 더한다(13절, 결정 4를 고를 때) |
| PlatformLayer | P1 표 "동적 모듈", 모바일 절 | 동적 모듈 미구현, Android는 시험 실행만 | P10 `DynamicLibrary` 절, P13 Android 실행 기반, P12 macOS 창(PlatformWindow) |
| RepositoryLayout | 트리(:14·:29)·:52–56 | Engine에 Shaders·Config 없음, ThirdParty는 Core만 원본(:14 목록도 낡음) | 3절 구조와 8절 루트·`binary`·`headers` 종류 |
| CompilerDiagnostics | :9·:25·:29·:36 | 생성 파일도 경고 검사, 외부 원본도 같은 경고 프로필로 컴파일, Core 루트만, "이 절 밖의 경고 억제 예외는 없다" | 15절 결정 7의 답에 따라: 두 루트, Rocks가 컴파일하지 않는 미리 빌드된 바이너리(오프라인 도구 빌드 포함) 승인 절과 그 공개 헤더의 경고 0 조건, 셰이더 경고 절(작성자 원문과 SPIRV-Cross가 다시 쓴 중간 원문 모두 경고를 오류로) |
| Compression | 3.1·3.3·3.5 | Core 루트, `.c` 번역 단위만 | Engine 루트와 `kind: binary`·`headers` 언급(규칙 자체는 그대로) |
| CoreDataFoundations | 1.2절(:20) | ThirdParty `kind`는 `code`·`data` | `binary`·`headers`를 더한다(8절) |
| 정책 검사(코드) | `PolicyChecks.cs` `PythonFreeMainPath`(:313–349) | `Programs`·`Build`·`.github`의 Python 호출 금지 | 오프라인 도구 빌드 workflow를 명시 제외(8절 4항, P3 구현 때) |
| 코드(SH2 때) | `Engine/Source/Runtime/Graphics/Graphics.Build.cs:4–5`, `Programs/Sandbox/Sandbox.Build.cs:4·14`, `D3D12Graphics.Build.cs:21–22`, `DeviceTypes.h:14–15` | OpenGL·Vulkan·Metal 자리 설명, `d3dcompiler` 링크 | VulkanGraphics 의존 추가, OpenGL ES 설명, `d3dcompiler` 제거 |

## 15. 결정 필요 사항

남은 결정의 기한(이 결정이 없으면 시작할 수 없는 작업 직전): 결정 7·11은 셰이더 구현 착수(A6 완료) 전, 결정 6은 SH1 전(측정 없이 정하고, 자체 구현의 정합성은 SH1 완료 기준인 S7로 확인한다), 결정 2·3·5는 형식 정의에 들어가므로 SH0 측정 S2·S9 뒤 P6a 전, 결정 4는 P14 전이다. 이 문서의 "확정"은 측정에 기대지 않는 내용의 승인이며, 측정 뒤에 정하는 결정과 그에 걸린 14절 개정은 해당 기한에 한다.

1. **확장자** — **결정됨(2026-11-13): `.rsf`(엔트리)·`.rsh`(include).** UE `.usf`/`.ush`의 역할 분리를 따르고 Rocks 명명(`.rasset`·`.rpkg`·`.rnug`)에 맞춘다. C++·HLSL 공용 헤더는 `.h`다(3절). 고려했던 대안은 `.hlsl`/`.hlsli`였다.
2. **HLSL 2021 범위** — UE는 플랫폼 값 `EnablesHLSL2021ByDefault`(0 끔, 1 Global 셰이더 전부, 2 모든 셰이더, `Runtime/RHI/Public/DataDrivenShaderPlatformInfo.h:119`)에 레이 트레이싱 셰이더 강제(`Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp:3489–3498`)와 타입별 선택을 더한다. PCD3D_SM6·VULKAN_SM5·VULKAN_SM6·VULKAN_SM5_ANDROID·METAL_SM5·METAL_SM6·METAL_SM6_IOS는 1, PCD3D_SM5는 0(키 없음)이다(`Engine/Config/Windows/DataDrivenPlatformInfo.ini:130`, `Engine/Config/VulkanPC/DataDrivenPlatformInfo.ini:131·257`, `Engine/Config/Android/DataDrivenPlatformInfo.ini:128`, `Engine/Config/Mac/DataDrivenPlatformInfo.ini:53·94`, `Engine/Config/IOS/DataDrivenPlatformInfo.ini:106`). 권장: 형식 정의의 기본값 + 셰이더 타입 선택. `D3D11_SM5`에서 2021 셰이더는 SPIRV-Cross 경로를 탄다(`D3DShaderCompiler.cpp:807,873–890`).
3. **D3D12 셰이더 모델** — 6.6(UE와 같음, `ShaderFormatD3D.cpp:107–110,139–142`) 또는 6.0~6.2 + bindless 프로필 6.6. 6.6은 Windows 10에서 Agility SDK가 Runtime 의존이 된다(9절). SH0에서 개발 기계 GPU·WARP 지원과 Windows 10 요건을 실측한 뒤 정한다(측정 S2).
4. **파라미터 선언** — MetaCompiler 셰이더 파라미터 종류(권장, 13절, 리플렉션 단일 출처·생성 HLSL을 디스크에 둠) 또는 UE 매크로 방식(`SHADER_PARAMETER_*`, `Runtime/RenderCore/Public/ShaderParameterMacros.h`).
5. **형식별 생산 호스트** — 키가 호스트별이므로(10절) DXIL·SPIR-V·ESSL은 각 호스트가 직접 만든다. 한 호스트만 만들 수 있는 형식: DXBC는 Windows. metallib은 macOS(Xcode Metal 도구)를 기준으로 하고, Windows에도 Apple Metal Developer Tools(UE 5.8이 지원, 기대 버전 5.3, `MetalShaderFormat.cpp:724–725,1030–1137`)가 있으면 만들게 할지 정한다(권장: 허용, 형식 정의의 생산 호스트 규칙, 측정 S9). 만들 수 없는 호스트에서는 `UnsupportedCapability`이고, 원격 워커가 생기면 그쪽으로 보낸다.
6. **전처리기** — Rocks 자체 구현을 `Developer/ShaderCompiler`에 둔다(권장, UE처럼 메인 프로세스에서 실행. UE 전처리기 stb_preprocess는 Epic 저작권 원본이라 가져올 수 없다, `Developer/ShaderPreprocessor/Private/stb_preprocess/preprocessor.h:1`) 또는 라이선스를 확인한 외부 C 전처리기를 P3 C 원본 규칙으로 들인다. DXC `-P`는 메인 프로세스가 DXC를 열게 되어(9절 규칙 위반) 쓰지 않는다. 자체 구현의 정합성은 측정 S7로 확인한다.
7. **경고 정책** — 확인 필요. CompilerDiagnostics는 "외부 원본도 엔진과 같은 경고 프로필로 컴파일한다"(:25), "생성 파일이라는 이유로 경고 검사를 생략하지 않는다"(:9), "이 절 밖의 경고 억제 예외는 없다"(:36)고 한다.
   - (1) 오프라인 도구 빌드: 결정 8(UE 방식)로 DXC·SPIRV-Cross·SPIRV-Tools는 업스트림 빌드 설정(예외·RTTI 사용, 업스트림 경고 설정)으로 엔진 빌드 밖에서 컴파일한다. DXC(LLVM)는 예외 없이·Rocks 경고 프로필로 빌드할 수 없다. 권장: CompilerDiagnostics에 "Rocks 빌드가 컴파일하지 않는 미리 빌드된 바이너리" 승인 절을 더한다(조건: 원본 태그·patch·빌드 스크립트 고정, Rocks가 include하는 공개 헤더는 경고 0, 엔진 컴파일 인자에 억제를 더하지 않음).
   - (2) 셰이더 컴파일 경고: 권장은 공통 규칙대로 작성자 원문과 SPIRV-Cross가 다시 쓴 중간 HLSL 모두 경고를 오류로 하는 것이다. 중간 원문 경고는 SPIRV-Cross 옵션이나 추적 가능한 patch(8절 4항)로 없앤다. UE는 작성자 원문에만 `-WX`를 걸고(`Developer/ShaderCompilerCommon/Private/ShaderConductorContext.cpp:425–429`), SPIRV-Cross 출력을 FXC로 컴파일할 때는 경고를 오류로 하지 않는다(`D3DShaderCompiler.cpp:959–961`).
8. **도구 공급 방식** — **결정됨(2026-11-13): UE 방식.** DXC·SPIRV-Cross·SPIRV-Tools를 엔진 빌드 밖 스크립트로 직접 빌드해 바이너리로 배포하고, 엔진은 받은 바이너리를 쓰기만 한다(8절 4항). 조사 근거: upstream DXC는 macOS 빌드를 공식 CI로 돌리고(업스트림 `azure-pipelines.yml`), SPIRV-Cross는 C API 공유 라이브러리를 CMake 옵션으로 만든다. 고려했던 대안은 LunarG Vulkan SDK 바이너리였다(SPIRV-Cross C API 공유 라이브러리 포함 여부 미확인, 설치기라 다시 묶어야 함, 패치 불가).
9. **SH2 범위** — **결정됨(2026-11-13), 2026-11-14 개정.** Graphics API를 컴파일된 코드로 바꾸는 변경이 D3D12·Vulkan self-test를 함께 통과할 때 들어간다. 처음 결정은 같은 변경에서 OpenGL을 지우는 것이었으나, 2026-11-14 OpenGL ES 필수·UE 방식 결정에 따라 OpenGLGraphics는 지우지 않고 같은 변경에서 GLES 미리보기 형식을 받게 바꾼다(UE `OpenGLDrv`가 Windows 미리보기와 Android를 함께 맡는 것과 같다). self-test는 D3D12·Vulkan·GLES 미리보기를 대조한다.
10. **셰이더 키의 툴체인 지문** — **결정됨(2026-11-13): 공통 규칙 준수.** 셰이더 job 키에도 AssetSystem 9절 공통 항목(툴체인 지문·결정적 수학 버전·대상 프로파일 id와 출력 설정 전체 포함)을 그대로 넣는다(10절). UE 5.8은 셰이더 job 키에 엔진을 빌드한 컴파일러·호스트를 넣지 않지만(`ShaderCore.cpp:4612–4671`), Rocks는 공통 규칙을 우선한다. 비용은 호스트 사이 셰이더 너겟 비공유와, 프로파일 출력 설정이 바뀔 때의 재컴파일이다.
11. **셰이더 구현 착수 위치** — 고정 순서(A0-B → A0-O → A0-G → A1–A6 → 화면 마일스톤) 안에서 셰이더 구현(6절 4항)을 어디에 둘지다. 셰이더는 A1–A4가 필요하고 고정 순서가 A1–A6을 한 묶음으로 두므로 A6 뒤다.
    - (가) 화면 마일스톤 뒤(권장): 고정 순서를 그대로 둔다. 화면 마일스톤은 지금의 backend별 원문 경로로 그리고, SH2가 Renderer 내장 셰이더와 Graphics 파이프라인 생성을 바꾼다. 화면 마일스톤의 범위는 아직 정의되어 있지 않으므로(4절) 이 권장은 화면 마일스톤이 내장 셰이더 몇 개로 그리고 material(셰이더 그래프)을 들이지 않는다는 조건에서다. (가)를 고르면 5.2절 G2(ES 공통 API) 규칙을 화면 마일스톤의 OpenGL 작업까지 넓히고, 화면 마일스톤의 새 셰이더도 7.2절 규칙(짝 규칙·허용 타입·`ATTRIBUTEn`)대로 쓴다.
    - (나) 화면 마일스톤 본체 앞: 화면 마일스톤을 처음부터 쿠킹된 셰이더와 최종 바인딩 규약 위에 쌓는다. 화면 마일스톤이 P2·P3·P4·P5·P6a·P6b·P9–P11·P14·SH0–SH2만큼 늦어진다. 화면 마일스톤이 material이나 많은 셰이더를 들인다면 이쪽이 낫다.

## 16. 이후 단계(선행 작업 뒤)

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| SH0 | 도구 스파이크: 지금의 내장 Renderer 셰이더(`Renderer.cpp:14–46`)를 DXIL·SPIR-V·DXBC·MSL·ESSL로 만들고 검증 도구를 돌린다. Windows·macOS 양쪽에서 실행 | P3·P4·P10 | 형식별 산출, `spirv-val` 통과, DXIL 서명 hash 검사(macOS DXC 포함, S1), FXC 고정본 로드, 리플렉션 덤프, `ROCKS_IMAGE_FORMAT`의 DXC 지원 확인, S2·S3·S6·S8·S9·S12(데스크톱) 측정 |
| SH1 | 소스 트리·가상 경로·`Platform.rsh`·전처리기(결정 6)·ShaderCompileWorker(P1, 11절)·두 층 키(P5)·에셋 너겟·`shader-compile` 명령·Development 필요 시 컴파일 | SH0·P1·P5(A2·A3·P6a)·결정 6 | 같은 입력 같은 바이트, 빈/찬 캐시 동일, 워커 강제 종료 보고, 부모 종료 시 워커와 그 자식 종료, 형식 버전 불일치 진단, 도구 조회, 전처리 정합성(S7) |
| SH2 | 리플렉션 정규화·워커 규칙 검사(7.2절)·Graphics API(`ShaderCodeKind`·`CreateShader`·파이프라인 layout·root signature·래스터(컬링·앞면)와 깊이 상태)·셰이더 타입과 퍼뮤테이션(P14)·쿠킹된 라이브러리 로드(P6b)·VulkanGraphics 최소 구현(offscreen·내장 셰이더, P9)·OpenGLGraphics GLES 미리보기 전환(P11)·Renderer 원문·런타임 `D3DCompile`·GLSL 원문 제거(결정 9) | SH1·P2·P4·P6b·P9·P11·P14·A0-G·결정 2·3·4·5 | Sandbox self-test에서 D3D12·Vulkan·GLES 미리보기가 허용 오차 안 일치(Vulkan·GL은 CI에서 NOT RUN일 수 있다), A0-G 범위를 Vulkan이 받음(G3), 행렬 옵션·앞면 매핑 기록(S5), Release에 컴파일러 없음(P4), 7.3절 SH2 시험 |
| SH3 | 파라미터 구조체(P7)·생성 HLSL·오프셋 시험 | SH2·P7 | 7.3절 (a)·(f)를 생성 구조체로 |
| SH4 | 감시 재컴파일(`DirectoryWatch`)·오류 위치·디버그 심볼·PSO 교체 | SH2 | 저장 후 10초 안 반영(계획 I5) |
| SH5 | D3D11Graphics → MetalGraphics | SH2·P12·P15 | 각 backend가 D3D12와 허용 오차 안 일치, 7.3절 (c)·(e)·(g)의 D3D11·Metal 부분 |
| SH6 | Android: `GLES31_ANDROID` 기기 실행, `VULKAN_ANDROID` 형식과 VulkanGraphics Android | SH2·P13·P11 | Android(실기기 또는 S11 에뮬레이터)에서 D3D12 기준 이미지와 허용 오차 안 일치, 7.3절 (c)·(i)의 Android 부분, S12(기기) |

## 17. 개정 이력

- 개정 1(2026-11-13): 초안.
- 개정 2(2026-11-13): 독립 검토 2건 반영.
  - UE 사실 확인: 약 60개 주장 중 14건을 고쳤다(모바일 형식 이름, D3D11 DXC 경로 근거, Vulkan bindless·SM5/SM6 구분, 예약 space와 register 명시, QBSS 양자화, Metal UAV 배정, `-fvk-use-dx-position-w`, 루트 상수 4 DWORD, DXCWrapper Windows 한정, `[ShaderPlatform]` 범위, ShaderArchive 파일·배포, HLSL 2021 기본값, `GLSL_430_REMOVED`).
  - 반례·정합성: OpenGL과 SH2의 모순 → Vulkan을 SH2로 앞당김. D3D12 64 DWORD 예산. 상수 버퍼 반례 → 허용 타입 제한. macOS 셰이더 생산 호스트 부재 → macOS DXC 자체 빌드와 Windows Metal 도구. DXC location 선언 순서 → 재작성과 `spirv-val` 필수. 공통 상한. SPIRV-Cross 예외 → 공유 라이브러리 격리. 경고 예외 삭제. 바이너리 manifest. FXC 지문. job 키 정의. 의존 관계. P6 출처 일원화와 MacOS 프로필. 개정 대상 누락. 교차 참조·용어.
- 개정 3(2026-11-13): 개정 2의 재검토 2건 반영.
  - 사실 9건: QBSS tier 기준, 구조화 SRV 근거, UE location 재배정 범위, UAV 상한의 파이프라인 단위, Metal 인덱스 상한, HLSL 2021 기본값 목록, DXC 1.8.2505 내장 validator, 근거 줄 번호, SPIRV-Cross locale 보정.
  - 반례·정합성 17건: 공유 라이브러리 모듈 종류를 P4에(차단), SPIRV-Tools 생성 파일·DXC 빌드의 Python과 버전 정합, 상한 표를 descriptor 종류로·UAV는 CS만, Metal buffer 배정·예산, D3D signature 연결 규칙·SV 허용 목록·vertex attribute remap, 워커 오프셋 검사·셰이더 전용 정렬 타입, job 키(모든 호스트 SHA·빌드 툴체인·SPIRV-Reflect·Apple 도구 manifest·공유 등급 기록), 상수 접기 결정성, 상한 검사를 프로필 단위로, 시험 시기를 SH2로, P9 Vulkan 선행, macOS DXIL 서명 실측, 텍스처·샘플러 짝, 저장 위치·CI 캐시·FXC 출처·header, P7 세부, 개정 대상 추가, 내부 정합성.
  - 사용자 결정으로 올림: 도구 공급 방식(결정 8), SH2 범위(결정 9), 셰이더 키의 툴체인 지문(결정 10). 전처리기 표현을 결정 6으로 통일.
- 개정 4(2026-11-13): 사용자 결정 반영.
  - 결정 8(도구 공급)을 UE 방식으로 정했다: 엔진 빌드 밖 스크립트로 DXC·SPIRV-Cross·SPIRV-Tools를 빌드해 바이너리로 배포하고 엔진은 링크·복사만 한다(UE ShaderConductor 빌드 스크립트·External 모듈 확인. 개정 7에서 절대 경로 동적 로드로 바뀌었다). 이에 따라 공유 라이브러리 모듈 종류, hostCode 종류, C++17·예외 빌드 설정, SPIRV-Tools 생성 파일 커밋, 경고 정책 결정(7)이 필요 없어졌다.
  - 결정 9(SH2에서 Vulkan 추가·OpenGL 제거를 한 변경으로)를 확정했다.
  - 결정 10에 UE 5.6 셰이더 job 키 구성(호스트·엔진 컴파일러 미포함)과 예외 없이 갈 때의 결과를 적었다.
- 개정 5(2026-11-13): 결정 10을 공통 규칙 준수로 확정했다.
  - 셰이더 job 키에 AssetSystem 9절 공통 항목을 모두 넣는다: 툴체인 지문, 결정적 수학 버전, 대상 프로파일 id와 출력 설정 전체. 공유 등급(`portable`)·영역별 프로필 digest·호스트 사이 바이트 동일 시험을 없앴다. 셰이더 캐시는 호스트 사이에 공유하지 않는다.
  - 상수 접기 결정성은 예외로 두지 않는다. 다르게 나오면 방법을 사용자 결정으로 올린다.
  - 같은 원칙으로 P7 셰이더 파라미터의 identity 면제를 없애고 `ROCKS_STRUCT` 경로를 따르게 했다. 공통 상한 검사는 job 안으로 옮겼다.
- 개정 6(2026-11-13): 결정 1(확장자 `.rsf`·`.rsh`)을 확정했다.
- 개정 7(2026-11-14): 현재 코드베이스·스펙 기준으로 구현 계획을 다시 세웠다.
  - 사용자 결정 반영: OpenGL ES 필수와 UE 방식("구형 모바일 때문이라도 opengles는 지원해야함", "언리얼이 다루는 방식으로 맞추면됨"), UE 근거 기준을 5.8로("앞으로 조사는 5.8 기준으로 해"). 결정 9를 개정했다(OpenGLGraphics는 지우지 않고 GLES 미리보기로 전환).
  - 조사: 빌드·ThirdParty 코드, 그래픽 런타임 코드, 관련 스펙 상태를 다시 조사해 4절(현재 코드베이스)을 새로 쓰고, 선행 작업을 다른 문서 담당(5.2절)과 셰이더 전용(5.3절)으로 나눠 상태·산출물·의존·막는 단계·완료 기준을 적었다. 측정 항목(5.4절)과 전체 순서(6절)를 더했다.
  - 독립 설계 대조(초안을 보지 않은 계획과 비교)에서 들인 것: Vulkan 로더 링크의 문제(Vulkan 런타임 없는 기계에서 실행 불가) → P10 동적 라이브러리 API와 도구 동적 로드(9절), 에셋 단계(A0-G·A1·A3·A4)에 넣을 셰이더 요구, 엔진 내장 셰이더 라이브러리(계획 9.6), Release 배제 정책 검사, Metal의 Objective-C++ 빌드·macOS 창(P12), Android 실행 기반(P13), 측정 항목.
  - UE 5.8.4 재확인(개정 6의 UE 근거 전부): 줄 번호를 모두 고쳤고, 내용이 바뀐 것은 RHI 셰이더 생성 API(`FRHICreateShaderDesc`), 셰이더 맵·코드 해시(XxHash64), Vulkan push constant(bindless RayGen에서 사용), Vulkan bindless set 구성, Metal Shader Converter 선택 조건·buffer 순서(UB 먼저), 정적 UB 매크로, 전처리 근거 위치, job 키 항목(`PreferredShaderCodeFormat`·`bBindlessEnabled`), D3D SM 선택 규칙, ShaderCodeLibrary 파일 위치, Windows Metal 도구 기대 버전, 워커 입력 검사 위치, Windows 도구 빌드 스크립트(VS2022·ClangCL)다. UE가 SPIRV-Cross·SPIRV-Tools를 ShaderConductor에 정적으로 묶고 DXC 1.8.2403·`dxil.dll`을 쓰는 점을 적고, Rocks의 업스트림 공유 라이브러리·DXC 1.8.2505 선택을 Rocks 결정으로 구분했다.
  - GLES 추가: 형식 `GLES31_ANDROID`·`GLES31_PC`(2절), UE 5.8 GL 경로 조사로 바인딩 규칙(상수 버퍼 에뮬레이션·combined sampler·클립 공간, 7절), UE 수정 도구에 기대지 않는 업스트림 대응(7.2절 11항), P11.
  - 워커 통신을 UE 파일 기반 방식으로 정했다(11절).
  - Vulkan target env를 UE `SF_VULKAN_SM5`와 같은 1.1로 고쳤다(개정 6은 1.2).
  - 교차 검증 4건(Rocks 코드 근거 약 185건, UE 5.8.4 근거 약 220건, 다른 스펙 정합성, 반례·선행 작업 완결성)을 반영했다.
    - 근거: 줄 번호·절 번호·인용 문구 30여 건(CoreObjectInputDetailedPlan 줄, Rockpile 절, `ThirdPartyRoot` 사용처, Metal 도구 버전 줄, ClangCL 기본 시점, `dxil.dll` 출처, `-Zpr` 근거, 업스트림 DXC 줄 번호 미확인 처리 등).
    - 차단 2건: 셰이더 타입·퍼뮤테이션·라이브러리 형식 → P14(12.2절). 형식 정의가 job 키보다 늦음 → P6을 P6a(형식 정의, P5 앞)와 P6b(프로필·매니페스트·내장 라이브러리, SH2 앞)로 나눔.
    - GLES: VS·PS 샘플러 binding 충돌 → 워커가 PS 0·VS 16부터 다시 매김(UE도 ESSL 320에서는 binding을 낸다), 상수 버퍼 에뮬레이션의 텍스트 치환 한계 → 구조체 1단·구조체 배열·버퍼 전체 load 금지, 모바일 상한(에뮬레이션 `vec4`·varying·MRT), UE 수정 도구 목록 정정(`pad_ubo_blocks`는 padding만, 묶음은 텍스트 재작성), sRGB 표시·확장 의존 목록, `GLES31_PC` 표시 blit과 Release 배제.
    - 키·결정성: 에셋이 아닌 처리기의 키 식(SCC 식 대신 SHA-256(`BuildKey`)), 형식 정의가 선언한 모든 도구 SHA(정적 목록), Apple 도구 지문을 워커 조회로, 프로필 없는 컴파일의 엔진 프로필, 호스트 사이 바이트 동일 측정(S3) 복원.
    - 빌드: 도구 공개 헤더를 저장소에(bootstrap 없이 기본 빌드), 도구 표 모듈과 로드 정책 구분, macOS quarantine, Agility 재배포 role, 내장 라이브러리는 설치물 단계 명령으로(Rocks.Build에 프로그램 실행 action이 없다), 라이브러리 파일 내용 주소와 게시 규칙.
    - 선행 작업: P15(D3D11·Metal 설계), P12·P13 완료 기준, APK VFS를 AssetSystem 선행으로, A0-G G2의 ES 확장 조건, A2 요구, storage image 형식 매크로, 손으로 쓴 레이아웃 표(P7 전 서명), 결정별 기한, SH 선행 열, D3D12·Metal 예산 수치, `SV_VertexID` base 규칙, 파이프라인 래스터·깊이 상태(SH2).
    - 경고 정책(결정 7)을 CompilerDiagnostics와의 충돌 때문에 확인 항목으로 되돌렸다.
  - 2차 점검(내부 정합성과 이번에 더한 UE 근거) 반영: 한 16 byte 슬롯에 기본 타입을 섞지 않는 규칙(GLES 배열 크기 계산), GLES binding을 종류별 공간으로 다시 매김, VS·PS storage block 0(ES 3.2 최소), 공통 상한을 셰이더 타입이 컴파일되는 형식으로 한정하고 그 형식 정의 SHA를 키에 넣음, 결정 2·3·5 기한을 P6a 전으로, 결정 6을 측정 없이 정하고 S7을 SH1 완료 기준으로, 엔진 컴파일 프로필을 P6a로 옮기고 TargetProfile과 스키마를 나눔, backend 이름 고정 어휘, `redist` role, Apple 도구 로드 예외, `GL_ARB_ES3_2_compatibility`, S12를 데스크톱(SH0)·기기(SH6)로 나눔, 막는 단계 열과 인용 정정.
