# 셰이더 시스템 설계

기준일: 2026-11-23, 개정 26(초안). 셰이더 원문을 HLSL 하나로 쓰고 오프라인에서 D3D12·D3D11·Vulkan·Metal·OpenGL ES용 코드로 바꾸는 구조와, 그 구현에 앞서 갖춰야 할 **선행 작업**을 정한다. 개정 7은 현재 코드베이스(HEAD `0be1e64`, 2026-11-14)와 다른 설계 문서의 상태를 다시 조사해 선행 작업과 순서를 새로 세웠다(4–6절).

UE 근거는 UE 5.8.4(EpicGames/UnrealEngine `release` 브랜치 commit `f45729c0b6f097038e72b52d366a62baeca3275b`)의 원본이다. 2026-11-14 사용자 지시로 기준을 5.6에서 5.8로 올렸고, 개정 6까지의 5.6 근거를 모두 5.8.4로 다시 확인했다(17절). 경로는 `Engine/Source/` 기준이며 `Engine/Shaders`·`Engine/Config`·`Engine/Build`는 그대로 적는다. UE 밖 도구(DXC·SPIRV-Cross·SPIRV-Tools·Vulkan)에 관한 사실은 그 출처를 따로 적는다. 개정 6까지의 조사 문서(UE 5.6 기준)는 [Rocks 셰이더 시스템](https://claude.ai/artifact/U5uNh2EWGvXKQiNYXnuc1Q)이다.

[엔진 구현 계획](EngineImplementationPlan.ko.md)·[엔진 런타임](EngineRuntime.ko.md)·[에셋 시스템](AssetSystem.ko.md)과 충돌하는 부분은 이 문서 기준으로 바꾸고 그 문서들도 같이 고친다(14절). 개정은 이 문서가 확정될 때 함께 하되, 에셋 시스템 단계(A0-G·A1·A3·A4)에 넣을 셰이더 요구는 그 단계 착수 전에 반영해야 한다(6절).

상태 표기는 AssetSystem과 같다: **구현**(동작·시험), **계약**(API·형식이 있고 미지원 값은 명시 오류), **후속**(API도 없음). 선행 작업 가운데 P1·P3·P4·P6a·P9·P10·P11과 SH0(Windows)이 구현되었고, SH1은 2026-11-23 사용자 지시로 착수한다(11.1절). 처음에는 엔진 기반이 준비될 때까지 스펙으로만 두었다(2026-11-13 사용자 지시). 착수 위치는 6절, 착수 전에 정할 것은 15절이다.

## 1. 결정

| 항목 | 결정 | 근거 |
|---|---|---|
| 셰이더 언어 | HLSL 하나. backend별 원문(현재 `ShaderSource{hlsl, glsl}`, `Engine/Source/Runtime/Graphics/Public/Graphics/DeviceTypes.h:140–143`)을 두지 않는다 | UE도 `.usf`·`.ush` HLSL을 쓰고(공유 디렉터리는 `.h`도 허용) 형식별 ShaderFormat 모듈이 바꾼다(`Runtime/RenderCore/Private/ShaderCore.cpp:1620–1670` `CheckVirtualShaderFilePath`, `.h`·`/ThirdParty/` 예외 `:1646–1659`) |
| 변환 도구 | DXC(HLSL → DXIL·SPIR-V, 1.8.2505 이상), SPIRV-Cross(SPIR-V → MSL·ESSL·D3D11용 HLSL 5.0), SPIRV-Tools(`spirv-val`·최적화 pass, 필수), SPIRV-Reflect, FXC(버전을 고정한 `d3dcompiler_47.dll`), Apple `metal`·`metallib` | UE 5.8도 같은 도구를 쓴다. 다만 UE는 SPIRV-Cross·SPIRV-Tools를 자기 래퍼 ShaderConductor 공유 라이브러리에 정적으로 묶고(`ThirdParty/ShaderConductor/ShaderConductor/Source/Core/CMakeLists.txt:56,82–95`) 두 도구에 UE 수정을 넣었다(7.2절 11항). ShaderConductor 원 저장소는 2023-01 보관되었고 UE 사본·수정은 UE 원본이라 가져올 수 없으므로, Rocks는 업스트림 SPIRV-Cross·SPIRV-Tools의 C API 공유 라이브러리를 직접 부른다. DXC 1.8.2505 이상은 UE(1.8.2403 기반 수정본과 `dxil.dll` 동봉, `ThirdParty/ShaderConductor/ShaderConductor/External/DirectXShaderCompiler/utils/version/latest-release.json`)를 넘어서는 Rocks 결정이다(10절 서명) |
| 도구 공급 | UE처럼 엔진 빌드 밖 스크립트로 도구를 빌드해 바이너리로 배포하고, 엔진은 받은 바이너리를 쓰기만 한다(2026-11-13 사용자 결정 8, 8절) | UE `ThirdParty/ShaderConductor/Build_ShaderConductor_*`, `Engine/Build/Commit.gitdeps.xml` |
| 필수 backend | D3D12, D3D11, Vulkan, Metal, **OpenGL ES** | 2026-11-13 사용자 결정(앞의 넷)에 2026-11-14 사용자 결정 "구형 모바일 때문이라도 opengles는 지원해야함", "언리얼이 다루는 방식으로 맞추면됨"을 더했다. 데스크톱 OpenGL SM5(GL 4.5 출시 대상)는 제외한다: UE 5.8에도 데스크톱 GL SM5 형식이 없다(`GLSL_430_REMOVED`, `Developer/ShaderFormatOpenGL/Public/ShaderFormatOpenGL.h:30`). OpenGL ES는 UE처럼 Android 출시 형식과 Windows 개발용 미리보기 형식 둘이다(2절) |
| 두 번째 검증 backend | Vulkan. VulkanGraphics 최소 구현을 SH2에 넣는다. OpenGLGraphics는 지우지 않고 SH2에서 GLES 미리보기 형식을 받는 backend로 바꾼다(결정 9 개정) | SH2가 backend별 원문을 없애므로 OpenGLGraphics도 쿠킹된 코드를 받아야 한다. GLES 필수 결정으로 OpenGLGraphics는 UE `OpenGLDrv`처럼 Windows 미리보기와 Android 실행을 맡는다(P11) |
| 런타임 | 쿠킹된 코드만 받는다. Release(UE Shipping에 해당) 실행 파일에 셰이더 컴파일러를 넣지 않는다(9절 정책 검사) | UE RHI 셰이더 생성은 코드 바이트를 받는다(`Runtime/RHI/Public/DynamicRHI.h:247` `RHICreatePixelShader(const FRHICreateShaderDesc&)`, 코드·소유자·라이브러리 `Runtime/RHI/Public/RHIResources.h:789–813`) |
| 컴파일 실행 | 메인 프로세스가 입력 수집·**전처리**·키 계산을 하고, 별도 ShaderCompileWorker 프로세스가 컴파일한다(11절). 메인 프로세스는 도구 바이너리와 ShaderFormat 모듈을 쓰지 않는다 | UE는 전처리를 job 제출 때 메인 프로세스에서 하고 워커에서는 하지 않는다(`Runtime/RenderCore/Public/ShaderCompilerCore.h:645`, `Runtime/Engine/Private/ShaderCompiler/ShaderCompilerJobCache.cpp:1544–1545`). 워커는 `Programs/ShaderCompileWorker`. 전처리기는 Rocks 자체 구현을 `Developer/ShaderCompiler`에 둔다(결정 6, 2026-11-17) |
| 도구 로드 | 워커가 도구 공유 라이브러리를 사용자 단위 저장소에서 **절대 경로로 연다**(P10 동적 라이브러리 API, 9절). Vulkan 로더도 런타임에 연다(P9) | UE도 Windows에서는 DXC·FXC를 전체 경로로 연다(`Developer/ShaderCompilerCommon/Private/DXCWrapper.cpp:69–81`, `Developer/Windows/ShaderFormatD3D/Private/D3DShaderCompiler.cpp:399–408`. macOS의 DXC는 링크와 지연 로드, `ThirdParty/ShaderConductor/ShaderConductor.Build.cs:59–70`). Vulkan 로더는 `LoadLibraryW("vulkan-1.dll")`(`Runtime/VulkanRHI/Private/Windows/VulkanWindowsPlatform.cpp:66`)·`dlopen("libvulkan.so")`(`Runtime/VulkanRHI/Private/Android/VulkanAndroidPlatform.cpp:314`)로 연다. 로더를 링크하면 Vulkan 런타임이 없는 기계에서 실행 파일이 시작하지 못한다 |
| 캐시 | 에셋 너겟(AssetSystem 9절), 공통 키 규칙 그대로(2026-11-13 사용자 결정 10, 10절) | UE의 job 캐시·DDC 역할이다 |
| D3D12 셰이더 모델 | SM 6.6(결정 3, 2026-11-17). Windows 10에서는 Agility SDK(`D3D12Core.dll`)를 `redist` role로 재배포한다(9절) | UE `PCD3D_SM6`이 SM 6.6이다(`Developer/Windows/ShaderFormatD3D/Private/ShaderFormatD3D.cpp:107–110`). 측정 S2: 개발 기계 GPU·WARP는 Agility SDK 없이 6.8 |
| HLSL 2021 | 형식 정의의 기본값과 셰이더 타입의 선택으로 정한다(결정 2, 2026-11-17). `D3D12_SM6`·`VULKAN_SM5`·`METAL_SM5`는 켜고 `D3D11_SM5`·GLES 형식은 끈다. `D3D11_SM5`에서 2021 셰이더는 SPIRV-Cross 경로를 탄다 | UE `EnablesHLSL2021ByDefault`: PCD3D_SM6·VULKAN·METAL 1, PCD3D_SM5·OPENGL_ES3_1_ANDROID 키 없음(15절 결정 2) |
| 형식별 생산 호스트 | 형식 정의의 생산 호스트 규칙으로 둔다(결정 5, 2026-11-17). DXBC는 Windows, metallib은 macOS와 Apple Metal Developer Tools가 있는 Windows, 그 밖은 각 호스트. 도구가 없으면 `UnsupportedCapability` | UE 5.8은 Windows Metal Developer Tools를 지원한다(`Developer/Apple/MetalShaderFormat/Private/MetalShaderFormat.cpp:724–725`) |

이 문서가 정한 규칙: 바인딩·상수 버퍼·단계 사이 변수(7절), ThirdParty 종류·오프라인 도구 빌드(8절), 도구 로드·Release 배제(9절), 셰이더 job 키(10절), 워커 통신(11절). 사용자가 정한 것: 확장자 `.rsf`·`.rsh`(결정 1), 도구 공급 UE 방식(결정 8), SH2 범위(결정 9, 2026-11-14 GLES 결정에 따라 개정), job 키 공통 규칙(결정 10), OpenGL ES 필수와 UE 방식(2026-11-14). 2026-11-17에 HLSL 2021 범위(결정 2), D3D12 셰이더 모델 6.6(결정 3), 형식별 생산 호스트(결정 5), 자체 전처리기(결정 6)를 정했다. 경고 정책(결정 7)과 셰이더 구현 착수 위치(결정 11)는 2026-11-16에 정했다. 파라미터 선언(결정 4)은 2026-11-18에 권장안(MetaCompiler 방식)으로 정했다. 남은 사용자 결정은 없다(15절).

## 2. 형식과 변환 경로

| 형식 id | backend | 경로 | UE 5.8 대응 |
|---|---|---|---|
| `D3D12_SM6` | D3D12 | DXC → DXIL SM 6.6(결정 3), DXC 내장 validator 서명(10절) | `PCD3D_SM6`: SM 6.6, 워크 그래프 6.8(`Developer/Windows/ShaderFormatD3D/Private/ShaderFormatD3D.cpp:102–110`). `PreferredShaderCodeFormat`이 DXIL이면 SM 6.0(`:120`) |
| `D3D11_SM5` | D3D11 | FXC → DXBC SM 5.0(Windows만). HLSL 2021 기본 끔(결정 2), HLSL 2021 셰이더와 FXC 1차 실패 셰이더는 DXC → SPIR-V → SPIRV-Cross HLSL 5.0 → FXC | `PCD3D_SM5`. DXC 선행 경로(`D3DShaderCompiler.cpp:802–995`, 조건 `:806–807`·`:840–841`) |
| `VULKAN_SM5` | Vulkan | DXC `-spirv`(target env `vulkan1.1`) → binding·location 재배치 → `spirv-val` | `SF_VULKAN_SM5`(Vulkan 1.1, `Developer/VulkanShaderFormat/Private/VulkanShaderCompiler.cpp:96–99`)의 비-bindless 경로. `SF_VULKAN_SM6`은 1.3(`:88–91`)·scalar layout·bindless(`Engine/Config/Windows/BaseWindowsEngine.ini:57–63`) |
| `METAL_SM5` | Metal | DXC `-spirv` → 재배치 → `spirv-val` → SPIRV-Cross MSL 2.4 → `metal`·`metallib`(macOS, 또는 Apple Metal Developer Tools가 있는 Windows, 결정 5) | `SF_METAL_SM5`. bindless면 DXIL → Metal Shader Converter(`Developer/Apple/MetalShaderFormat/Private/MetalShaderCompiler.cpp:1113–1120`, Rocks는 후속) |
| `GLES31_ANDROID` | OpenGL ES | DXC `-spirv`(target env `vulkan1.1`) → SPIRV-Tools 최적화 → 재배치 → `spirv-val` → SPIRV-Cross ESSL `#version 320 es` → 상수 버퍼 에뮬레이션 재작성(7.2절 4항) | `GLSL_ES3_1_ANDROID`: ES3.1 feature level, ESSL 320(`Developer/ShaderFormatOpenGL/Private/OpenGLShaderCompiler.cpp:2883–2908`), target env 기본 1.1(`Developer/ShaderCompilerCommon/Public/ShaderConductorContext.h:118`). ES 3.2 기기 필수(`Runtime/OpenGLDrv/Private/Android/AndroidOpenGL.cpp:453–485`. ES 지원을 켜고 패키징하면 manifest `glEsVersion=0x00030002`, `Programs/UnrealBuildTool/Platform/Android/AndroidToolChain.cs:279–284`, 기록 `UEDeployAndroid.cs:3049`) |
| `GLES31_PC` | OpenGL ES(Windows 미리보기) | `GLES31_ANDROID`와 같고 클립 공간 보정만 다르다(7.2절 8항). 개발용이며 출시 프로필에 넣지 않는다 | `GLSL_150_ES31`(`SP_OPENGL_PCES3_1`): Windows GL 4.3 core(`Runtime/OpenGLDrv/Private/Windows/WindowsOpenGLPlatform.cpp:200–205`), 쿠킹 데이터를 쓰는 실행 파일에는 등록하지 않는다(`Runtime/RHI/Private/Windows/WindowsDynamicRHI.cpp:628–633`) |
| `VULKAN_ANDROID`·`METAL_IOS` | 모바일 | 자리만 둔다(Unsupported). `VULKAN_ANDROID`는 SH6 | `SF_VULKAN_ES31_ANDROID`(`Runtime/RHI/Public/RHIShaderFormatDefinitions.inl:35`), `SF_METAL_SM5_IOS`·`SF_METAL_SM6_IOS`(`:25–27`) |

GLES 기기 요구는 UE와 같다: OpenGL ES 3.2 기기만 실행하고 ES 3.0·3.1만 지원하는 기기는 받지 않는다. 근거(2026-11-14 확인):

- **점유율.** Google [Android 배포 대시보드](https://developer.android.com/about/dashboards)(2025-11-24까지의 활성 기기 표본, 표는 기기가 지원하는 최고 버전)의 OpenGL ES 분포는 아래와 같다. ES 3.2를 요구해 빠지는 기기는 전체의 약 3.4%, 휴대 기기의 약 1.3%다.

  | 최고 지원 버전 | 전체 기기 | 휴대 기기 |
  |---|---|---|
  | ES 3.2 | 94.64% | 98.24% |
  | ES 3.1 | 2.62% | 0.39% |
  | ES 3.0 | 0.80% | 0.86% |
  | ES 2.0 | 1.91% | 0.49% |

- **GLES가 실제로 맡는 기기.** 같은 대시보드(Android API 23 이상, 2025-11-24까지 28일)에서 Vulkan이 없는 기기는 전체의 8.85%(휴대 7.37%), Vulkan 1.0.3까지만 되는 기기는 3.82%(휴대 3.86%)다. Vulkan 1.1 미만이 약 12.7%다. Android는 64비트이고 저메모리가 아닌 기기에 Vulkan을 요구한다: Android 10 출시 기기까지 1.1, Android 13 출시부터 1.3, Android 16 출시부터 1.4다([AOSP Vulkan 구현](https://source.android.com/docs/core/graphics/implement-vulkan), [Android Vulkan 개요](https://developer.android.com/games/develop/vulkan/overview) "All 64-bit Android devices from Android 10 (API level 29) and higher support Vulkan 1.1"). 그래서 Vulkan 1.1이 없는 기기는 Android 9 이전에 출시된 기기와 32비트·저메모리 기기다. 이 가운데 ES 3.2를 지원하는 기기가 "구형 모바일"용 GLES 경로의 대상이다. UE도 Vulkan을 못 쓰는 기기에서 ES 3.2로 실행하고 ES 3.2가 안 되면 실행을 거부한다(`Runtime/OpenGLDrv/Private/Android/AndroidOpenGL.cpp:453–485`).
- **세대.** Vulkan은 Android 7(API 24)부터 들어왔다(같은 Vulkan 개요). ES 3.0·3.1까지만 지원하는 기기는 대략 2013–2016년 GPU 세대로 지금 9–12년 된 기기다. GPU별 지원 버전은 드라이버 판마다 달라 확인하지 않았다(미확인). 5년 전(2021년) 기기는 대부분 ES 3.2와 Vulkan 1.1을 함께 지원한다.
- **앞으로.** Google은 OpenGL ES의 기능 개발을 멈췄고(Vulkan이 기본), Android 15부터 ANGLE(Vulkan 위의 OpenGL ES)을 선택 계층으로 넣었으며, 결국 OpenGL ES를 ANGLE로만 제공하되 모든 기기에서 OpenGL ES 지원은 계속한다고 밝혔다(같은 Vulkan 개요). 그래서 GLES 경로는 Vulkan 1.1이 없는 기기를 위한 경로로 두고 새 렌더링 기능은 Vulkan 쪽에 먼저 둔다.
- **ES 3.0·3.1까지 받으려면.** 별도의 셰이더 형식과 제약(ES 3.0에는 compute·storage buffer가 없다)이 더 필요하고, UE도 그런 형식을 지웠다(`GLSL_310_ES_EXT_REMOVED` 등, `Developer/ShaderFormatOpenGL/Public/ShaderFormatOpenGL.h:27–41`). 남는 대상이 위 비율이므로 넣지 않는다.

GLES 형식의 셰이더 단계는 VS·PS·CS다. UE GL 컴파일러는 VS·PS·GS·CS를 받지만 ES3.1 feature level은 GS를 빼고 테셀레이션은 받지 않는다(`OpenGLShaderCompiler.cpp:2998–3004`, `Runtime/RHI/Public/DataDrivenShaderPlatformInfo.h:1087–1089`).

장치 요구 수준이 다르면(예: Vulkan 최소 보장보다 큰 descriptor 한도) 다른 형식 id로 둔다. 다만 대체 경로가 있는 선택 기능(VS layer·viewport 출력 등)은 형식 id를 나누지 않고 실행 장치의 능력으로 확인한다(7.2절 7항, 15절 결정 12). 상한과 기능 요구는 형식 정의가 가진다(12절). 위 표의 형식 정의 파일은 `Engine/Config/ShaderFormats/<id>.json`이다(P6a, 12.1절). HLSL 2021 기본값은 `D3D12_SM6`·`VULKAN_SM5`·`METAL_SM5`가 켬, `D3D11_SM5`·`GLES31_*`가 끔이다(결정 2, GLES는 UE `OPENGL_ES3_1_ANDROID`처럼 끔). MSL 버전 2.4는 UE 5.8 Mac 기본(`Engine/Config/BaseEngine.ini:3434` `MetalLanguageVersion=7`, `Developer/Mac/MacTargetPlatformSettings/Classes/MacTargetSettings.h:23`)과 같다.

SM 6 기능(웨이브 연산 등)이 필요한 셰이더: UE는 이런 셰이더(`ShaderFormatD3D.cpp:113–122`)를 DXIL로 컴파일하고 `EShaderOptionalDataKey::ShaderModel6`을 붙이며(`D3DShaderCompilerDXC.cpp:1195–1199`), D3D11 RHI는 이 표시가 있는 셰이더를 만들지 않는다(`Runtime/Windows/D3D11RHI/Private/D3D11Shaders.cpp:54–66`). Rocks는 셰이더 타입이 요구 기능을 선언하고, 형식 정의의 지원 기능 목록에 없으면 그 형식에서 쿠킹하지 않는다(12절). 런타임에 없는 셰이더를 찾으면 `IncompatibleCook`이다.

## 3. 구조 요약

```text
Engine/Shaders/{Public,Private,Shared}             .rsf(엔트리)·.rsh(include)·.h(C++·HLSL 공용)
Engine/Config/ShaderFormats/<id>.json              형식 정의(12절)
Engine/Config/TargetProfiles/<id>.json             graphicsBackends·shaderFormats(12절, 파일은 AssetSystem 10절)
Engine/Source/ThirdParty/<Lib>/                    DXC·SPIRV-Cross·SPIRV-Tools·FXC(binary manifest·오프라인 빌드 스크립트), SPIRV-Reflect(C 원본), Vulkan-Headers(headers)(8절)
Engine/Source/Runtime/ShaderCore                   셰이더 타입·퍼뮤테이션 ID·파라미터 메타데이터·셰이더 전용 정렬 타입·라이브러리 로드
Engine/Source/Developer/ShaderCompilerCommon       형식 정의 읽기·워커 메시지 형식·형식 컴파일 인터페이스(두 프로세스 공용, 11.1절)
Engine/Source/Developer/ShaderCompiler             소스 트리·전처리기·job 키·에셋 너겟·컴파일 관리자·워커 풀(메인 프로세스)
Engine/Source/Developer/ShaderWorker               워커 메시지 처리·도구 조회·형식 분배(워커 프로세스 본체, 시험 probe도 부른다)
Engine/Source/Developer/ShaderToolTable            ThirdParty manifest에서 생성한 도구 표(이름·SHA-256, 메타데이터만, 9절)
Engine/Source/Developer/ShaderFormat{D3D,Vulkan,Metal,OpenGL}   워커 안의 형식별 컴파일·재배치·리플렉션·GLES 재작성
Programs/ShaderCompileWorker                       워커 프로세스(ShaderWorker를 부르는 진입점)
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
| Renderer | 내장 셰이더를 C++ 문자열로 가진다(HLSL·GLSL 문자열, 2026-11-17부터 Sandbox 시험 샘플 `Programs/Sandbox/Private/SampleRenderer.cpp`), 파이프라인 하나(텍스처×정점색×항목색) | `.rsf` 원문과 쿠킹된 라이브러리 → SH1·SH2 |
| Sandbox | self-test가 D3D12 하드웨어·WARP·OpenGL을 대조(허용 오차 채널 ±8·2%, `Programs/Sandbox/Private/GraphicsSelfTest.cpp:409–415`, 대상 `:427–438`). `--graphics=` 인자는 없다 | Vulkan·GLES 미리보기 대조 → SH2 |
| GraphicsTypes | RGBA8 2종, 2D, 필터 4종(`PixelFormat.h:12–16`, `Sampler.h:11–20`) | A0-G(다른 문서, 5.2절 요구) |
| 수학 타입 | `Matrix4`·`Vector3`는 정렬 지정 없음(`Core/Source/Runtime/Core/Public/Core/Math/Matrix4.h:56–61`, `Vector3.h:19–24`) | 셰이더 전용 정렬 타입(7.2절 5항) → SH2 |
| 플랫폼 | `Platform/Process.h`(P1 구현), `Platform/DirectoryWatch.h:88`(A0-P 구현). `Platform/DynamicLibrary.h`(P10 구현, Windows 검증, XInput이 이 API로 연다). PlatformWindow는 Win32만이고 다른 플랫폼은 Unsupported(`PlatformWindow.Build.cs`) | P10, P12, P13 |
| Core | SHA-256(`Core/Encoding/Sha256.h:23,40`), JSON(`Core/Text/Json.h`), 결정적 수학 버전 `rocks::deterministic::Version`(`DeterministicMath/DeterministicMath.h:31`), 툴체인 지문·도구 빌드 ID(`Core/Build/BuildInfo.h:24,30`), Archive·Record·RecordList, `WorkerScheduler`, `CommandletRegistry` | 그대로 쓴다(5.2절) |
| 리플렉션 | A0-B B1–B8 구현(`ROCKS_STRUCT`·identity·배열·지정자·World 타입, `e405c37`) | 셰이더 파라미터 종류 → P7 |
| ThirdParty | 루트 `Core/Source/ThirdParty` 하나(`Programs/Rocks.Tooling.Common/ThirdPartyManifest.cs:76`), `kind`는 `code`·`data`(`:28`), `code`는 `.c` 번역 단위만(`:357–364`), 미리 빌드된 바이너리 필드 없음(`:132–136`, 모르는 필드는 오류 `:45`) | P3 |
| 빌드 | 모듈 종류는 StaticLibrary·Executable(`Programs/Rocks.Build/NativeBuildModels.cs:38`), 언어는 C·C++(`:57`), `PrebuiltLibraries`(`:147`), 런타임 파일 복사는 Asan DLL만(`ActionGraph.cs:329–337`), rpath 처리 없음, 생성 코드 `<out>/Generated/<Module>`·`<out>/BuildInfo` | P4·P12 |
| CI | `.github/workflows/foundation.yml` 하나(Windows·macOS), 캐시 단계 없음 | P3(저장소 캐시), P9(Vulkan 장치) |
| 에셋 | A0-O·A0-G·A1–A6 미착수(AssetRuntime·AssetNugget 모듈 없음), `ProjectLayout`은 이름만(AssetSystem 1절 A2) | A1–A4 → P5·P6b |
| 셰이더 원문·설정 | `Engine/Shaders`·`Engine/Config` 없음, 셰이더 파일 없음 | SH1·P6a·P14 |

다른 설계 문서 상태: A0-P·A0-C·A0-B는 구현되었고 A0-O·A0-G·A1–A6은 착수 전이다. "화면 마일스톤"의 범위는 2026-11-16 [렌더 기반](RenderBase.ko.md) 18.1절에 정의했다(셰이더 구현과 렌더 기반을 함께 세우는 단계, 결정 11 (나)).

## 5. 선행 작업

### 5.1 목록

| ID | 작업 | 담당 문서 | 상태 | 막는 단계 | 의존 | 상세 |
|---|---|---|---|---|---|---|
| A0-C | SHA-256·JSON·결정적 수학 버전 | CoreDataFoundations | **구현** | — | — | 5.2 |
| A0-P | 파일 감시·원자적 교체·no-replace 이동·시각 설정 | AssetSystem A0-P | **구현**(macOS 미검증) | — | — | 5.2 |
| B1–B8 | 리플렉션(`ROCKS_STRUCT`·identity·배열), B7 툴체인 지문 | ReflectionFoundations | **구현** | — | — | 5.2 |
| A0-G | 형식·차원·swizzle·샘플러 확장 | AssetSystem A0-G | 미착수 | P11, SH2 | — | 5.2 G1–G3 |
| A1 | AssetRuntime, `UnsupportedCapability`, 매니페스트 reader | AssetSystem A1 | **구현** | P6b | A0-O | 5.2 |
| A2 | `ProjectLayout` | AssetSystem A2 | **구현**(A2a `AssetDatabase/ProjectLayout.h`) | P5, SH1 | A1 | 5.2 |
| A3 | 에셋 너겟 | AssetSystem A3 | **구현**(2026-11-23, `Developer/AssetNugget`) | P5 | A2 | 5.2 N1–N3 |
| A4 | Cooker·TargetProfile·쿠킹 매니페스트 | AssetSystem A4 | 미착수 | P6b | A3 | 5.2 K1–K3 |
| P1 | 자식 프로세스·pipe | PlatformLayer | **구현** | SH1 | — | 5.3 |
| P2 | 바인딩 규약(GLES 포함) | 이 문서 | 설계 | SH2 | — | 7절 |
| P3 | ThirdParty Engine 루트·`binary`·`headers` 종류·저장소·오프라인 도구 빌드 | 이 문서 | **구현**(Windows, Release 게시 완료. macOS 산출물 남음) | P4, P9, P12, SH0 | — | 8절 |
| P4 | 도구 표 생성·binary 의존 정책·Release 배제 검사 | 이 문서 | **구현**(Windows. macOS 미검증, 알려진 Release 예외 2건은 SH2까지) | SH0, SH2 | P3, P10 | 9절 |
| P5 | 셰이더 job 키 | 이 문서 | 설계(개정 23, A3 API에 맞춤) — 구현은 SH1b | SH1 | A2, A3, P6a | 10절 |
| P6a | 형식 정의 스키마와 파일, 엔진 컴파일 프로필 | 이 문서 | **구현**(2026-11-17, Windows 시험) | P5, P14, P6b, SH1 | A0-C(JSON), 결정 2·3·5(결정됨) | 12.1절 |
| P6b | TargetProfile 필드·매니페스트 v2·엔진 내장 셰이더 라이브러리 | 이 문서 | 설계 | SH2 | A1, A4, P6a, P14 | 12.3절 |
| P7 | MetaCompiler 셰이더 파라미터 | 이 문서 | 설계 | SH3 | B1–B8 | 13절 |
| P8 | 다른 문서 개정 | 이 문서 | 목록 | 모든 단계의 기준 | 결정별 기한(15절) | 14절 |
| P9 | Vulkan 선행 | 이 문서·[VulkanGraphics](VulkanGraphics.ko.md) | **구현**(Windows. VulkanGraphics 개정 1, Android는 교차 빌드만) | SH2 | P3, P10 | 5.3 |
| P10 | 동적 라이브러리 API | PlatformLayer | **구현** | P4, P9, P13, SH0, SH2 | — | 5.3 |
| P11 | OpenGL ES 선행(OpenGLGraphics를 UE `OpenGLDrv` 방식으로) | [OpenGL ES backend](OpenGLESGraphics.ko.md)·이 문서 | **구현**(2026-11-17, Windows 미리보기. Android는 컴파일만, 실행은 P13) | P13, SH2, SH6 | A0-G | 5.3 |
| P12 | macOS 실행 기반(Objective-C++ 빌드·Cocoa 창·Metal layer) | PlatformLayer·Rocks.Build | 미설계 | P15, SH5 | P3 | 5.3 |
| P13 | Android 실행 기반(앱·창·EGL surface·수명·APK) | PlatformLayer | 미설계 | SH6 | P10, P11, AssetSystem APK 읽기(VFS) | 5.3 |
| P14 | 셰이더 타입·퍼뮤테이션·라이브러리 형식 | 이 문서 | 설계 | SH2, P6b | P6a | 12절 |
| P15 | D3D11Graphics·MetalGraphics 설계 | EngineRuntime | D3D11 **구현**(10.2절), Metal 미설계 | SH5 | P12(Metal) | 5.3 |

선행 작업 뒤 첫 단계인 SH0(16절)은 **Windows 부분 구현**(2026-11-17, `feature/shader-tools`, `Programs/ShaderToolSpike`)이다. 형식별 산출·검증·리플렉션 덤프·`ROCKS_IMAGE_FORMAT` 확인과 Windows에서 할 수 있는 측정(S2·S3·S6·S8·S12 데스크톱)을 마쳤고, macOS 측정(S1 macOS DXC 서명, S3 호스트 사이, S9)이 남았다. 결과는 5.4절, 진척은 16절이다.

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
- **A3** — **반영됨**(AssetSystem 9.1절 결정 2·5·15, A3 구현 2026-11-23). 아래는 요구 원문이고 셰이더 쪽 대응은 10절(개정 23)이다: N1은 `WriteNuggetKeyHeader` 공통 머리와 `WriteNuggetEncoder` 목록, 키를 Record 키로 그대로 쓰는 규칙(결정 2 끝)으로, N2는 Record 값 표(ValueId 행)로, N3은 세대 Guid 둘(결정 4)·`.rnug` v2(결정 6)·일괄·비동기 요청(결정 12)·bucket `shader`(`NuggetBucketShader`)로 들어갔다.
  - N1. 에셋이 아닌 처리기도 공통 키 항목을 쓰게 한다. AssetSystem 9절 키 항목(번호는 이 문서 10절 표가 붙인 것) 가운데 1–5·8–9항(형식 버전, 처리기 Guid·버전 Guid, 외부 도구, 결정적 수학 버전, 툴체인 지문, 설정, 프로파일)은 공통 머리로 쓰고, 에셋 전용인 6·7·10항(TypeId·입력 bulk·의존 목록)은 처리기가 자기 입력으로 바꿔 쓸 수 있게 한다. 에셋이 아닌 처리기는 AssetId·Build 의존이 없으므로 SCC 식(구성원 key = SHA-256(SCC key, AssetId))을 쓰지 않고 키 = SHA-256(`BuildKey` 바이트)로 한다. 셰이더의 대응은 10절 표다.
  - N2. 값은 여러 부분(`.rnug` 부분 구조)이다: 바이트코드·정규화 바인딩 layout·GLES 복사 표·진단·디버그 심볼.
  - N3. Rockpile이 요구한 AssetSystem 9절 개정(`Rockpile.ko.md` 17절: 키의 에셋 너겟 세대 Guid 11.4절, `.rnug` v2 5.2절, 일괄·비동기 API 12.2절)이 A3에 반영되면 셰이더 키·값도 그대로 따른다. 셰이더 bucket은 `shader`다(`Rockpile.ko.md` 5.1절).
- **A4**
  - K1. TargetProfile에 `graphicsBackends`(복수, 순서 = 런타임 선택 순서)·`shaderFormats`(backend마다 선호 순서가 있는 형식 목록) 필드와 엔진 기본 `MacOS` 프로필을 둔다(12절). 지금 AssetSystem 10절은 "그래픽 backend"를 이름 없는 단일 항목으로 적고 기본 프로필은 Windows·Android뿐이다.
  - K2. 쿠킹 매니페스트 v2에 에셋이 아닌 산출물 항목을 둔다(12절). v1 항목은 AssetId 단위다(AssetSystem 10절).
  - K3. 엔진 내장 셰이더 라이브러리를 프로젝트·너겟·네트워크 없이 제공하는 경로를 둔다. 계획 9.6절이 "에셋 너겟·네트워크·프로젝트 없는 설치에서도 최소 자원을 제공한다"고 요구한다. 라이브러리를 만들 때 쓰는 엔진 컴파일 프로필은 P6a가 정하고(12.1절), A4는 설치물에 라이브러리를 두고 여는 경로만 맡는다(12.3절).

- **텍스처 스트리밍**([텍스처 스트리밍](TextureStreaming.ko.md) 결정 T6, 2026-11-19 확정): 스트리밍 텍스처의 명시 LOD 표본과 정수 `Load`는 셰이더 helper를 거친다. helper는 `textureSize(t, 0)`로 상주 base mip을 계산해 논리 mip을 물리 mip으로 바꾼다(그 문서 5.3절). helper 없이 명시 LOD·`Load`를 쓰는 셰이더가 스트리밍 텍스처 멤버를 읽으면 워커가 진단을 내고, 그 텍스처는 "전체 상주 전용"으로 표시해 비스트리밍으로 돌린다. helper는 셰이더 표준 include에 두며 TS2(mip 전송)와 함께 구현한다.

### 5.3 셰이더 전용 선행 작업

규칙이 긴 작업(P2–P7)은 해당 절에 상세를 두고 여기에는 요약만 둔다.

**P1 자식 프로세스·pipe — 구현(2026-11-13).**
- `Platform/Process.h`: 시작·대기·종료 코드·자손까지 강제 종료(Windows job object, macOS 프로세스 그룹)·시간 제한 pipe·`RunProcess`·`CurrentProcessId`·`IsProcessRunning`·입력 pipe EOF lifeline([플랫폼 계층](PlatformLayer.ko.md) "자식 프로세스와 pipe").
- 검증: Windows Debug·Asan, macOS CI(`b1d316f`), Android 에뮬레이터(Unsupported 경로).
- 남은 제약: Android `Start`는 Unsupported다(워커는 개발 호스트에서만 돈다). 환경 변수 지정이 없어 부모 환경을 물려준다(필요해지면 PlatformLayer 후속).

**P2 바인딩 규약 — 설계(7절).**
- 산출물: 7.2절 규칙, `ShaderBindingLayout` 형식, GLES 상수 버퍼 에뮬레이션 규약.
- 완료 기준: 측정에 기대지 않는 규칙이 확정되고 7.3절 시험이 SH2·SH3·SH5·SH6 완료 기준에 들어 있다. 행렬 규약(7.2절 6항)의 형식별 확인과 backend별 앞면 매핑(8항)은 측정 S5로 확인하는 SH2 산출물이다.

**P3 ThirdParty 확장과 오프라인 도구 빌드 — 설계(8절).**
- 산출물: Engine 루트, `kind: binary`·`kind: headers`, 저장소에 커밋하는 도구 공개 헤더, 사용자 단위 내용 주소 저장소와 bootstrap 명령, 라이브러리별 오프라인 빌드 스크립트와 수동 CI workflow, FXC 고정본, Apple 도구 최소 버전, SPIRV-Reflect(C 원본)·Vulkan-Headers(headers).
- 막는 것: SH0.

**P4 도구 표·binary 의존 정책·Release 배제 — 구현(9절).**
- 산출물: manifest에서 생성한 도구 표 모듈(메인 프로세스 키 계산·워커 로드 공용), binary 로드 정책, Release 배제 정책 검사.
- 의존: P3, P10. 막는 것: SH0(도구 표), SH2(Release 배제 판정).
- 달성(2026-11-17, `feature/shader-tools`): 9절 진척.

**P5 셰이더 job 키 — 설계(10절, 개정 23에서 A3 API에 맞춤).** 의존: A2, A3(N1–N3), P6a, A0-C·B7(모두 구현). 막는 것: SH1. 구현은 SH1b다(11.1절).

**P6a 형식 정의와 엔진 컴파일 프로필 — 구현(2026-11-17, 12.1절 진척).** 형식 정의 스키마와 `Engine/Config/ShaderFormats/<id>.json` 파일, 호스트별 엔진 컴파일 프로필(`Engine-<호스트>.json`)이다. job 키 3·9항이 형식 정의 SHA-256과 프로필을 쓰고 워커가 target env·ESSL 버전·상한을 읽으므로 P5·SH1 앞이다. 의존: A0-C JSON(구현), 결정 2·3·5.

**P6b TargetProfile 필드·매니페스트 v2·엔진 내장 라이브러리 — 설계(12.3절).** 의존: A1, A4(K1–K3), P6a, P14. 막는 것: SH2(런타임이 쿠킹된 라이브러리를 연다).

**P7 MetaCompiler 셰이더 파라미터 — 설계(13절, 15절 결정 4 확정).** 의존: B1–B8(구현). 막는 것: SH3. P7 전에는 손으로 쓴 레이아웃 표를 쓴다(7.2절 5항).

**P8 다른 문서 개정 — 목록(14절).** AssetSystem 쪽(5.2절 요구)은 해당 단계(A0-G·A1·A2·A3·A4) 착수 전에, 나머지는 15절의 결정별 기한에 맞춰 한다. 측정 뒤에 정하는 결정(3·5·6)에 걸린 행은 그 결정 뒤에 고친다.

**P9 Vulkan 선행 — 구현(Windows, 2026-11-17).**
- Vulkan-Headers: `kind: headers`(P3)로 Engine 루트에 둔다.
- 로더: import library를 링크하지 않고 런타임에 연다(P10, UE와 같다, 1절). Windows `vulkan-1.dll`은 드라이버·Vulkan 런타임이 설치한다. 없으면 VulkanGraphics 생성이 `UnsupportedCapability`로 실패하고 다른 backend는 그대로 동작한다.
- 검증 레이어: D3D12 디버그 레이어에 대응하며 같은 조건(EngineRuntime 10.1, 장치 생성의 `enableValidation`)에서 켠다. 공급(Khronos Vulkan-ValidationLayers 릴리스를 binary 항목으로 받거나 오프라인 빌드)은 측정 S4로 정한다.
- surface: Windows `VK_KHR_win32_surface`에 PlatformWindow의 네이티브 창 핸들을 쓴다. Android surface는 P13이다.
- VulkanGraphics 설계: [VulkanGraphics](VulkanGraphics.ko.md)(디바이스·큐·메모리·파이프라인 layout(7.2절 4항)·검증 오류를 `ValidationErrorCount`로 집계). 내용이 커서 EngineRuntime 절 대신 따로 둔다.
- CI: GPU 없는 runner의 소프트웨어 ICD(lavapipe·SwiftShader)는 측정 S4다. 안 되면 CI에서 **NOT RUN**으로 보고하고 로컬 결과를 기록한다.
- macOS는 Metal을 쓰므로 Vulkan(MoltenVK 등)은 범위 밖이다.
- 완료 기준: Vulkan-Headers가 Rocks 경고 규칙으로 include되고, 로더 동적 로드와 검증 레이어 켜기가 Windows에서 시험되며, VulkanGraphics 설계 절이 확정된다.
- 달성(2026-11-17, 사용자 지시 "Vulkan까지는 작업을 미리하자"): 설계를 넘어 backend 자체를 SH2 전에 구현했다(`VulkanGraphics`, 개발용 컴파일 훅 `ShaderFormatVulkan`). 로더 동적 로드·검증 레이어 켜기는 EngineTests `VulkanDevice_LoadsTheSystemLoaderAndCreatesADevice`가, Graphics 계약 전체는 Sandbox self-test가 GTX 1060과 lavapipe에서 확인한다(D3D12와 픽셀 차이 0). 측정 S4 결과는 5.4절, 상세는 VulkanGraphics 17절이다. SH2에 남은 Vulkan 일은 쿠킹된 `VULKAN_SM5` SPIR-V와 리플렉션 머리를 받는 경로, `ROCKS_ROOT_CONSTANTS` 전개(지금은 `cbuffer DrawConstants` 이름 규칙), 단계 사이 변수 location 재작성, 7.3절 시험의 Vulkan 몫이다. Android 실행은 SH6·P13이다.

**P10 동적 라이브러리 API — 구현(2026-11-16, PlatformLayer P1 표의 "동적 모듈" 항목, 상세: PlatformLayer "동적 라이브러리" 절).**
- 구현 전: 공개 로더가 없고 private 코드가 OS 함수를 직접 불렀다(XInput, `Win64Window.cpp`). 지금은 XInput도 `OpenSystem`으로 연다. 계획 §2.5의 "동적 loader를 Foundation 선행 조건으로 만들지 않는다"는 native plugin 모듈 이야기다. 이 API는 Foundation이 아니라 셰이더 워커와 VulkanGraphics의 선행 조건이고, 엔진 모듈을 동적으로 싣는 로더가 아니다.
- 산출물 `Platform/DynamicLibrary.h`:
  - `Open(absolutePath)`: 절대 경로만 받는다. Windows `LoadLibraryExW`(`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`, 현재 디렉터리·PATH를 찾지 않고 같은 디렉터리의 의존 DLL은 찾는다), macOS·Android `dlopen(RTLD_NOW | RTLD_LOCAL)`.
  - `OpenSystem(name)`: OS가 공급하는 라이브러리만 이름으로 연다(Windows System32의 `vulkan-1.dll`·XInput, Android `libvulkan.so`).
  - `Find(symbol)`, 수명, 오류 변환(파일 없음 `NotFound`, 심볼 없음, 잘못된 경로 `InvalidArgument`). 수명은 PlatformLayer "동적 모듈" 검증 기준("참조 중 unload 거부")을 따른다: 얻은 심볼 표를 lease로 세고, lease가 남아 있으면 `Close`가 `Busy`로 거부한다.
- 완료 기준: 파일 없음·심볼 없음 오류, 현재 디렉터리에 둔 같은 이름 DLL을 열지 않는 음성 대조, 같은 디렉터리 의존 DLL 해결, Windows Debug·Asan·macOS CI·Android 에뮬레이터 시험. XInput 직접 로드를 이 API로 옮긴다.
- 달성(2026-11-16): Windows Debug·Asan에서 위 항목과 PATH 음성 대조·긴 경로를 확인했고 XInput을 옮겼다. 시험 DLL은 시험 지원 코드가 실행 중에 쓰는 데이터 전용 PE다(8절대로 Rocks.Build에 공유 라이브러리 종류를 더하지 않았다). Android 에뮬레이터는 OS 공급 라이브러리 시험만 돌고 의존·검색 시험은 SKIP, macOS는 CI 미확인이다. 두 플랫폼의 시험 라이브러리 생성기(Mach-O 코드 서명, ELF)는 후속이다.
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
- 달성(2026-11-17, `feature/gles-graphics`, 설계·상태는 [OpenGL ES backend](OpenGLESGraphics.ko.md)): 장치 동작을 ES 3.2 core 함수 표로 하는 공통 층과 플랫폼 층(Windows WGL, Android EGL)으로 나눴다. 데스크톱 전용 호출(`glClipControl`·`GL_FRAMEBUFFER_SRGB` 켜기·WGL sRGB 픽셀 형식)은 Windows 층에만 있고 DSA는 없앴다. Windows 미리보기는 ES 3.2 profile context를 먼저 쓰고 데스크톱 GL 4.3 core로 내려갈 수 있다(위 "GL 4.3 core 이상"과 다른 점, 측정은 그 문서 2.2절). 클립 공간은 창·RenderTarget 모두 `GL_UPPER_LEFT`이고 스왑체인은 offscreen 텍스처에 그린 뒤 Y 반전 blit으로 표시한다(7.2절 8항을 SH2보다 앞당겼다). 그리기 상수는 루트 상수 에뮬레이션 배열(`vu_h`·`pu_h`, 7.2절 4항)로 올리고 Sandbox 원문은 쿠킹된 `GLES31_PC`와 같은 모양의 GLSL ES 3.20이다. Sandbox self-test가 두 context에서 통과하고 D3D12와 픽셀 차이 0이다. Android 교차 빌드에서 EGL 층이 컴파일·링크된다. A0-G 형식 확장 전이라 형식 표는 지금 형식만 두고 확장 조건 열을 미리 두었다.

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
- 산출물: 셰이더 타입 등록(이름·`.rsf` 경로·엔트리·단계·요구 기능, 2절), 퍼뮤테이션 차원 선언과 ID 계산, 형식·퍼뮤테이션 거르기 규칙, 라이브러리 항목 키(셰이더 타입·퍼뮤테이션 ID·단계 → 코드 SHA-256)와 파일 형식·버전, 런타임 조회와 빠졌을 때의 `IncompatibleCook`. 개정 16에서 정점 입력 타입 축, 머티리얼 셰이더 표지, 거르기 서명과 형식 사실 조회, 퍼뮤테이션 수 보고를 더했다(12.2절).
- 완료 기준: 등록·퍼뮤테이션 ID의 결정성 시험(등록 순서와 무관), 같은 코드의 중복 제거, 라이브러리 왕복·손상 검출, 빠진 항목 조회의 `IncompatibleCook`.
- 의존: P6a. 막는 것: P6b, SH2.

**P15 D3D11Graphics·MetalGraphics 설계 — D3D11 구현, Metal 미설계(EngineRuntime).**
- 산출물: VulkanGraphics 절(P9)과 같은 수준의 backend 설계 절(장치·스왑체인·자원·파이프라인 layout(7.2절 4항)·검증 레이어·self-test 대상)이다. D3D11은 Windows, Metal은 P12 위에 선다.
- D3D11(2026-11-17): 설계는 EngineRuntime 10.2절이다. 사용자 결정으로 SH5의 D3D11 부분 가운데 backend 자체를 앞당겨 구현했다(`D3D11Graphics`, 실행 중 `D3DCompile` SM 5.0). SH5에 남은 D3D11 일은 쿠킹된 `D3D11_SM5` 코드를 받는 경로(SH2 API 위), 그리기 상수 b13 배치(7.2절 4항), 7.3절 (c)·(g)의 D3D11 대조다.
- 막는 것: SH5.

### 5.4 측정

결과에 따라 설계가 갈리는 값은 짐작하지 않고 SH0(또는 해당 단계)에서 잰다.

| ID | 측정 | 결과가 정하는 것 | 결과(SH0, 2026-11-17) |
|---|---|---|---|
| S1 | DXC 1.8.2505 내장 validator 서명(Windows·macOS) | 10절 서명 검사, `D3D12_SM6` 생산 호스트 | Windows: DXC 1.9.2609(내장 validator)가 Renderer VS·PS와 fixture DXIL 4개를 모두 서명했다(container digest가 0도 preview 우회 값도 아님). macOS: macOS 필요, 미측정 |
| S2 | 개발 기계 GPU와 WARP의 D3D12 최대 셰이더 모델, Windows 10 Agility SDK 요건 | 결정 3 | GTX 1060 3GB(드라이버 32.0.15.6094)·WARP(10.0.26100.9818) 모두 최대 SM **6.8**, feature level 12.1, resource binding tier 3. Agility SDK 없이 OS 런타임(Windows 11 build 26200, `D3D12Core.dll` 10.0.26100.9818)에서 6.6 이상이 된다. Windows 10 요건은 Windows 10 기계가 없어 미측정 |
| S3 | 결정성: 같은 호스트 반복, Debug·Release 워커, 작업 디렉터리 변경, FMA3 유무, OS 버전, **Windows·macOS 사이**(DXIL·SPIR-V·ESSL·MSL 텍스트)에서 형식별 바이트 동일 | 10절 결정성 | Windows 부분 byte 동일: 같은 입력 반복(Debug 2회·Release 3회), 작업 디렉터리 변경(Debug·Release 각 1회), Debug·Release 스파이크 빌드 사이에서 manifest의 산출물 44개(DXIL·SPIR-V·DXBC·ESSL·MSL·HLSL 5.0·리플렉션 덤프)가 모두 같다. 상수 초월 함수 fixture는 DXC가 DXIL·SPIR-V 모두 상수로 접었고(최적화 전 SPIR-V에도 `OpExtInst` 0), 접은 8개 값은 이 호스트 UCRT double 함수 결과를 float로 반올림한 값과 같다. FMA3 유무·OS 버전·Windows–macOS는 미측정(기계 하나) |
| S4 | Vulkan 검증 레이어 공급, GPU 없는 CI의 소프트웨어 ICD | P9 | 로컬 결과(2026-11-17, VulkanGraphics 17절): 검증 레이어는 LunarG Vulkan SDK 1.4.363.0 설치로 공급했고 CI 공급 방식은 SH2 때 정한다. 소프트웨어 ICD는 lavapipe(Mesa 26.2.7 `mesa-dist-win`, `VK_ADD_DRIVER_FILES`)로 self-test 전체가 통과한다(calibrated timestamps의 QPC domain이 없어 calibration은 `Unsupported`). SwiftShader는 Windows 공식 바이너리가 없어 쓰지 않았다. CI 연결은 아직이다 |
| S5 | D3D12·Vulkan·GLES 미리보기 픽셀 대조(행렬 옵션·앞면·Y 반전·루트 상수 대응) | 7.2절 6·8항 | C7 런타임 경로(2026-11-17, Windows, GTX 1060·WARP·NVIDIA OpenGL 4.5, Sandbox self-test `RunCullingChecks`). 행렬: D3D12(`D3DCompile` + `D3DCOMPILE_PACK_MATRIX_ROW_MAJOR`)·WARP·OpenGL(`transpose = GL_TRUE`) 모두에서 비균등 scale 뒤 이동한 비대칭 행렬의 사각형이 기대 영역(64칸 격자의 열 32–63·행 8–23)만 덮는다. 앞면: D3D12 `FrontCounterClockwise = TRUE`, OpenGL은 창(`GL_LOWER_LEFT`)·RenderTarget(`GL_UPPER_LEFT`) 모두 `glFrontFace(GL_CCW)`에서 화면 반시계가 앞면이다. None·Back·Front 컬, 음수 scale·거울 view 반전, 둘 다 음수일 때 원래대로가 세 장치에서 기대대로이고, 창 경로(`CopyPassTarget`)와 offscreen 경로의 이미지가 byte 단위로 같다. 창을 `GL_CW`로 둔 음성 대조는 창 경로 6건 불일치로 실패한다. 기존 self-test 픽셀 검사와 backend 간 대조도 그대로 통과한다(차이 0). Vulkan·GLES 미리보기·쿠킹 경로(DXC `-Zpr`)와 루트 상수 대응은 SH2에서 잰다 P11(2026-11-17): GLES 미리보기 런타임 경로(ES 3.2 profile·GL 4.3 core 두 context, 손으로 쓴 GLSL ES 3.20)에서 같은 검사가 통과하고 D3D12와 픽셀 차이 0이다. 창·RenderTarget 모두 `GL_UPPER_LEFT` + `GL_CCW`, 스왑체인은 offscreen에 그린 뒤 Y 반전 blit. 음성 대조: Y 반전을 빼면 창 경로 7건, `GL_CW`면 컬 검사 6건 불일치 |
| S6 | 도구 빌드 컴파일러(clang-cl·MSVC)·빌드 시간·바이너리 크기 | 8절 4항 | P3 인용: Windows x64 도구는 Visual Studio C++ 도구(vcvarsall x64, MSVC, DLL 링커 버전 14.29)와 CMake·Ninja Release, 업스트림 빌드 설정으로 만들었다. 크기: `dxcompiler.dll` 21,943,296 B, `spirv-cross-c-shared.dll` 2,305,536 B, `SPIRV-Tools-c.dll` 3,644,416 B(PDB 별도). 빌드 시간은 P3에서 기록하지 않았고 clang-cl 비교와 macOS는 미측정 |
| S7 | 자체 전처리 결과로 컴파일한 출력 = DXC 직접 전처리 출력(`#line` 포함) | 결정 6 구현의 정합성(SH1 완료 기준) | SH1a(2026-11-23, Windows, DXC 1.9.2609, `ShaderPreprocessS7_*`): fixture(상대·절대 include, `#pragma once`·include guard, `##`·가변 인자·여러 줄 호출, `ROCKS_*` 조건·`defined`·`true`, `ROCKS_IMAGE_FORMAT`, `__LINE__`, 여러 줄 주석, job 정의)에서 (가) Rocks 전처리 결과 컴파일과 (나) DXC 직접 전처리 컴파일의 산출물이 byte 단위로 같다: `D3D12_SM6` `cs_6_6` DXIL 3,508 B, `VULKAN_SM5` `cs_6_0` SPIR-V 1,164 B. job 정의 값 하나만 바꾼 음성 대조는 다른 byte다. 일부러 넣은 오류의 첫 진단 위치는 두 방식 모두 `/Plugin/S7/Lib/Broken.rsh:4`다. `#if`의 내장 이름은 Rocks 쪽 오류다. macOS는 미측정(DXC 호출 구현이 Windows만 — 11.1절 SH1a 구현 메모) |
| S8 | 워커 기동·DLL 로드 비용, SHA-256 처리량 | 11절 묶음 크기 | 측정값만(Release 스파이크 3회, i5-13600KF): `ShaderToolSet::Open` 82.5 ms 가운데 runtime 파일 31.3 MiB의 SHA-256 대조가 약 74 ms(Core SHA-256 423–426 MiB/s)이고 나머지 로드는 약 9 ms. 첫 DXIL 컴파일 5.6–6.4 ms(다음 3.6–3.7 ms), 첫 SPIR-V 3.6–4.3 ms(다음 2.9–3.1 ms), SPIRV-Cross ESSL 첫 0.34–0.36 ms(다음 0.07–0.10 ms), 첫 `spirv-val` 0.43–0.46 ms. Debug 빌드는 SHA-256 26–27 MiB/s, 열기 1.2 s. 워커 프로세스 기동은 워커가 없어 미측정 |
| S9 | Apple `metal` 재배포 조건, Windows용 Metal Developer Tools(UE 5.8 기대 버전 5.3, `Developer/Apple/MetalShaderFormat/Private/MetalShaderFormat.cpp:724–725`) | 결정 5 | macOS 필요, 미측정 |
| S10 | `d3dcompiler_47.dll` 재배포 조건과 고정 버전 | 8절 2항 FXC | 문서 조사(2026-11-20): Windows SDK 사용 조건 2.a "Distributable Code"가 가리키는 REDIST.TXT 목록(`learn.microsoft.com/legal/windows-sdk/redist`, 2024-10-21 갱신)에 `Redist\D3D\x64\d3dcompiler_47.dll`이 있다(x86과 `d3dcsx_47.dll`·`dxcompiler.dll`·`dxil.dll`도 같은 묶음). 조건: 고치지 않은 파일만, **Classic Windows(데스크톱) 응용과 함께만** 재배포하고 UWP 앱에는 넣지 않는다. 사용 조건의 배포 요건이 붙는다(프로그램에 상당한 주 기능을 더할 것, 배포자·최종 사용자가 이 조건 이상으로 보호하는 조건에 동의할 것, 자기 저작권 표시, Microsoft 면책, Microsoft OS 밖 플랫폼용 배포 금지, 저작권·상표 고지 변경 금지). 고정 버전: 저장소 manifest(`Engine/Source/ThirdParty/D3DCompiler/ThirdParty.json`)의 SDK 10.0.22621.0 파일 버전 10.0.22621.2428(4,916,728 B, SHA-256 `af7b99be…`)이 이 기계 설치본과 크기·SHA가 같다. 같은 기계 System32 본은 10.0.26100.9974라 OS 빌드마다 다르다는 전제도 맞다. UE 5.8 동봉본은 10.0.19041.1(GitDependencies, `Engine/Build/Commit.gitdeps.xml:39300`)이다. 결론은 8절 2항 |
| S11 | Android 에뮬레이터·CI의 OpenGL ES 3.2 context | SH6 검증 경로 | — |
| S12 | (SH0) 데스크톱 GL 드라이버의 `#version 320 es` 수용(`GL_ARB_ES3_2_compatibility` 유무), (SH6) UE 수정 pass 없이 업스트림 SPIRV-Tools·SPIRV-Cross만으로 만든 ESSL이 Android 기기 드라이버에서 도는지 | P11, 7.2절 11항 | 데스크톱(GTX 1060 3GB, NVIDIA 560.94): GL 4.6 core context에 `GL_ARB_ES3_2_compatibility`·`GL_ARB_ES3_1_compatibility`·`GL_ARB_clip_control`이 있고 GLSL 목록에 `320 es`가 있다. `GLES31_PC` Renderer VS·PS 원문이 경고 없이 컴파일·링크된다. `WGL_EXT_create_context_es2_profile`로 만든 ES 3.2 context에서도 같다. AMD·Intel 드라이버는 미측정. 기기(SH6)는 미측정 P11(2026-11-17) 추가: ES 3.2 profile context는 `GL_EXT_clip_control`·`GL_EXT_disjoint_timer_query`·`GL_KHR_debug`·`GL_EXT_sRGB_write_control`과 ES 형식 확장을 공개하고, 4.3 core 요청은 4.3.0을 준다([OpenGL ES backend](OpenGLESGraphics.ko.md) 2.2절) |
| S13 | SPIRV-Cross가 HLSL `Buffer<T>`·`RWBuffer<T>`(typed buffer)를 ESSL `samplerBuffer`·`isamplerBuffer`·`usamplerBuffer`와 `texelFetch`, `imageBuffer`와 `imageLoad`·`imageStore`로 내는지, 결합 sampler 목록에 typed buffer SRV가 들어가는지 | 7.2절 4항 GLES typed buffer 규칙(Renderer RQ10) | Windows(2026-11-20, SPIRV-Cross C API 0.68.0, `TypedBuffer.ps`·`TypedBuffer.cs` fixture): ESSL 320(`GLES31_PC`·`GLES31_ANDROID` 같음)에서 `Buffer<float4>`·`Buffer<int4>`·`Buffer<uint>`가 `samplerBuffer`·`isamplerBuffer`·`usamplerBuffer`와 `texelFetch`로, `RWBuffer<float4>`(rgba8 지정)·`RWBuffer<uint>`가 `layout(rgba8) writeonly imageBuffer`·`layout(r32ui) uimageBuffer`와 `imageLoad`·`imageStore`로 나온다(읽지 않는 UAV에 `writeonly`가 붙는다). 결합 sampler 목록(`spvc_compiler_get_combined_image_samplers`)에는 텍스처·샘플러 짝(`albedo`·`albedoSampler`)만 있고 typed buffer SRV는 **들어가지 않는다**. typed buffer는 SPIR-V binding을 그대로 단 `layout(binding = n)`으로, 결합 sampler는 binding 없이 id 이름(`_57`)으로 나온다. MSL 2.1은 기본값이면 `texture2d`와 좌표 변환 함수로 흉내 내고 `texture_buffer_native`를 켜면 `texture_buffer<T>`(UAV는 `access::write`·`access::read_write`)다. DXIL·SPIR-V(`spirv-val`) 모두 통과. 결론은 7.2절 4항 |
| S14 | VS 출력 `SV_RenderTargetArrayIndex`·`SV_ViewportArrayIndex`의 변환: DXC DXIL(SM 6.6), DXC SPIR-V(target env 1.1에서 `ShaderViewportIndexLayerEXT` capability·`SPV_EXT_shader_viewport_index_layer` 확장으로 나오는지), SPIRV-Cross MSL(`[[render_target_array_index]]`·`[[viewport_array_index]]`), FXC `vs_5_0`이 받는지 | 7.2절 7항 VS layer·viewport 출력 규칙의 형식별 `vertexViewportLayerOutput`(Renderer RQ14), `D3D11_SM5` 포함 여부 | Windows(2026-11-20, `LayerViewport.vs` fixture: layer·viewport를 함께 낸다): DXC DXIL은 SM 6.6과 6.0 모두 받고 서명되며 요구 플래그 `D3D_SHADER_REQUIRES_VIEWPORT_AND_RT_ARRAY_INDEX_FROM_ANY_SHADER_FEEDING_RASTERIZER`(0x2000)가 선다. DXC SPIR-V(vulkan1.1)는 capability `ShaderViewportIndexLayerEXT`(5254)와 확장 `SPV_EXT_shader_viewport_index_layer`를 내고 `spirv-val`을 통과한다. SPIRV-Cross MSL 2.1은 `[[render_target_array_index]]`·`[[viewport_array_index]]`, HLSL 5.0은 `SV_RenderTargetArrayIndex`·`SV_ViewportArrayIndex`다. ESSL은 만들어지지만 `#extension GL_NV_viewport_array2 : require`가 붙는다. FXC `vs_5_0`은 원문과 SPIRV-Cross HLSL 5.0 출력을 모두 받고 같은 요구 플래그를 세운다. 결론은 7.2절 7항(`D3D11_SM5`에도 넣는다, 개정 21) |

SH0 남은 측정(2026-11-20): Windows에서 할 수 있는 측정은 모두 끝났다. 남은 것은 macOS가 필요한 S1(macOS DXC 서명)·S3(Windows–macOS 사이 byte 비교)·S9, 기계가 하나라 못 잰 S2(Windows 10 요건)·S3(FMA3 유무·OS 버전)·S12(다른 GPU 공급자), 다른 단계 몫인 S5(Vulkan·GLES 미리보기 쿠킹 경로와 루트 상수 대응, SH2)·S7(SH1 완료 기준)·S11(SH6, P13 미설계)·S12(기기, SH6)다.

## 6. 전체 순서

고정 순서(사용자 지시): A0-B → A0-O → A0-G → A1–A6 → 화면 마일스톤. A0-B는 B8b(`e405c37`)로 끝났다. 셰이더 시스템은 이 순서를 바꾸지 않고 다음 자리에 들어간다.

1. **지금**: 이 문서를 스펙으로만 둔다.
2. **A0-G·A1·A2·A3·A4 착수 전**: 5.2절 셰이더 요구(G1–G3, A1 ErrorCode, A2 `ProjectLayout` 위치, N1–N3, K1–K3)를 AssetSystem에 반영한다(P8의 AssetSystem 부분). A0-O에 넣을 셰이더 요구는 없으므로 가장 가까운 기한은 A0-G 착수 전이다. 반영하지 않고 그 단계를 구현하면 셰이더 착수 때 그 단계를 다시 고쳐야 한다.
3. **셰이더 구현 착수**: A6 뒤다. 의존만 보면 A4 뒤면 되지만(P5가 A2·A3을, P6b가 A1·A4를 요구한다) 고정 순서가 A1–A6을 한 묶음으로 두기 때문이다. 결정 11(2026-11-16, (나))에 따라 셰이더 구현은 화면 마일스톤 안에서 렌더 기반보다 먼저 한다([렌더 기반](RenderBase.ko.md) 18.1절).
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

1. **선언.** 셰이더 원문은 register·`[[vk::*]]`를 적지 않는다(UE의 일반 파라미터와 같다). 예외는 `Platform.rsh`가 형식별로 전개하는 매크로 둘이다: 루트 상수(2항)와 storage image 형식 `ROCKS_IMAGE_FORMAT(<형식>)`. DXC는 `RWTexture2D<float4>`의 SPIR-V 형식을 원소 타입으로 추론하므로(예: Rgba32f) 실제 형식이 다르면 Vulkan 검증 오류가 나고, ESSL image는 형식 한정자가 필요하다. 이 매크로는 SPIR-V 형식에서 DXC의 image format 지정으로, D3D 형식에서는 빈 것으로 전개한다. DXC가 지정을 지원하는지는 SH0에서 확인하고, 안 되면 워커가 SPIR-V `OpTypeImage`의 형식 operand를 고친다. SH0 확인(2026-11-17): DXC 1.9.2609가 지원한다(`[[vk::image_format("rgba8")]]` 등 7종이 SPIR-V 형식으로 나오고, DXIL에서는 attribute를 무시하며 `-Wignored-attributes` 경고를 낸다, 16절). 그래서 D3D 형식의 빈 전개가 맞고, 셰이더 경고를 오류로 하는 정책(결정 7)에서는 D3D 형식에 attribute가 남지 않아야 한다. Vulkan 형식 정의는 `shaderStorageImageWriteWithoutFormat`을 요구하지 않는다. 정적 샘플러 같은 다른 예약 space는 후속이다. 상수는 `cbuffer`·`ConstantBuffer<T>`·루트 상수로만 선언하고, 이름 없는 전역 uniform(`$Globals`가 되는 전역 변수)은 컴파일 오류다. UE는 느슨한 파라미터를 `$Globals`에 두지만, Rocks 파라미터는 언제나 이름 있는 버퍼다(13절).
2. **루트 상수(그리기별 상수).** `ROCKS_ROOT_CONSTANTS(Type, Name)` 하나로 선언한다. 크기는 최대 128 byte(32 DWORD, Vulkan push constant 최소 보장)이고 16 byte 배수이며 멤버는 5항 규칙을 따른다.
   - **배치는 선언이 정한다**([렌더 기반](RenderBase.ko.md) 결정 R7, RenderBase 개정 21·이 문서 개정 19에서 확정). 파라미터 구조체 종류에 "그리기별 상수" 표시가 있는 구조체만 루트 상수로 가고, 표시가 없는 구조체는 크기가 128 byte 이하여도 1항의 이름 있는 cbuffer다. 표시가 있는데 128 byte를 넘으면 워커(P7 뒤에는 MetaCompiler) 오류이고 cbuffer로 옮기지 않는다. 크기 기준 자동 배치는 버렸다: 멤버 하나를 더해 경계를 넘으면 root signature·pipeline layout·GLES 복사 표·쓰는 API가 조용히 바뀐다(근거 전체는 RenderBase 9.4절).
   - 파이프라인 하나에 그리기별 상수 블록은 하나이고 모든 단계가 같은 값을 본다. 파이프라인 layout이 크기를 선언하고(Graphics `PipelineDesc::drawConstantBytes`, 0 = 없음), 장치는 단계별 리플렉션 크기가 선언보다 크면 생성을 거부한다. 값은 `CommandContext::SetDrawConstants`로 선언과 같은 byte 수만 받는다.
   - SH2 전 개발 경로에서는 원문이 `cbuffer DrawConstants : register(b0)` 이름 규칙으로 쓰고(Vulkan 컴파일 훅이 push constant로 바꾼다), C++ 쪽 고정 구조체는 없다(호출자가 자기 구조체를 둔다). 예전 고정 `DrawConstants`(80 byte: 변환 행렬 + 색)는 개정 19에서 없앴다.

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
   - **상한 검사 위치(개정 17).** 모든 상한은 job 안에서 **job 형식의 상한**으로만 검사한다: 단계별 상수 버퍼·SRV 텍스처(typed buffer SRV 포함)·샘플러·구조화·raw 버퍼·storage image 수, render target 수(PS가 쓰는 `SV_Target`의 가장 큰 번호 + 1), 단계 사이 vec4, GLES 에뮬레이션 vec4, UAV 단계, 루트 상수 크기다. 개정 13은 SRV 텍스처·샘플러를, 개정 16은 render target 수를 옮겼다([렌더러](Renderer.ko.md) 결정 RN13·RQ22·RQ14, [머티리얼 시스템](MaterialSystem.ko.md) MSR1). 개정 17에서 나머지도 옮겼다. 프로필 공통 상한(프로필 형식들의 최소값)은 셰이더 타입 단위로 계산되므로, 거르기(조합 단위, 12.2절)가 GLES에서 뺀 데스크톱 전용 퍼뮤테이션에도 GLES 상한(예: 단계 사이 vec4 15, 에뮬레이션 vec4 224)이 걸려 모든 형식에서 오류가 났기 때문이다. 원문은 형식마다 바꾸지 않으므로 같은 셰이더가 넓은 형식에서는 컴파일되고 좁은 형식(GLES·`D3D11_SM5`)에서는 그 형식 job만 오류가 될 수 있다. 정상 경로는 거르기가 그런 퍼뮤테이션을 그 형식에서 빼는 것이고(예: GBuffer +D·+E profile, Renderer 7.1절, 결정 RN3·RN4), job 형식 검사는 거르기 누락을 잡는 안전장치다. 오류에는 셰이더 타입·퍼뮤테이션·형식과, 머티리얼 job이면 머티리얼 경로·pass를 담는다. UE도 원문은 하나이고 플랫폼 샘플러 상한을 컴파일 환경에 넣어 그 플랫폼 컴파일에서만 실패시킨다(`Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp:3945–3946`, OpenGL `Developer/ShaderFormatOpenGL/Private/OpenGLShaderCompiler.cpp:868–877`). 형식 정의 SHA-256이 job 키에 있으므로(10절 3항) 그 형식의 상한이 바뀌면 그 형식 job만 다시 검사된다. 파이프라인 합계 한도(D3D12 Tier 1 UAV, Vulkan `maxDescriptorSet*`)는 파이프라인 layout을 만들 때(쿠킹 때 알려진 쌍, 런타임 생성) 검사한다. **프로필 공통 상한은 검사에 쓰지 않는다.** 남은 용도도 없다(편집기·쿠커 진단도 형식마다 계산한다, 머티리얼 시스템 5.4절 V9). 그래서 job 키 9항의 공통 상한 형식 목록을 빼고, 읽기 코드의 `CommonLimits`와 그 시험은 개정 18에서 지웠다(12.1절 진척).
   - **API 하한 표(형식 정의 초기값의 근거).** Vulkan 값은 핵심 최소 보장이다. 마지막 열 "데스크톱 공통"은 형식 정의 초기값을 정할 때 본 데스크톱 최솟값이고, 개정 17부터 검사에 쓰지 않는다(위 항). 더 큰 장치 요구는 형식 정의 `limits` 자체가 장치 요구가 된다(아래 항).

     | 종류(단계당) | D3D12(Tier 1) | D3D11(FL 11.0) | Vulkan 최소 | Metal(macOS) | 데스크톱 공통 |
     |---|---|---|---|---|---|
     | 상수 버퍼 | 14 | 14(Rocks는 13 + 루트 상수) | 12(uniform buffer) | buffer 공간 | 12 |
     | SRV 텍스처·typed buffer SRV | 128 | 128 | 16(sampled image) | 128 | 16 |
     | 구조화·raw 버퍼(SRV·UAV 합) | SRV 128, UAV는 아래 | SRV 128, UAV는 아래 | 4(storage buffer) | buffer 공간 | 4 |
     | storage image(UAV 텍스처·typed buffer UAV) | UAV는 아래 | UAV는 아래 | 4 | texture 공간 | 4 |
     | 샘플러 | 16 | 16 | 16 | 16 | 16 |
     | UAV 단계 제약 | Tier 1은 모든 단계 합 8(FL 11.0), FL 11.1 이상 64 | PS·CS만, PS는 render target과 출력 슬롯 8개를 나눈다 | VS·PS 쓰기는 선택 기능(`vertexPipelineStoresAndAtomics`·`fragmentStoresAndAtomics`), PS는 `maxFragmentCombinedOutputResources`(최소 4)에 color attachment가 들어간다 | — | **UAV는 CS만**. VS·PS UAV는 형식 정의의 기능 요구로 후속 |

     위 표는 하드웨어·API 한도다. SRV 텍스처의 **형식 정의 값**은 형식마다 다르다(개정 13, 12.1절): `D3D12_SM6`·`METAL_SM5` 128, `D3D11_SM5` 16(하드웨어는 128이지만 GLES와 같은 16장 형식으로 둔다, 결정 RN13), GLES 16, `VULKAN_SM5` 128(개정 14, GX1 측정 뒤). Vulkan을 넓히려면 단계당 한도만으로는 모자란다. set = 단계이므로(아래 Vulkan 항) `maxDescriptorSetSampledImages`(최소 보장 96)도, 그리고 단계의 모든 자원과 color attachment를 합한 `maxPerStageResources`(최소 보장 128)도 넘지 않아야 한다. 그래서 이 세 한도를 함께 장치 요구로 본다(아래 **상한은 장치 요구다**). 샘플러 상한은 모든 형식이 16으로 같아(Metal 장치 상한 16, UE `ML_MaxSamplers`, `Runtime/Apple/MetalRHI/Private/MetalPipeline.h:47`) `maxPerStageDescriptorSamplers`는 핵심 최소 보장 16 그대로이며 장치 요구를 더하지 않는다. 16장을 넘는 텍스처는 9항의 공유 샘플러를 쓴다.

   - **상한은 장치 요구다(개정 14).** 쿠킹된 코드는 바인딩 수·출력 수가 정해진 채로 나오므로, 그 수를 받지 못하는 장치에서는 실행할 수 없다. 그래서 형식 상한은 늘 장치 요구다. 형식 정의 `limits`가 그 API의 핵심 최소 보장을 넘으면, 넘는 값이 곧 장치 요구다. 별도 장치 요구 어휘(한도 이름·값)를 두지 않고, 런타임 형식 선택(12.3절 P6b, SH2)이 장치 능력(Graphics `DeviceCaps`, RenderBase GX1)과 형식 `limits`를 대조한다. 대조 규칙: 단계당 각 종류 수 ≤ 장치 단계당 한도, `renderTargets` ≤ 장치 MRT 수. Vulkan은 set = 단계이므로 `sampledTextures` ≤ `maxDescriptorSetSampledImages`, 샘플러·상수 버퍼·storage도 각 `maxDescriptorSet*` 이하, 그리고 `constantBuffers + sampledTextures + storageBuffers + storageImages + renderTargets` ≤ `maxPerStageResources`(PS 기준, Vulkan 사양의 합산 대상). 맞지 않는 장치에서는 그 형식을 쓸 수 없고 `Unsupported`다(다른 형식으로 몰래 바꾸지 않는다. 사용자 설정·대상 프로필이 다른 backend를 고를 수 있다). 장치 요구 이름 목록(`deviceRequirements`)은 버전·확장처럼 수가 아닌 요구에만 쓴다. 한 값을 두 곳에 적지 않기 위해서다.

     근거: D3D12 [하드웨어 지원 tier](https://learn.microsoft.com/en-us/windows/win32/direct3d12/hardware-support)(Tier 1 UAV는 "모든 단계의 모든 descriptor table" 합), Vulkan 사양 필수 한도(`maxPerStageDescriptor*`, Vulkan-Docs `chapters/limits.adoc`).
   - **모바일 상한.** Android 프로필(`GLES31_ANDROID`, SH6 이후 `VULKAN_ANDROID`)의 상한도 형식 정의가 가진다. GLES 형식 정의는 OpenGL ES 3.2 사양의 최소 보장과 UE 상한(셰이더당 샘플러 16, `OpenGLShaderCompiler.cpp:868–876`. 런타임 PS·VS 16, 합계 32, `Runtime/OpenGLDrv/Private/OpenGLES.cpp:160–163`) 가운데 작은 값을 P6a에서 정해 다음 항목을 가지며 워커가 검사한다(P11은 기기 값으로 확인만 한다): 단계별 샘플러 수, 단계별 에뮬레이션 `vec4` 합계(상수 버퍼·루트 상수 크기 합 ÷ 16을 `GL_MAX_VERTEX_UNIFORM_VECTORS`·`GL_MAX_FRAGMENT_UNIFORM_VECTORS`·compute uniform 한도와 대조), VS·PS storage block·image 수(0), 단계 사이 varying 벡터 수(`GL_MAX_VARYING_VECTORS`), MRT 수(`GL_MAX_DRAW_BUFFERS`. 넘는 `SV_Target`은 GLES 형식에서 컴파일 오류).
   - **공통 상한(개정 17에서 폐기).** 개정 16까지는 셰이더 타입이 프로필에서 컴파일되는 형식들의 최솟값을 공통 상한으로 검사했다. 위 "상한 검사 위치"대로 모든 상한을 job 형식으로 옮겨 이 계산을 쓰지 않는다. 렌더러 pass 셰이더처럼 GLES에서도 쓰는 타입은 GLES 형식 job이 GLES 상한을 검사하므로 결과가 같다.
   - **D3D12.** 파이프라인마다 VS·PS layout을 합쳐 root signature를 만들고 캐시한다. UE QBSS 구조를 따르되 처음에는 개수를 양자화하지 않는다(root signature 수가 문제가 되면 UE 양자화를 들인다). 순서는 단계별 테이블(SRV·Sampler·UAV, 필요하면 CBV) → root CBV → 루트 상수다. 예산식 `루트 상수 DWORD + 테이블 수 + 2 × root CBV 수 ≤ 64`를 만족하도록 root CBV를 채우고, 넘치는 CBV는 높은 슬롯부터 그 단계의 CBV 테이블로 내린다(UE의 넘친 CBV 테이블과 같은 방식, UE는 16개에서 자른다). Rocks 그래픽 파이프라인 단계는 VS·PS이고 테이블은 단계당 최대 4개라, 루트 상수 32 + 테이블 8 = 40이므로 root CBV는 최대 12개이고 나머지는 CBV 테이블로 간다. CS 파이프라인은 테이블 4개라 root CBV 14개까지다. 어느 경우도 항상 만들 수 있다.
   - **Vulkan.** set = 단계(VS 0, PS 1, CS 0. 최소 보장 `maxBoundDescriptorSets` 4 안이다), binding = 종류 순서(상수 버퍼 → 구조화·raw 버퍼 → storage image → SRV 텍스처 → 샘플러)로 0부터 연속. 워커가 DXC 출력의 decoration을 SPIRV-Reflect로 고친다(UE `SF_VULKAN_SM5`의 비-bindless 경로와 같은 방식, `-fvk-*-shift`는 쓰지 않는다).
   - **Metal.** buffer 인덱스: 0은 루트 상수, 1부터 UAV(UE처럼 buffer·texture 공간에서 같은 번호를 함께 차지한다) → 상수 버퍼 → 구조화 SRV → 버퍼 크기 side table(UE 5.8 순서. Rocks에는 간접 인자 버퍼와 가속 구조가 없다). texture 인덱스: UAV가 차지한 번호(1부터)를 비우고 SRV 텍스처는 0과 남은 가장 낮은 번호부터 쓴다. sampler는 0부터다. 정점 버퍼는 30부터 아래로 쓴다(UE와 같은 방향). 최대 정점 스트림을 8로 고정하고, VS·PS는 셰이더 buffer 인덱스 ≤ 22(정점 버퍼 8개가 30–23을 차지한다)를 컴파일 때 검사한다. 형식 정의 상한(Metal 12·4)에서 VS·PS의 최대는 루트 상수 1 + 구조화 SRV 4 + 상수 버퍼 12 + side table 1 = 18이라 들어간다. 실제 정점 버퍼 수와의 겹침은 파이프라인 생성 때 검사한다. CS는 정점 버퍼가 없으므로 0–30을 쓰고, 최대는 루트 상수 1 + 구조화·raw 버퍼 4 + storage image 4(UAV라 buffer 번호도 차지) + 상수 버퍼 12 + side table 1 = 22다. typed buffer는 SPIRV-Cross `texture_buffer_native`를 켜 `texture_buffer<T>`로 내고 texture 인덱스를 차지한다(개정 21, 측정 S13: 끄면 기본값인 `texture2d`와 좌표 변환 함수로 흉내 낸다. UE 5.8도 켠다, `Developer/Apple/MetalShaderFormat/Private/MetalCompileShaderSPIRV.cpp:1005`).
   - **D3D11.** FXC 경로는 FXC가 정한 단계별 슬롯을 그대로 쓰고 상한을 넘으면 오류다. PS UAV는 모든 형식 정의가 금지하므로(`uavStages`는 CS만) FXC가 u0을 주어 render target 0과 겹치는 일이 없다. SPIRV-Cross 경로는 종류 구분 없이 매긴 SPIR-V binding을 `spvc_compiler_hlsl_add_resource_binding`으로 종류별 register(b·t·u·s, 단계별 0부터)에 다시 배정하고(그대로 두면 s24처럼 범위 밖 register가 나온다), 정점 입력은 `spvc_compiler_hlsl_add_vertex_attribute_remap`으로 `ATTRIBUTEn` 시맨틱을 유지한다(SPIRV-Cross 기본은 `TEXCOORD{location}`).
   - **OpenGL ES(UE 방식).**
     - 상수 버퍼와 루트 상수는 모두 에뮬레이션한다(UE도 OpenGL이면 늘 에뮬레이션한다, 7.1절). 워커가 SPIRV-Cross `emit_uniform_buffer_as_plain_uniforms`로 일반 uniform이 된 버퍼의 `Name.member` 참조를 텍스트로 바꿔, 단계(`v`·`p`·`c`)와 기본 타입(`h` float·`i` int·`u` uint)별 `uniform highp vec4 <단계>c<순번>_h[N]`·`ivec4 …_i[N]`·`uvec4 …_u[N]` 배열과 `#define`으로 잇는다. 루트 상수는 `<단계>u_<타입>`(UE `$Globals`의 `pu_h` 자리)이다. 이름 규칙은 UE와 같다. 바꾼 뒤 원래 버퍼 이름이 남아 있으면(블록 전체 load 등) 컴파일 오류다.
     - 5항이 허용하는 타입은 4 byte 원소이고 3·4성분과 행렬이 16 byte 정렬이므로 멤버는 `vec4` 경계를 넘지 않고, `float4x4`는 연속한 원소 4개다. 5항이 16 byte 슬롯 하나에 기본 타입을 섞지 않게 하므로 슬롯마다 타입이 하나이고, 슬롯을 타입별 배열에 차례로 빽빽하게 넣는다. 그래서 타입별 배열 크기의 합은 버퍼 크기 ÷ 16이다(UE는 멤버마다 `vec4`를 새로 시작한다, `OpenGLShaderCompiler.cpp:1297`).
     - 쿠킹 결과에 복사 표(버퍼 순번, 바이트 범위 → 배열·원소)를 둔다. 런타임은 그리기 때 바뀐 버퍼만 복사 표대로 `glUniform4fv/4iv/4uiv`로 올린다(UE `UniformBuffersCopyInfo`).
     - mediump 배열은 없다. 5항이 half(`min16*`)를 막으므로 실수는 모두 highp다(UE는 RelaxedPrecision을 `m` 배열로 나눈다, `OpenGLShaderCompiler.cpp:1117–1124`).
     - 진짜 UBO(std140, `glBindBufferRange`)는 후속이다(UE `NoEmulatedUniformBuffer` 표시에 해당).
     - 텍스처는 9항의 짝마다 combined sampler 하나(SPIRV-Cross `build_combined_image_samplers`)이고 이름은 `<단계>s<순번>`(UE `ps<n>`)이다. Vulkan 재배치(set = 단계, binding 0부터, 종류를 이어서 번호)를 그대로 ESSL에 내면 VS와 PS 샘플러가 같은 binding이 되고 GLES의 종류별 binding 공간(텍스처 유닛·image 유닛·storage block binding)과도 맞지 않으므로, 워커가 GLES용 binding을 종류마다 다시 매긴다. 텍스처 유닛은 PS·CS 0부터, VS 16부터다(셰이더당 샘플러 16개 이하이고 ES 3.2의 `GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS` 최소 96 안이다). image 유닛과 storage block binding은 0부터다. UE는 binding을 낸 뒤 런타임이 `glUniform1i`로 VS 유닛을 `MaxTextureImageUnits`부터 정한다(7.1절). Rocks는 컴파일 때 고정해 런타임 조회를 없앤다. 샘플러 상태는 샘플러 객체(`glBindSampler`)다.
     - typed buffer SRV(`Buffer<T>`)는 ESSL `samplerBuffer`(정수는 `isamplerBuffer`·`usamplerBuffer`)이고 **텍스처 유닛 하나를 차지한다**. 그래서 단계의 SRV 텍스처 수(typed buffer SRV 포함, 3항 종류 구분)와 GLES 텍스처 유닛 수에 함께 세며, 모바일 상한의 샘플러 16 안에 든다. 샘플러 상태가 없으므로 짝 샘플러를 두지 않고 `Load`(`texelFetch`)만 쓴다(9항의 `Load`만 쓰는 텍스처와 같다). 유닛 번호는 텍스처와 같은 공간에서 매긴다(PS·CS 0부터, VS 16부터). typed buffer UAV(`RWBuffer<T>`)는 `imageBuffer`이고 image 유닛을 차지하며 CS만이다. 형식은 ES 3.2 texel buffer 표의 형식만이다([PixelFormat](PixelFormat.ko.md) 8.6절, Renderer RQ9). SPIRV-Cross 출력이 이 모양임을 측정 S13(2026-11-20)으로 확인했다(Renderer RQ10). typed buffer SRV는 결합 sampler 목록에 들지 않으므로, 워커는 결합 목록과 따로 texel buffer(SPIR-V `OpTypeImage` Dim Buffer, Sampled 1)를 찾아 같은 텍스처 유닛 공간에 매기고 SPIRV-Cross가 단 SPIR-V binding(`layout(binding = n)`)을 그 유닛으로 바꾼다. **읽고 쓰는 UAV(개정 21).** ESSL에서 `readonly`·`writeonly`가 없는 image는 `r32f`·`r32i`·`r32ui` 형식만 될 수 있다(GLSL ES 3.20 사양 4.10절 memory qualifier). SPIRV-Cross는 읽지 않는 UAV에 `writeonly`를 붙이므로(S13) 쓰기만 하는 UAV는 ES image 형식이면 되지만, 한 셰이더가 읽고도 쓰는 typed buffer UAV·storage image가 이 세 형식이 아니면 GLES 형식 job 오류다(워커 검사). SH2 전 런타임 경로(GX2, 2026-11-18)는 typed view를 텍스처 유닛 PS 1–4·VS 17–20에 묶는다([렌더 기반](RenderBase.ko.md) 4.4절 GX2).
     - storage image는 CS만이다(4항, 모든 형식 정의의 `uavStages`가 CS). 구조화·raw 버퍼 SRV도 GLES 형식에서는 CS만이다: ES 3.2의 VS·PS storage block 최소 보장이 0이기 때문이며, 모바일 상한(VS·PS storage block 0)으로 워커가 검사한다.
     - 정점 입력은 `layout(location = n) in`(`ATTRIBUTEn`), PS 출력은 SPIRV-Cross 이름(`out_var_SV_TargetN`)에 `layout(location = n)`이다.
     - 바인딩 머리(attribute 마스크, 배열 이름과 크기, 샘플러 이름과 유닛, 복사 표)는 쿠킹 결과의 별도 부분이다. 런타임은 링크 때 `glGetUniformLocation`으로 배열 위치를 한 번 찾는다.
5. **상수 버퍼 레이아웃.** D3D cbuffer, DXC 기본 cbuffer 규칙(vector-relaxed std140), push constant 규칙(vector-relaxed std430), GLES 에뮬레이션 배열이 같은 오프셋을 내도록 허용 타입을 좁히고, 워커가 실제 오프셋을 검사한다.
   - 멤버 타입: 4 byte 스칼라(float·int·uint), 그 2–4성분 벡터, `float4x4`만 허용한다. bool·double·half(`min16*`)와 다른 행렬(`float3x3` 등)은 컴파일 오류다. `float3x4`·`float4x3`는 6항의 행렬 저장 결정 뒤 별도 시험으로 허용 여부를 정한다.
   - 정렬: 스칼라 4, 2성분 8, 3·4성분·행렬 16. 이 정렬이 요구하는 자리에 명시 padding 멤버를 둔다(UE와 같다).
   - 배열: 원소 크기가 16 byte 배수일 때만 허용한다. 스칼라·2·3성분 배열은 float4 배열로 묶는다(UE `SHADER_PARAMETER_SCALAR_ARRAY`·`GET_SCALAR_ARRAY_ELEMENT`와 같은 방식).
   - 기본 타입 슬롯: 16 byte 슬롯 하나 안에 float·int·uint를 섞지 않는다(예: `float a; int b;`는 b 앞에 padding을 두어 다음 슬롯으로 보낸다). GLES 에뮬레이션이 슬롯을 타입별 배열에 나눠 담기 때문이다(4항).
   - 구조체: 16 byte 정렬로 시작하고 크기를 padding으로 16 byte 배수로 맞춘다. 상수 버퍼 안의 구조체는 1단까지이고 구조체 배열은 금지한다. 상수 버퍼 전체를 한 번에 읽는 것(`Type t = Name;`, 함수 인자로 버퍼 전체를 넘기기)도 금지한다. GLES 에뮬레이션이 멤버 참조를 텍스트로 바꾸기 때문이며(UE도 1단까지만 처리한다, `OpenGLShaderCompiler.cpp:1517–1548`), 한 원문을 모든 형식에 쓰므로 모든 형식에서 같은 규칙이다.
   - **워커 검사.** 모든 형식의 리플렉션 오프셋이 위 규칙으로 계산한 오프셋과 같아야 하며, 다르면(암묵 padding·다른 배치) 컴파일 오류다. GLES 복사 표는 이 오프셋으로 만든다. 손으로 쓴 구조체(SH0–SH2의 `ROCKS_ROOT_CONSTANTS`, 개발 경로의 `cbuffer DrawConstants`)도 같다.
   - **손으로 쓴 레이아웃 표.** P7(13절) 전에는 손으로 쓴 구조체마다 C++ 쪽에 레이아웃 표(멤버 이름·타입·오프셋·크기)를 두고 `static_assert`로 C++ 오프셋과 맞춘다. 이 표의 SHA-256이 파라미터 레이아웃 서명(job 키 8항, 10절)이고, 워커는 이 표를 받아 리플렉션 오프셋과 대조한다(7.3절 (a)). P7 뒤에는 MetaCompiler가 같은 형식의 표를 만든다.
   - 반례(이 규칙이 막는 것): `float a[2]; float b;`는 D3D에서 b가 20(마지막 원소의 남는 자리에 채움), std140에서 32이고, 명시 padding으로도 두 규칙을 함께 맞출 수 없다. `float3x3 m; float s;`는 D3D 44, std140 48이다. push constant의 스칼라 배열은 std430 stride 4, D3D root constant는 16이다. `struct S { float4 v; }; struct R { float a; S s; }`의 s는 D3D·std140 16, push constant(std430, 구조체 정렬 4) 4다. 명시 padding이 없으면 워커 검사가 거부한다.
   - C++ 타입: Core `Matrix4`·`Vector3`는 정렬 지정이 없다(`Matrix4.h:56–61`, `Vector3.h:19–24`). 그래서 ShaderCore에 셰이더 전용 정렬 타입(`alignas(8)` 2성분, `alignas(16)` 3·4성분·`float4x4`)을 두고, 셰이더 파라미터 구조체는 이 타입만 쓴다(13절에서 MetaCompiler가 강제). 그리기별 상수 구조체의 C++ 행렬 멤버(개정 19 전 Graphics `DrawConstants::transform`, 지금은 호출자 구조체의 `float transform[16]`)도 이 행렬 타입으로 바꾼다.
   - Rocks는 수정되지 않은 DXC를 쓰므로 scalar block layout을 쓰지 않는다(Android Vulkan 1.1·GLES와 호환).
6. **행렬.** 2026-11-17 사용자 결정으로 Rocks 수학 규약을 UE와 같게 바꾼다. 행렬은 행 벡터(`p' = p·M`, `A*B`는 A 먼저)와 행 우선 저장이다. 그래서 셰이더 규약도 UE와 같다.
   - 셰이더는 `mul(v, M)`으로 쓰고, 모든 형식에서 행렬을 행 우선으로 담는다.
     - DXC는 `-Zpr`이다(UE `Developer/Windows/ShaderFormatD3D/Private/D3DShaderCompilerDXC.cpp:189–190` "Unpack uniform matrices as row-major to match the CPU layout").
     - FXC는 `/Zpr`이다(UE `D3DShaderCompiler.cpp:1337–1339`의 `D3DCOMPILE_PACK_MATRIX_ROW_MAJOR`, 명령줄 `:223–227`).
     - SPIR-V에서 나오는 Vulkan·Metal·GLES 경로도 행 우선이다. UE ShaderConductor 기본값은 `bPackMatricesInRowMajor = true`다(`Developer/ShaderCompilerCommon/Public/ShaderConductorContext.h:78`).
   - CPU 행렬의 메모리 배치는 전환 전후에 바이트 단위로 같다(이동이 평면 index 12–14). 그래서 상수를 올릴 때 전치하지 않는다.
   - 비대칭 행렬 픽셀 대조 시험(SH2, 측정 S5)은 옵션을 고르는 시험이 아니다. 형식마다 위 규약이 지켜지는지 확인하는 시험이다.
   - 렌더 코드 반영(C7, 2026-11-17 구현): 샘플 렌더러 그리기 상수의 `transform`(개정 19 전에는 Graphics `DrawConstants::transform`)은 행 우선이고 샘플 렌더러 원문은 `mul(float4(position, 1), transform)`이다. SH2 전 런타임 경로에서 D3D12는 `D3DCompile`에 `D3DCOMPILE_PACK_MATRIX_ROW_MAJOR`(FXC `/Zpr`와 같은 플래그)를 준다. 원문에 `row_major`를 적지 않고 컴파일 옵션으로 담는 것은 위 DXC `-Zpr`·FXC `/Zpr` 규약과 같게 하려는 것이다. 그러면 SH2에서 쿠킹 경로로 바꿔도 원문이 그대로다. OpenGL ES(P11, 2026-11-17)는 쿠킹된 ESSL과 같은 방식으로 행 우선 byte를 4항의 루트 상수 `vec4` 배열(`vu_h`)에 전치 없이 올리고, 손으로 쓴 GLSL ES 원문이 `mat4(vu_h[0], vu_h[1], vu_h[2], vu_h[3]) * v`로 쓴다(C7의 `glProgramUniformMatrix4fv(transpose = GL_TRUE)` 방식을 대신했다). 스파이크(`Programs/ShaderToolSpike`)도 DXC `-Zpr`·FXC `D3DCOMPILE_PACK_MATRIX_ROW_MAJOR`로 컴파일한다. 그 산출에서 SPIRV-Cross ESSL·MSL은 기본(열 우선) `mat4`·`float4x4`와 `M * v`로, HLSL 5.0은 `row_major float4x4`와 `mul(v, M)`로 나온다. 둘은 같은 바이트를 같은 행렬로 읽는다(열 우선으로 읽은 행렬은 Mᵀ이고 Mᵀ·v = v·M). 시험 결과는 S5에 적었다.
7. **정점 입력과 단계 사이 변수.** DXC `-spirv`는 입출력 location을 시맨틱 번호가 아니라 선언 순서로 준다(기본 `-fvk-stage-io-order=decl`). D3D11·D3D12는 VS 출력과 PS 입력을 register 위치로 맞추고 register는 선언 순서로 정해진다. 그래서 두 단계를 따로 컴파일해도 맞도록 다음을 지킨다.
   - 정점 입력 시맨틱은 `ATTRIBUTEn`이다(현재 `TEXCOORDn`, `DeviceTypes.h:130–136`). n은 `VertexAttribute::location`이다.
   - 단계 사이 변수는 하나의 struct로 선언하고 VS 출력과 PS 입력이 같은 struct를 쓴다(공용 `.rsh`). struct 멤버 순서는 `SV_Position` 다음 `TEXCOORDn`을 n 오름차순으로 둔다. 그러면 선언 순서 = location 순서라 FXC 경로(선언 순서)와 SPIRV-Cross 경로(location 순서)의 signature가 같다. PS 입력에 다른 struct를 쓰려면 VS 출력의 앞부분(같은 순서)이어야 한다.
   - 허용 시맨틱: 단계 사이는 `TEXCOORDn`(n ≤ 15, GLES 형식은 4항 모바일 상한의 varying 수 안). 시스템 값은 `SV_Position`·`SV_Target0–7`(n < job 형식 `renderTargets`, 4항. 개정 14 기준 GLES 형식 4, 그 밖 8)·`SV_Depth`·`SV_VertexID`·`SV_InstanceID`·`SV_IsFrontFace`와 CS의 `SV_DispatchThreadID`·`SV_GroupID`·`SV_GroupThreadID`·`SV_GroupIndex`다. VS 출력 `SV_RenderTargetArrayIndex`·`SV_ViewportArrayIndex`는 아래 "VS layer·viewport 출력" 규칙으로 연다(개정 16·17). PS의 `SV_PrimitiveID`(Vulkan Geometry capability)는 같은 규칙으로 열 수 있으나 후속이다. 그 밖은 컴파일 오류다.
   - `SV_VertexID`·`SV_InstanceID`에 base vertex·base instance가 들어가는지는 backend마다 다르다(D3D는 빼고 Vulkan·Metal은 넣으며, Vulkan 1.1의 draw parameters는 선택 기능이다). Graphics API가 base vertex·base instance를 0으로만 그리므로 지금은 차이가 없다. base 값을 도입하면 루트 상수로 넘기고 시스템 값에 기대지 않는다.
   - **VS layer·viewport 출력(개정 16·17, Renderer RQ14·결정 RN10).** VS가 `SV_RenderTargetArrayIndex`(layer)를 내면 그리기 한 번으로 texture array·cube·volume의 여러 층에 그리고, `SV_ViewportArrayIndex`(viewport)를 내면 한 렌더 타깃의 여러 영역에 그린다. 쓰임: viewport 출력은 shadow atlas의 point 광원 6타일 한 번 그리기(Renderer 9.2절, 결정 RN10의 후속 능력 확장), layer 출력은 cube·array·volume 렌더 타깃 한 번 그리기(반사 capture·volume LUT 등 후속 기능. UE도 volume 텍스처 렌더링에 VS layer 출력을 쓴다, `Runtime/RHI/Public/DataDrivenShaderPlatformInfo.h:1166–1173`). Renderer 결정 RN10 (가)는 point 그림자를 2D atlas에 두므로 layer 출력이 아니라 viewport 출력이 그 확장에 맞다.
     - **형식 기능.** 형식 정의 `features` 어휘에 `vertexViewportLayerOutput` 하나를 더한다. 두 시맨틱을 함께 연다. 장치 기능이 API마다 둘을 함께 공개하기 때문이다(D3D11 OPTIONS3 `VPAndRTArrayIndexFromAnyShaderFeedingRasterizer`, Vulkan `VK_EXT_shader_viewport_index_layer`). 넣는 형식은 `D3D12_SM6`·`VULKAN_SM5`·`METAL_SM5`·`D3D11_SM5`다. `D3D11_SM5`는 FXC `vs_5_0`이 이 출력을 받고 DXBC 요구 플래그(`D3D_SHADER_REQUIRES_VIEWPORT_AND_RT_ARRAY_INDEX_FROM_ANY_SHADER_FEEDING_RASTERIZER`)를 세우는 것을 측정 S14(2026-11-20)로 확인해 개정 21에서 넣었다. D3D11 장치 값은 OPTIONS3로 이미 읽는다(`Engine/Source/Runtime/D3D11Graphics/Private/Win64/D3D11Caps.cpp`). GLES 형식에는 넣지 않는다. ES core에는 이 출력이 없고 NVIDIA 확장 `GL_NV_viewport_array2`(ES에서는 `NV_viewport_array`·`EXT_geometry_shader`·`EXT_shader_io_blocks` 필요, Khronos 확장 문서 NV_viewport_array2.txt)만 있는데, Rocks GLES backend는 이 확장을 확인하지 않아 `DeviceCaps`를 거짓으로 공개한다(RenderBase 4.4절 GX1). UE OpenGL ES도 이 확장을 쓰지 않는다(`Runtime/OpenGLDrv` 전체 grep, multiview 확장 `GL_OVR_multiview`만 본다, `Runtime/OpenGLDrv/Private/OpenGLES.cpp:246–248`).
     - **셰이더 타입 선언.** 이 출력을 쓰는 셰이더 타입(또는 퍼뮤테이션)은 요구 기능에 `vertexViewportLayerOutput`을 적는다. 형식에 그 기능이 없으면 거르기에서 빠지고 그 형식에서 쿠킹하지 않는다(2절 요구 기능 규칙, 12.2절). 요구 기능을 적지 않은 셰이더가 VS 출력에 이 시맨틱을 쓰면 컴파일 오류다. PS 입력·CS에서 이 시맨틱은 계속 오류다.
     - **장치 요구가 아니다.** `vertexViewportLayerOutput`은 **런타임 확인 기능**이다. 4항의 `limits`와 달리 런타임 형식 선택이 이 기능으로 장치를 거르지 않는다. D3D11(OPTIONS3)·Vulkan(확장)에서 선택 기능이라 형식 요구로 묶으면 같은 형식을 못 쓰는 장치가 생기기 때문이다. D3D12는 이 기능이 늘 된다. `D3D12_FEATURE_DATA_D3D12_OPTIONS`의 `VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation`은 드라이버가 geometry shader로 흉내 내는지만 알려 준다(UE도 D3D12에서 장치 값을 늘 참으로 둔다, `Runtime/D3D12RHI/Private/D3D12Device.cpp:272`, `Runtime/D3D12RHI/Private/D3D12RHI.cpp:135`). viewport 출력은 viewport 여럿도 필요하다(D3D는 16개 보장, Vulkan `multiViewport` 기능). 셰이더 집합(RenderBase 9.2절)이 장치 능력이 없는 장치에서 그 셰이더의 장치 셰이더·파이프라인 생성을 `Unsupported`로 거부한다. 호출자(렌더러)는 능력을 먼저 확인해 대체 경로(point 그림자 면마다 그리기, Renderer 결정 RN10)를 고른다. 몰래 바꾸지 않는다.
     - **형식 id 규칙.** 대체 경로가 있는 런타임 확인 기능 때문에 형식 id를 나누지 않는다. 형식 id를 따로 두는 것은 기능을 장치에 **보장**받아야 하고 대체 경로가 없을 때뿐이다. 그때 그 기능은 새 형식 id의 `deviceRequirements`(버전·확장 이름)에 넣는다(2절, 4항 "상한은 장치 요구다"와 같은 방식). 기능 어휘마다 장치 보장 기능(`compute`·`waveOperations`: 형식의 `deviceRequirements`가 보장한다)인지 런타임 확인 기능인지가 정해져 있고 형식 정의 읽기 코드가 이 구분을 안다(15절 결정 12).
     - **UE 대응.** UE의 조건은 두 범주다(`Runtime/RenderCore/Private/RenderUtils.cpp:1672–1678`). (1) 플랫폼 정적 표시(`bSupportsVertexShaderLayer`, `Runtime/RHI/Public/DataDrivenShaderPlatformInfo.h:111, 1161–1164`)가 참이면 그것만으로 쓴다. 장치 보장 기능에 해당한다. (2) 정적 표시가 없으면 "실행 때 감지" 설정(`r.Shadow.DetectVertexShaderLayerAtRuntime`, Windows 기본 1, `Engine/Config/Windows/BaseWindowsEngine.ini:35`)이 켜졌고 장치 값 `GRHISupportsArrayIndexFromAnyShader`가 참일 때 쓴다. 이 경우 vslayer 퍼뮤테이션은 설정만으로 컴파일해 두고(`Runtime/Renderer/Private/ShadowDepthRendering.cpp:356–361, 408`) 실행 때 장치 값을 본다(`Runtime/Renderer/Private/ShadowSetup.cpp:3868`). 런타임 확인 기능에 해당한다. Rocks는 이 기능을 늘 (2)로 다룬다.
     - **변환(측정 S14, 2026-11-20).** DXIL은 SM 6.0부터 받고 요구 플래그 0x2000을 세운다. SPIR-V(vulkan1.1)에는 capability `ShaderViewportIndexLayerEXT`와 확장 `SPV_EXT_shader_viewport_index_layer`가 붙는다. 그래서 워커의 `VULKAN_SM5` 허용 SPIR-V 확장 목록에 이 확장을 넣되, 셰이더 타입이 `vertexViewportLayerOutput`을 요구할 때만 허용한다(그 밖의 셰이더에 나오면 오류). 장치 쪽은 같은 이름의 장치 확장 `VK_EXT_shader_viewport_index_layer`(Vulkan 1.2 core 기능 `shaderOutputLayer`·`shaderOutputViewportIndex`)다. MSL은 `[[render_target_array_index]]`·`[[viewport_array_index]]`, D3D11 대체 경로의 HLSL 5.0은 같은 시맨틱이다. ESSL은 `#extension GL_NV_viewport_array2 : require`를 붙여 내므로 GLES 형식에서 빼는 규칙과 맞다.
     - **장치 능력 이름.** Graphics `DeviceCaps.features.vertexShaderLayerOutput`은 이름과 주석이 layer만 말하고, viewport 수 한도가 없으며, D3D12 값을 "GS 흉내 없음" 표시로 정한다. 두 출력을 함께 뜻하도록 이름·주석을 고치고 viewport 수(Vulkan `maxViewports`·`multiViewport`)를 더하며 D3D12는 늘 참으로 두는 것이 Graphics 쪽 일이다(RenderBase 4.4절 GX1).
   - 워커가 SPIR-V 입력·출력 location을 시맨틱 번호 n으로 다시 쓴다. UE는 VS 입력(`ATTRIBUTEn`)만 시맨틱 번호로 다시 배정하고(7.1절) 단계 사이 변수는 다시 쓰지 않는다. 단계 사이 변수 재작성은 Rocks가 더하는 규칙이다. Metal의 `[[attribute(n)]]`·`[[user(locnN)]]`과 GLES의 `layout(location = n)`(SPIRV-Cross는 ESSL 310 이상에서 단계 사이 변수에도 location을 쓴다)도 이 location에서 나온다. GLES는 단계 사이 struct를 SPIRV-Cross `force_flattened_io_blocks`로 펼친다(UE와 같다).
   - 다시 쓴 SPIR-V는 `spirv-val`로 검증한다. 그래서 SPIRV-Tools는 필수다.
   - VS·PS 쌍의 연결은 DXIL·DXBC signature와 SPIR-V location을 대조해 검사한다(쿠킹 때 알려진 쌍, 파이프라인 생성 때 Development 검사).
   - 셰이더가 쓰지 않는 attribute는 리플렉션 입력 마스크로 거른다(UE `InOutMask`, `Runtime/VulkanRHI/Public/VulkanShaderResources.h:100`).
8. **좌표와 앞면.** 셰이더는 D3D 규약으로 쓰고 backend 분기를 두지 않는다(계획 11.5 "한 번만 적용").
   - Vulkan은 음수 viewport 높이(Vulkan 1.1 core)로 뒤집고, HLSL을 뺀 모든 대상(SPIR-V·MSL·ESSL)에 `-fvk-use-dx-position-w`를 넘긴다(UE와 같다).
   - `GLES31_ANDROID`는 SPIRV-Cross 업스트림 옵션 `fixup_clipspace`·`flip_vert_y`로 VS 끝에서 z를 [-1,1]로 바꾸고 y를 뒤집으며, backend는 컬 방향을 뒤집고 표시 때 Y를 뒤집는다(UE와 같다).
   - `GLES31_PC`는 두 옵션을 끄고 창·RenderTarget 모두 `glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)`으로 그린다(UE도 결과가 같다: UE가 SPIRV-Cross Options에 더한 `force_glsl_clipspace` 필드를 보고 ShaderConductor가 두 옵션을 끈다. Rocks는 업스트림 옵션을 직접 지정한다). 그래서 창에는 offscreen color target에 그린 뒤 Y를 뒤집는 blit으로 표시한다(UE 데스크톱도 표시 blit에서 Y를 뒤집는다, `Runtime/OpenGLDrv/Private/Windows/WindowsOpenGLPlatform.cpp:511–513`). C7까지의 규칙(창 `GL_LOWER_LEFT`, RenderTarget `GL_UPPER_LEFT`)은 P11(2026-11-17)에서 이것으로 바뀌었다. Windows 미리보기의 present blit은 sRGB 쓰기 스위치를 끄고 byte를 옮긴다(UE와 같다). Android는 ES 사양대로 sRGB 원본을 디코딩하므로 스위치를 켠 채 sRGB 창 표면으로 옮긴다([OpenGL ES backend](OpenGLESGraphics.ko.md) 2.2·5.2절).
   - **앞면 규약.** UE와 같게, 표시된 화면에서 반시계로 감긴 삼각형이 앞면이다.
     - 기본 컬링은 뒤면이다. UE D3D12는 `FrontCounterClockwise = true`이고 기본 컬 `CM_CW`가 `D3D12_CULL_MODE_BACK`이다(`Runtime/D3D12RHI/Private/D3D12State.cpp:33–35, 353`, `Runtime/Renderer/Private/MeshPassProcessor.cpp:1860–1868`).
     - 렌더 메시의 앞면 법선은 `(C−A)×(B−A)`다(UE `MeshDescription.cpp:1311–1313`). 기하 `Plane`·`Triangle`의 법선 식과 이 렌더 앞면의 관계는 Core Math 쪽 결정(좌표 전환 C3)을 따른다.
   - **음수 행렬식.** 물체 local-to-world 행렬식이 음수이면 컬 방향을 뒤집는다(UE `StaticMeshSceneProxy.cpp:1318–1327`, `DynamicMeshBuilder.cpp:920`). view 행렬식이 음수여도 뒤집는다(`SceneView.cpp:1000`, `MeshPassProcessor.cpp:1697`). 둘 다 해당하면 원래 방향으로 돌아온다. 뒤집기는 파이프라인 래스터 상태 선택에서 하고, 셰이더는 관여하지 않는다.
   - **backend 매핑.** 앞면 규약은 Graphics API 하나로 정하고, backend별 `frontFace`·컬 매핑은 backend 안에 숨긴다.
     - UE Vulkan은 `VK_FRONT_FACE_CLOCKWISE`와 `CM_CW → VK_CULL_MODE_FRONT_BIT`에 음수 viewport를 함께 쓴다(`Runtime/VulkanRHI/Private/VulkanPipeline.cpp:936`, `VulkanState.cpp:186–188`). 컬링 결과는 D3D12와 같지만 앞면이 화면 시계가 되어 `SV_IsFrontFace`가 반대이고, UE는 머티리얼 셰이더에서 Vulkan일 때 부호를 뒤집는다(`Engine/Shaders/Private/MaterialTemplate.ush:4641–4648`). 셰이더에 backend 분기를 두지 않으므로 Rocks Vulkan은 `VK_FRONT_FACE_COUNTER_CLOCKWISE`·`Back → VK_CULL_MODE_BACK_BIT`로 D3D12와 같은 매핑을 쓴다([VulkanGraphics](VulkanGraphics.ko.md) 11절).
     - GLES Android는 `flip_vert_y` 때문에 컬 방향을 뒤집는다(위 항).
     - 매핑이 맞는지는 컬링 픽셀 대조로 확정한다(SH2 Vulkan·GLES 미리보기, SH5 Metal·D3D11, SH6 Android).
   - 구현(C7, 2026-11-17): Graphics `PipelineDesc::cullMode`(None·Back·Front)가 컬을 정한다. 기본은 None이다. UE `TStaticRasterizerState`의 기본 `CM_None`과 같고(`RenderCore/Public/RHIStaticStates.h:86`), 감김을 정하지 않은 기존 화면 채우기·시험 그리기가 그대로 보인다. D3D12는 `FrontCounterClockwise = TRUE`와 `Back → D3D12_CULL_MODE_BACK`·`Front → D3D12_CULL_MODE_FRONT`로 옮긴다. OpenGL은 `glCullFace`(GL_BACK·GL_FRONT)를 쓰고, `glFrontFace`는 장치가 클립 공간 규약에 맞춰 정한다(S5. P11 뒤: `GLES31_PC` `GL_CCW`, `GLES31_ANDROID` `GL_CW`). 샘플 렌더러는 뒤면 컬 파이프라인과 앞면 컬 파이프라인을 두고, 항목 world 행렬식과 view 행렬식 가운데 하나만 음수일 때 앞면 컬 파이프라인을 쓴다. 내장 메시는 UE 좌표(로컬 Y 오른쪽·Z 위)로 다시 적었고 감김을 화면 반시계(법선 `(C−A)×(B−A)` = −X)로 바꿨다.
9. **텍스처와 샘플러.** 텍스처마다 샘플러를 정하는 현재 규칙(`DeviceTypes.h:126–127`, `Sampler.h:42`)을 유지한다. HLSL은 둘을 따로 선언하므로 짝 규칙을 둔다: 텍스처 `X`는 같은 단계의 `SamplerState XSampler`와 짝이다. 짝이 없는 텍스처·샘플러와, 한 샘플러를 여러 텍스처가 쓰는 경우는 컴파일 오류다. 예외는 아래 공유 샘플러뿐이다. 이 규칙으로 GLES combined sampler가 텍스처마다 하나로 정해진다(UE는 텍스처마다 샘플러 하나로 합치려고 SPIRV-Cross를 고쳤다, `ShaderConductor.cpp:1745–1758`). 정적 샘플러(UE space1000, root signature에 박는 샘플러)는 후속이다.
   - **공유 샘플러(개정 13, Renderer RQ23·머티리얼 MSR13).** 짝 규칙만으로는 자기 샘플러를 쓰는 텍스처 수가 형식의 샘플러 상한 16에 묶여, 12.1절에서 SRV 텍스처 상한을 넓힌 형식도 16장을 넘지 못한다. UE는 머티리얼 텍스처의 샘플러 출처(`ESamplerSourceMode`: `SSM_FromTextureAsset`·`SSM_Wrap_WorldGroupSettings`·`SSM_Clamp_WorldGroupSettings`, `Runtime/Engine/Classes/Engine/EngineTypes.h:283–292`)로 view가 주는 공유 샘플러를 고르게 해 이를 푼다(생성 `Runtime/Engine/Private/Materials/HLSLMaterialTranslator.cpp:7364–7388`). Rocks도 같은 구조를 둔다.
     - **선언.** 공유 샘플러는 엔진 공용 헤더 `/Engine/Shared/SharedSamplers.rsh`가 선언하는 예약 이름 둘이다: `SharedWrapSampler`(UE `Wrap_WorldGroupSettings`)와 `SharedClampSampler`(UE `Clamp_WorldGroupSettings`). 원문은 다른 샘플러처럼 register 없이 쓴다(1항). 이 이름은 짝 규칙의 `XSampler`로 쓸 수 없고, 예약 이름이 아닌 샘플러를 여러 텍스처가 쓰면 여전히 오류다.
     - **텍스처마다 출처 하나.** 텍스처 `X`는 자기 샘플러 `XSampler` 또는 공유 샘플러 하나 가운데 **정확히 하나**로만 샘플링한다. `XSampler`가 선언되어 있으면 자기 샘플러가 출처이고, 없으면 그 텍스처를 샘플링하는 공유 샘플러가 출처다. 한 텍스처를 두 출처로 샘플링하면(자기 샘플러와 공유, 공유 Wrap과 Clamp) 컴파일 오류다. 그래야 GLES combined sampler가 텍스처마다 하나로 남는다. `Load`·`GetDimensions`만 쓰는 텍스처는 출처가 없어도 된다(지금 규칙과 같다).
     - **검사.** 이름 규칙(예약 이름, 짝, 출처 하나)은 모든 형식에서 리플렉션 이름으로 검사한다. 실제 (텍스처, 샘플러) 사용 쌍은 SPIR-V 경로 형식이 SPIRV-Cross `build_combined_image_samplers`가 낸 결합 목록으로 대조한다. DXIL·DXBC 리플렉션에는 사용 쌍이 없으므로 D3D 형식만 쿠킹하는 프로필에서는 이름 규칙까지만 확인된다(엔진 컴파일 프로필 `Engine-Windows`는 SPIR-V 형식을 함께 쿠킹하므로 엔진·머티리얼 셰이더는 쌍까지 확인된다).
     - **리플렉션.** `ShaderBindingLayout`(3항)의 텍스처 항목은 샘플러 출처(`Own`·`SharedWrap`·`SharedClamp`, 또는 없음)를 갖고, 샘플러 항목은 공유 여부를 갖는다. 머티리얼 생성 원문은 텍스처 slot의 출처대로 `MaterialTexture<k>Sampler`를 내거나 공유 샘플러를 쓴다([머티리얼 시스템](MaterialSystem.ko.md) 13절 9항 대응).
     - **상한.** 단계의 샘플러 수 = 자기 샘플러 수 + 쓰인 공유 샘플러 수(0–2) ≤ 형식 `samplers`. SRV 텍스처 수 ≤ 형식 `sampledTextures`. GLES는 텍스처 유닛이 텍스처마다 하나이므로 텍스처 수만 센다.
     - **backend 위치.** 공유 샘플러는 그 단계의 일반 샘플러 slot이다. 순번은 자기 샘플러(텍스처 순번 순) 다음에 `SharedWrapSampler`, `SharedClampSampler` 순이다(쓰인 것만). D3D12 샘플러 테이블, D3D11 `s` register, Vulkan 샘플러 binding(종류 순서 끝), Metal sampler 인덱스가 모두 이 순번을 따른다. GLES는 공유 샘플러 slot이 없다. 공유 샘플러로 샘플링하는 텍스처의 유닛에 그 공유 샘플러 객체를 `glBindSampler`로 묶는다(Renderer RQ23 "combined sampler 규칙 유지"). 바인딩 머리의 샘플러 표에 유닛마다 출처를 둔다.
     - **상태의 주인.** 공유 샘플러의 상태(필터·이방성·주소 Wrap/Clamp·mip bias)는 셰이더가 아니라 렌더러가 view마다 정한다. UE처럼 world 텍스처 그룹 설정과 view의 머티리얼 mip bias(TAA·업스케일)를 담는다. Graphics API는 파이프라인 layout의 공유 샘플러 slot을 호출자가 주는 샘플러 객체로 채운다(SH2 Graphics API). 텍스처마다 샘플러를 정하는 지금의 Graphics 규칙(`Sampler.h:42`)은 자기 샘플러 출처에만 적용된다.
     - **시기.** 원래 후속이던 것을 앞당겨 SH2(쿠킹 셰이더·바인딩 layout)와 함께 구현한다. Renderer RQ23의 시점(머티리얼 텍스처가 16장을 넘는 콘텐츠 전, 측정 MM5)보다 이르며, 셰이더 규칙과 바인딩 layout을 두 번 바꾸지 않기 위해서다.
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
| (b) | 한 셰이더 안 binding 충돌 0건(GLES는 VS·PS 합쳐서, 텍스처·image·storage block 공간별로), 모든 상한을 job 형식 상한으로 검사(넓은 형식은 통과하고 좁은 형식 job만 오류, GLES를 거르는 퍼뮤테이션은 GLES 상한과 무관, 4항), 한 형식 정의의 상한만 바꿨을 때 그 형식 job만 키 무효화 | SH2(형식), P6b(프로필) |
| (c) | 비대칭 행렬·attribute 순서를 바꾼 정점 입력·단계 사이 변수·컬링의 픽셀 대조 | D3D12·Vulkan·GLES 미리보기는 SH2. D3D11·Metal은 SH5, Android GLES는 SH6까지 **계약(미검증)** |
| (d) | 루트 상수 128 byte 경계, D3D12 64 DWORD 예산 초과 시 CBV 테이블로 내림, GLES 루트 상수 배열 갱신 | SH2 |
| (e) | Metal 셰이더 buffer 인덱스 상한 음성 대조(컴파일), 정점 스트림 겹침 음성 대조(파이프라인 생성) | SH2(컴파일), SH5(파이프라인) |
| (f) | 5항 반례(`float a[2]; float b;`, `float3x3`, bool, 스칼라 배열, padding 없는 구조체), 상수 버퍼 안 구조체 배열·2단 중첩 구조체·버퍼 전체 load, 이름 없는 전역 uniform, `ROCKS_IMAGE_FORMAT` 없는 storage image의 컴파일 거부 | SH2 |
| (g) | location 재작성 뒤 `spirv-val` 통과, 선언 순서를 바꾼 VS·PS 쌍의 연결 거부·일치(SPIR-V·DXIL·ESSL) | SH2. D3D11 두 경로 혼합은 SH5 |
| (h) | 텍스처·샘플러 짝 규칙의 음성 대조, GLES combined sampler 이름·유닛. 공유 샘플러: 17장 이상 텍스처를 공유 샘플러로 쓰는 PS가 넓은 형식에서 컴파일되고 16장 형식에서는 그 형식 job만 오류, 한 텍스처를 두 출처로 쓰기·예약 이름을 짝 샘플러로 쓰기의 음성 대조, 공유 Wrap·Clamp의 픽셀 대조(GLES 유닛별 `glBindSampler` 포함) | SH2 |
| (i) | GLES 모바일 상한(샘플러 16, 에뮬레이션 `vec4` 합계, VS·PS storage block, varying 수, MRT 수) 초과와 한 슬롯에 타입을 섞은 상수 버퍼의 컴파일 오류, 바뀐 버퍼만 올리는지(호출 수 계측) | SH2(미리보기), SH6(기기) |
| (j) | MRT: `SV_Target4`–`7`을 쓰는 PS가 render target 8 형식에서 컴파일되고 GLES 형식 job만 오류(4항 job 형식 검사). VS layer·viewport 출력: 요구 기능을 적지 않은 타입의 두 시맨틱 컴파일 오류, `vertexViewportLayerOutput`이 없는 형식에서 그 타입이 거르기로 빠짐, `DeviceCaps` 거짓 장치(GLES 미리보기)에서 장치 셰이더·파이프라인 생성 `Unsupported`, 참 장치에서 atlas 6타일 한 번 그리기(viewport)와 cube 6면 한 번 그리기(layer) 픽셀 대조(7항) | SH2(D3D12·Vulkan·GLES 미리보기), SH5(D3D11·Metal) |

P2 완료 기준: 이 절의 규칙과 `ShaderBindingLayout` 형식이 확정되고, 위 시험이 SH2·SH3·SH5·SH6 완료 기준에 들어 있다. 6항 행렬 규약의 형식별 확인과 8항 backend별 앞면 매핑은 측정 S5로 확인하는 SH2 산출물이며 P2 완료를 막지 않는다.

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
- [x] 정책 검사(`PolicyChecks.ShaderToolReferences`, `ReleaseExclusion`): `shader-tool-table`(도구 표 의존·include는 ShaderCompiler·워커·ShaderFormat\*·ShaderToolLoader·시험 runner만), `shader-tool-binary-load`(ShaderToolLoader·binary 라이브러리 이름 참조·binary 공개 헤더 include·`DynamicLibrary`의 절대 경로 `Open`은 워커·ShaderFormat\*·ShaderToolLoader·시험 runner만, `Open`은 PlatformBase·TestSupport와 모듈 로더의 manifest 적재 `ModuleManifest.cpp`도 허용 — [모듈 로더](ModuleLoader.ko.md) 3.13절), Release 배제(`release-developer-module`·`release-shader-compiler`·`release-windows-opengl`). "Release 대상"은 `TargetRules.Type == Game`(UE TargetType 대응, 기본 Game, 도구·시험·벤치마크·Commandlet은 Program)인 타깃을 — [빌드 타깃](BuildTargets.ko.md) BD20 구현 뒤에는 Game·Client·Server 타깃을, Server는 Windows x64·macOS arm64·Linux x64로 — Windows x64·macOS arm64·Android arm64 Release 규칙으로 평가한 도달 집합이다. Developer 모듈은 규칙 폴더가 `<계층>/Source/Developer/`인 모듈이다.
- [x] 알려진 Release 예외 2건(SH2까지): Sandbox가 닿는 D3D12Graphics의 `d3dcompiler` 링크, Sandbox의 Windows OpenGLGraphics 링크. 실패가 아니라 `policy` 출력의 `note: known Release exception until SH2` 줄로 보고하고, 맞는 위반이 없어지면 `release-exception-stale` 실패로 예외를 지우게 한다.
- [x] 음성 대조(ToolTests `Policy.ShaderToolReferenceControls`·`Policy.ReleaseExclusionControls`): Runtime 모듈의 도구 표 의존·include, Runtime 모듈의 binary 라이브러리 참조·`dxcapi.h` include·절대 경로 `Open`, 메인 프로세스(ShaderCompiler)의 로더 참조, 이름만 비슷한 `ShaderFormat` 폴더, Release Game 타깃의 Developer 모듈 도달·d3dcompiler·dxcompiler 링크, Windows Release 대상의 OpenGLGraphics(Android는 허용), 예외의 알림 전환과 낡은 예외.
- [x] 시험(EngineTests): 생성 표 내용(manifest SHA 대조), 인자 오류, 빈 저장소의 bootstrap 안내, bootstrap한 저장소만으로 네 도구 열기와 함수 호출(DXC 버전, `spirv-val` 양성·음성, SPIRV-Cross ESSL 320 출력), SHA-256·크기 불일치와 없는 파일을 열지 않음, 현재 디렉터리·PATH의 같은 이름 라이브러리를 쓰지 않음(자식 probe, P10 음성 대조와 같은 시험 라이브러리). 저장소가 없거나 도구가 bootstrap되지 않았으면 도구를 여는 시험은 `[SKIP]`이며 실패가 아니다(P10과 같은 관례). 기본 빌드·Foundation CI는 저장소에 기대지 않는다(EngineTests는 CI에 없다).
- [x] SH0 도구 스파이크(`Programs/ShaderToolSpike`)를 워커 자리 측정 프로그램으로 정책 허용 위치에 더했다(`shader-tool-table`·`shader-tool-binary-load`, 플랫폼 경계는 `Private/Win64`만 허용). 워커가 생기면 스파이크와 함께 지운다. SPIRV-Cross 함수 표에 `spvc_compiler_build_combined_image_samplers`를 더했다(ESSL에는 분리 texture·sampler가 없다).
- [ ] 런타임 의존 파일 복사: 결정 3(SM 6.6)으로 필요해졌다. 할 일은 Agility SDK `D3D12Core.dll`을 `kind: binary` `redist` 항목으로 등록하고, Asan 런타임 복사를 일반 `RuntimeDependencies`로 넓혀 Runtime 실행 파일 옆에 두며, `D3D12SDKVersion`·`D3D12SDKPath` export를 넣는 것이다. 구현은 P6b 또는 SH2에서 한다(그 전에는 `redist` 파일이 없다).
- [ ] macOS: Mac 저장소 루트 구현은 있으나 macOS 산출물(P3)이 없어 열기 시험은 SKIP이고 CI 미검증이다. `QueryDxcVersion`은 Windows만 구현했다.

## 10. P5 에셋 너겟과 셰이더 job 키

AssetSystem A3(9.1절, 구현 `Engine/Source/Developer/AssetNugget`)을 그대로 쓰고, 셰이더 job도 AssetSystem 9절의 **공통 키 규칙을 그대로 따른다**(2026-11-13 사용자 결정 10). 개정 23(2026-11-23)에서 A3의 실제 API에 맞췄다. 의존: A2 `ProjectLayout`(구현), A3(5.2절 N1–N3, 반영됨), B7(구현).

**너겟 위치와 설정.** 메인 프로세스가 `ReadNuggetCacheSettings(allocator, files, root, &failure)`로 `<root>/Config/AssetNuggets.json`(없으면 기본값)을 읽고 `NuggetCache::Open(settings, root)`로 연다. root는 프로젝트가 있으면 프로젝트 루트(`ProjectLayout::Root`)다. 프로젝트 없이 엔진 셰이더만 컴파일할 때(Sandbox·self-test·엔진 내장 라이브러리)는 root를 엔진 폴더(HostConfig `FindEngineDirectory`, 저장소의 `Engine`)로 준다. 그러면 같은 규칙으로 `Engine/Config/AssetNuggets.json`(선택)을 읽고 기본 위치가 `Engine/Intermediate/AssetNuggets/`가 된다(저장소 `.gitignore`에 `/Engine/Intermediate/`를 더한다). 설정 파일로 `localPath`를 절대 경로로 주면 프로젝트 사이·엔진과 공유할 수 있다(내용 주소라 안전하다). UE 엔진 DDC(`Engine/DerivedDataCache`)의 자리다. 키 공통 머리의 프로젝트 세대는 `NuggetCache::ProjectGeneration()`이다(엔진만 쓰면 기본 null Guid).

**job 키.** 메인 프로세스가 계산한다(도구를 열지 않으므로 형식별 값은 형식 정의와 도구 표에서, Apple 도구 지문은 워커 도구 조회(11절 3항, 11.1절 결정 SC11)에서 얻는다). `NuggetKeyWriter`(`AssetNugget/NuggetKey.h`)에 아래 순서로 기록하고 `Finish()`의 SHA-256이 키다. 셰이더 job에는 AssetId·Build 의존 순환이 없으므로 SCC 식(`NuggetSccKey`)을 거치지 않고 **이 키를 Record 키로 그대로 쓴다**(AssetSystem 9.1절 결정 2 끝, 5.2절 N1). 기록 순서와 번호 표는 고정이며 바꾸면 셰이더 job 처리기 버전 Guid를 바꾼다. 키에 넣는 byte는 **워커에 보내는 바로 그 byte**다: 형식 정의·프로필은 세션을 시작할 때 한 번 읽어 둔 snapshot의 byte로 SHA-256을 계산하고 같은 byte를 워커에 보낸다(디스크를 다시 읽지 않는다), 레이아웃 표는 메시지에 싣는 정규 인코딩 byte의 SHA-256이다.

| 순서 | `NuggetKeyWriter` 기록 | AssetSystem 9절 항목 |
|---|---|---|
| 1 | `WriteNuggetKeyHeader(writer, ShaderJobProcessor, projectGeneration)`: `"RocksAssetNugget"`·키 형식 버전·엔진 세대 Guid·프로젝트 세대 Guid·처리기 Guid·처리기 버전 Guid·`deterministic::Version`·`build::ToolchainFingerprint()`. `ShaderJobProcessor`는 셰이더 job 처리기 하나다(형식마다 나누지 않는다. 형식 쪽 출력 의미 변화는 형식 정의 `processor.version`이 3행 SHA로 키에 들어간다). 메인 프로세스의 지문이 워커의 지문과 같다는 것은 워커가 입력 머리로 확인한다(11.1절 결정 SC5·SC6) | 형식 버전, 세대 Guid 둘, 처리기 Guid·버전 Guid, 결정적 수학 버전, 툴체인 지문 |
| 2 | `WriteList(n)` 뒤 도구마다 `WriteNuggetEncoder(name, version, settings)`. 순서는 형식 정의 `tools` 목록 순서(형식 정의 byte가 키에 들어가므로 순서도 고정이다). 저장소 도구: name·version은 도구 표 값, settings는 이 호스트 도구 표 파일 가운데 출력에 관여하는 role(`Runtime`·`Redist`·`Tool`)의 SHA-256(32 byte)을 표 순서로 이은 byte(`Symbols`·`Notice`는 넣지 않는다 — PDB·라이선스만 바뀌어도 모두 미적중이 되지 않게). `status: system` 도구(Apple `MetalToolchain`): version은 워커가 잰 `metal -v`의 버전 줄과 `Target:` 줄(경로가 든 `InstalledDir:` 등은 뺀다), settings는 도구 조회가 `xcrun --find`로 고정한 `metal`·`metallib` 실행 파일의 SHA-256을 이은 64 byte(11.1절 결정 SC11). SPIRV-Reflect(워커에 컴파일되는 C 원본)는 리플렉션을 쓰는 SH2부터 목록 끝에 name `SpirvReflect`·manifest 버전·manifest SHA-256으로 넣는다(SH1 출력은 리플렉션을 쓰지 않는다) | 외부 도구의 이름·버전·설정 |
| 3 | `WriteString(formatId)`, `WriteDigest(형식 정의 byte의 SHA-256)` | 외부 도구 설정(형식 정의가 셰이더 모델·target env·경로·배포 대상·상한·처리기 버전을 가진다) |
| 4 | `WriteU32(stage)` — 단계는 이미 있는 고정 값 `ShaderStageBits`의 한 비트(vertex 1·pixel 2·compute 4, 형식 정의 `stages` 어휘 `vertex`·`pixel`·`compute`와 1:1), `WriteString(entry)` | 에셋 TypeId 자리 |
| 5 | `WriteDigest(전처리 결과 SHA-256)` — 가상 경로만 담긴 `#line`을 가진 원문이고 정의(`#define`)·`Platform.rsh`는 이미 펼쳐져 있다. 워커는 이 원문 밖의 정의를 컴파일러에 넘기지 않는다 | 입력 bulk SHA-256 |
| 6 | `WriteReflected(writer, options)` — 컴파일 옵션 `ShaderCompileOptions`(`ROCKS_STRUCT`, 11.1절 결정 SC9)를 AssetSystem 9.1절 결정 2의 리플렉션 기록으로 쓴다(기본값 포함 전체, 필드 이름이 키와 trace에 들어간다). 그 뒤 파라미터 레이아웃 서명: P7 전에는 손으로 쓴 레이아웃 표(7.2절 5항)의 정규 인코딩 byte SHA-256을 `WriteDigest`, 레이아웃이 없는 job(SH1 시험·도구 원문)은 `WriteNone` | 결과에 영향을 주는 설정 전체 |
| 7 | `WriteString(profileId)`, `WriteDigest(프로필 byte의 SHA-256)` — SH1은 호스트별 엔진 컴파일 프로필(`Engine-Windows`·`Engine-MacOS`, 12.1절)만 받는다. 프로젝트 TargetProfile(A4)은 엔진 파일과 프로젝트 재정의를 합친 결과를 `WriteReflected`로 쓰도록 P6b에서 바꾼다(파일 SHA로는 재정의 변경을 놓친다) | 대상 프로파일 id와 출력 설정 전체 |
| — | 없다. include 폐포는 전처리 결과에 들어 있다 | 의존 목록 |

**Record와 값(5.2절 N2).** bucket `shader`(`NuggetBucketShader`), namespace `rocks.nuggets`(고정), key = job 키. `inputs`는 비어 있다(다른 Record를 소비하지 않는다). `meta`는 비워 둔다. 값은 ValueId 오름차순의 부분이다:

| ValueId | 이름(`ShaderNuggetValue`) | 내용 | 단계 |
|---|---|---|---|
| 0 | `Code` | 형식 산출물: DXIL·DXBC·SPIR-V byte, ESSL·MSL UTF-8 텍스트, metallib byte | SH1 |
| 1 | `BindingLayout` | 정규화된 바인딩 layout(7.2절) | SH2 |
| 2 | `GlesCopyTable` | GLES 바인딩 머리·상수 버퍼 복사 표(7.2절 4항) | SH2 |
| 3 | `DebugSymbols` | 디버그 심볼(PDB·SPIR-V debug 등) | SH4 |

- 진단은 Record에 넣지 않는다. 경고는 결정 7로 오류이므로 성공 job에는 진단이 없고, 진단 문자열은 작업 디렉터리·도구 버전 문구가 섞여 같은 키에서 digest를 흔들 수 있다(`Conflict`). 진단은 실패 job의 결과로만 돌려준다.
- 실패한 job(컴파일 오류·도구 오류)은 게시하지 않는다. 같은 입력을 다시 컴파일하면 같은 진단이 나오고, 실패를 캐시하면 도구·환경 문제의 회복이 늦어진다.
- 조회: 값 선택을 비워 Record 전체를 청한다(값을 골라 청하면 없는 값 때문에 `Miss`가 되고 다시 만든 Put이 `Exists`가 되는 고리가 생긴다 — A3 Get은 청한 값이 하나라도 없으면 `PartialRecord` 없이 `Miss`다). 정책은 `Default`(`PartialRecord` 없음). 적중했는데 값 0이 없으면 손상으로 보고 진단·통계를 남긴 뒤 다시 컴파일한다.
- 컴파일 묶음은 `NuggetGetRequest`로 모든 job을 한 번에 조회하고, 미적중만 워커에 보내고, 결과를 `NuggetPutRequest`로 게시한다(AssetSystem 9.1절 결정 12). 요청을 시작하고 기다리는 것은 관리자의 조정 스레드이며 그 scheduler의 worker가 아니다(같은 scheduler worker에서 진행 중 요청을 기다리거나 소멸하면 교착·Fatal — `NuggetRequest.h` 계약). 필요 시 컴파일(11.1절 결정 SC12) 한 건은 동기 `NuggetCache::Get`·`Put`이다.
- Put은 값 버퍼를 옮겨 간다(`NuggetPutRequest::Start`·`NuggetCache::Put`). 관리자는 호출자에게 돌려줄 값을 게시 전에 결과 쪽에 복사해 둔다.
- 결과 처리: `Hit` → 값 0을 쓴다. `Miss`·`Corrupt`·`DigestChanged`·`Dropped` → 다시 컴파일해 게시한다(DigestChanged 뒤 다시 만든 Record는 캐시가 Memory에 고정 보관한다 — 결정 11). `Cancelled`(캐시 Shutdown) → 그 job은 취소로 끝난다. Put 결과 `Conflict`는 결정성 위반이라 진단과 통계(결정성 gate)를 남기고 이번 결과를 쓴다. 캐시 실패(`failed`)는 설정 `required`면 job 오류, 아니면 진단만이다(R-1).

공통 규칙을 따른 결과:
- 툴체인 지문에 대상 triple(아키텍처·OS·배포 대상)이 들어가므로 Windows x64와 macOS arm64는 같은 셰이더도 키가 다르다. 셰이더 너겟은 호스트 사이에 공유하지 않는다(계획 §2.5 "host가 다르면 우선 분리"와 같은 방향). 키가 달라도 결정성 규칙(아래)은 그대로 적용한다.
- 한 호스트만 만들 수 있는 형식(Windows의 DXBC, 15절 결정 5의 metallib)은 다른 호스트에서 해당 형식 job이 `UnsupportedCapability`로 실패한다(계획 §27.9). 원격 워커(계획 §2.5 "macOS는 같은 action을 Windows worker에 전달", §27.7)가 생기면 그쪽으로 보낸다. 이때 키는 요청한 호스트의 것이다.
- 프로파일의 출력 설정(텍스처 LOD 그룹, 셰이더 형식 목록 등)이 바뀌면 그 프로파일의 셰이더 키가 모두 바뀐다. 반대로 셰이더 형식 목록을 바꾸면 같은 프로파일의 텍스처 키도 바뀐다. 이것은 공통 규칙의 비용으로 받아들인다.
- 엔진 세대 Guid(`EngineNuggetGeneration`)를 바꾸면 셰이더 키도 모두 바뀐다(오염 회수 — AssetSystem 9.1절 결정 4).

Apple 도구 지문: `metal -v --target=…`의 버전 줄과 `Target:` 줄(UE도 이 출력을 해석한다, `MetalShaderFormat.cpp:575–660`, 호출 `:882–920`)과 도구 조회가 `xcrun --find`로 고정한 `metal`·`metallib` 실행 파일의 SHA-256이다. 배포 대상(`-mmacosx-version-min` 등)은 SDK 기본값에 기대지 않고 형식 정의에 명시한다.

참고로 UE 5.8의 셰이더 job 키(BLAKE3)는 엔진을 빌드한 컴파일러·호스트를 넣지 않는다. 캐시 버전 GUID(`Runtime/RenderCore/Private/ShaderCompilerCore.cpp:5`), 형식 버전, 대상, 엔트리, 컴파일 환경, 전처리 결과, 루트 파라미터 레이아웃 서명, `PreferredShaderCodeFormat`·`bBindlessEnabled`·셰이더 파이프라인 여부·메타데이터 지시문 등을 넣는다(`ShaderCore.cpp:4612–4671`, `:4627`·`:4634`·`:4636`·`:4645–4652`). 형식 버전에 넣는 도구 버전 해시(DXC는 `dxcompiler.dll`·`dxil.dll` 이미지의 xxhash, ShaderConductor도 같은 방식)는 Windows가 아니면 상수라(`DXCWrapper.cpp:28–67, 109–116, 137–139`, `:64–66`), macOS에서 만드는 Metal·Vulkan·GL 형식은 도구가 바뀌어도 UE 키가 바뀌지 않는다(`Developer/Apple/MetalShaderFormat/Private/MetalShaderFormat.cpp:117,560`, `Developer/VulkanShaderFormat/Private/VulkanShaderFormat.cpp:65,117`, `ShaderFormatOpenGL.cpp:145`). Rocks는 모든 호스트에서 도구 SHA를 키에 넣고, 공통 규칙을 우선해 툴체인 지문도 넣는다.

**값과 결정성.** AssetSystem §10 결정성 규칙(같은 도구 빌드에서 호스트 무관하게 같은 바이트)을 셰이더에도 적용한다.

- **호스트 사이 동일.** 두 호스트가 모두 만드는 형식(DXIL·SPIR-V·ESSL·MSL 텍스트)은 같은 원본 태그·patch로 빌드한 도구에서 Windows와 macOS가 같은 바이트를 내야 한다. 도구 바이너리는 호스트마다 다르므로 SH0에서 잰다(측정 S3). 다르면 원인(도구 빌드 차이, 호스트 libm, 출력 형식화)을 고칠 방법을 정하기 전에는 그 형식을 공유 쿠킹에 쓰지 않고, 방법은 사용자 결정으로 올린다. 한 호스트만 만드는 형식(DXBC·metallib)은 해당 없음이다.
- 값은 바이트코드, 정규화된 바인딩 layout, GLES 바인딩 머리·복사 표, 진단, 디버그 심볼을 부분으로 나눈다(`.rnug` 부분 구조, 5.2절 N2).
- 경로 누출 금지: 입력은 메모리 버퍼와 가상 이름으로 넘기고 `#line`에는 가상 경로만 둔다. 디버그 정보는 별도 부분으로 빼고 Apple 도구에는 prefix map을 쓴다. "작업 디렉터리를 바꿔도 같은 바이트" 시험을 둔다.
- SPIRV-Cross의 실수 출력(`%.32g`)에서 locale 소수점은 SPIRV-Cross가 보정하므로(업스트림 `spirv_common.hpp`의 `fixup_radix_point`) CRT `%g` 구현 차이만 남는다. 같은 호스트 키 안에서도 OS의 CRT 버전은 다를 수 있다. 워커를 C locale로 고정하고, 극값(비정규수·최대값·17자리 이상 필요한 값) fixture로 출력이 같은지 시험한다. 다르면 추적 가능한 SPIRV-Cross patch(8절 4항)로 고정한다. MSL·ESSL·HLSL 5.0 모두 해당한다.
- **상수 접기.** DXC(LLVM)와 SPIRV-Tools 최적화기가 초월 함수 상수식을 호스트 libm으로 접으면, 같은 호스트 키 안에서도 CPU(예: UCRT의 FMA3 분기, [핵심 데이터 기반](CoreDataFoundations.ko.md))에 따라 바이트가 다를 수 있다. SH0에서 상수 초월 함수 fixture를 FMA3 유무와 OS 버전을 바꿔 비교한다(측정 S3). 다르면 결정성 규칙을 지킬 방법(도구 빌드 patch, 해당 최적화 끄기 등)을 정하기 전에는 그 형식을 공유 캐시에 올리지 않고, 방법은 사용자 결정으로 올린다.
- **dxil 서명.** UE 5.8은 DXC 1.8.2403 기반 수정본을 쓰고 `dxil.dll`을 명시적으로 열며, 이 DLL이 없으면 서명되지 않은 DXIL이 조용히 나온다고 적는다(`DXCWrapper.cpp:100`, `:111–113`). DXC 1.8.2505부터는 외부 `dxil.dll`을 찾지 않고 내장 validator가 서명한다(업스트림 DXC `docs/ReleaseNotes.md`, 1.8.2502에서 validator hash 공개. UE 동봉본(1.8.2403)으로는 확인할 수 없어 측정 S1로 확인한다). Rocks는 DXC를 1.8.2505 이상으로 고정하고 `dxil.dll`을 쓰지 않는다. 결과 DXIL container 헤더의 hash digest가 0이거나 preview용 우회 값(`PreviewByPassHash`, 0x02×16, 업스트림 `include/dxc/DxilContainer/DxilContainer.h`)이면 job을 실패시킨다. macOS DXC에서도 서명되는지 SH0에서 실측하고(측정 S1), 서명되지 않으면 `D3D12_SM6`의 생산 호스트를 형식 정의에서 Windows로 고정한다.

완료 기준: A3 시험(적중·무효화·손상·동시 기록·GC — A3 구현에 있다)에 더해 다음을 시험한다(SH1b·SH1d).
- job 키의 입력별 무효화: 옵션, 형식 정의, 도구 SHA, Apple 도구 지문, 툴체인 지문, 결정적 수학 버전, 세대 Guid 둘, 처리기 버전, 단계·엔트리, 전처리 결과, 레이아웃 서명, 프로파일 출력 설정을 하나씩 바꾼다. 경로·시각·locale·작업 디렉터리·옵션 기록 순서만 바꾸면 같은 키다.
- 키 golden: `NuggetKeyTrace` 줄의 순서·태그·필드 이름이 고정 fixture와 같다. 호스트·빌드마다 다른 줄(공통 머리의 툴체인 지문·결정적 수학 버전, 도구 SHA)은 실행 중 값으로 기대값을 채운다(A3 `NuggetKeyTests`와 같은 방식). 리터럴 키 byte는 그 두 항목을 고정값으로 넣는 시험용 기록 경로에서만 비교한다.
- 빈 캐시와 찬 캐시의 결과 byte가 같고, system 도구가 없는 형식의 찬 캐시는 워커를 띄우지 않는다(system 도구 형식은 키 계산에 도구 조회가 필요하다 — 결정 SC11).
- 만들 수 없는 형식의 `UnsupportedCapability`(macOS의 DXBC).
- 실수 출력·상수 접기 fixture.

## 11. SH1 워커 통신

UE 5.8의 방식(파일 기반):
- 메인 프로세스가 `ShaderCompileWorker <작업 디렉터리> <부모 PID> <스레드 번호> <입력 파일> <출력 파일> -communicatethroughfile -TimeToLive=<초>`로 워커를 띄운다(`Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp:1687–1696`, 워커 쪽 `Programs/ShaderCompileWorker/Private/ShaderCompileWorker.cpp:642,647`).
- 입력 파일은 입력 형식 버전(`ShaderCompileWorkerInputVersion`, `Runtime/RenderCore/Public/ShaderCompilerCore.h:35`)으로 시작하고, 압축 형식 이름과 (압축했으면) 압축된 본문이 뒤따르며, 형식별 버전 표는 본문 안에 있다. 워커는 둘 중 하나라도 자기 것과 다르면 끝난다(`Runtime/RenderCore/Private/ShaderCompileWorkerUtil.cpp:942–955, 970–1017`).
- 워커는 입력 파일을 읽고 지우며(`ShaderCompileWorker.cpp:407–419`), 결과를 임시 파일에 쓴 뒤 출력 이름으로 옮긴다(`:425–433`, `:227–231`). 그래서 메인 프로세스가 덜 쓴 출력을 읽지 않는다.
- 부모 PID로 부모 생존을 확인하고(`:460–469` macOS·Linux, `:483–507` Windows), 새 입력이 TimeToLive(기본 20초, 빌드 머신 600초, `ShaderCompiler.cpp:303–304`) 동안 없으면 끝난다.

Rocks 규칙:
1. **파일 기반**(UE와 같다). 작업 디렉터리는 `<root>/Intermediate/ShaderWorkers/<부모 PID>/<워커 번호>/`이다(root는 10절 너겟 root와 같다 — 프로젝트 루트 또는 엔진 폴더). 메시지는 Serialization Record TLV 기반 schema로 크기·정렬·endianness가 정의된 직렬화 메시지다(계획 §2.5 "프로세스/worker 경계에는 … 직렬화 메시지 또는 검증된 C ABI만 사용한다"). 메시지 본문은 파일로 주고받고, 짧은 제어 줄(묶음 번호·완료 알림)만 P1 pipe로 보낸다(11.1절 결정 SC4 — UE의 입력 파일 대기 polling 대신).
2. **입력 머리.** 메시지 형식 버전, 형식별 처리기 버전 표(형식 정의 `processor.version`), 도구 표 SHA, 툴체인 지문·결정적 수학 버전(11.1절 결정 SC5). 워커는 자기 것과 다르면 정해진 종료 코드로 끝내고, 메인 프로세스는 "워커를 다시 빌드하라"는 진단을 낸다.
3. **도구 조회.** 메인 프로세스는 세션마다 처음 한 번 워커에 도구 조회 요청을 보낸다. 워커는 저장소 도구의 SHA가 도구 표와 같은지와 Apple 도구 지문(`metal -v` 문자열, `metal`·`metallib` SHA-256)을 돌려준다. 메인 프로세스는 도구를 직접 실행하지 않고 이 값을 job 키 3항(10절)에 쓴다. 도구 조회 결과가 세션 중에 바뀌면(시스템 도구 갱신) 남은 job을 실패시키고 다시 조회한다.
4. **job.** 키, 형식 id, 단계, 엔트리, 전처리된 원문(가상 경로 `#line`), 컴파일 옵션, 파라미터 레이아웃 표(7.2절 5항)다. 워커는 원본 파일 경로를 받지 않는다(10절 경로 누출 금지). 한 입력 파일에 job 여러 개를 묶으며, 묶음 크기(job 수·원문 바이트 상한)는 측정 S8로 정해 설정으로 둔다.
5. **출력.** 임시 파일에 쓰고 no-replace `Move`(A0-P)로 출력 이름에 옮긴다. 입력 파일은 읽은 뒤 지운다. 디버그용으로 입력을 남기는 선택(UE `-KeepInput`)을 둔다.
6. **부모 감시.** Windows는 P1 job object가 부모 비정상 종료 때 워커를 끝낸다. macOS는 프로세스 그룹이 부모 종료에 따라 끝나지 않으므로 워커가 입력 pipe EOF(P1 lifeline)로 부모 종료를 알고 끝난다. 끝나기 전에 자기가 띄운 자식(Apple `metal` 등, P1에서 자기 프로세스 그룹을 가진다)을 `Process::Close`로 끝낸다. 워커가 강제 종료되어 이 정리를 못 하면 남은 자식은 `RunProcess` 시간 제한으로 끝난다. PID 확인(`IsProcessRunning`)은 PID 재사용 때문에 보조로만 쓴다.
7. **유휴 종료.** UE는 워커가 TimeToLive(기본 20초, 빌드 머신 600초) 동안 새 입력이 없으면 스스로 끝난다. Rocks 워커에는 타이머가 없다: 관리자가 유휴 시간(같은 기본값, 설정)이 지난 워커의 입력 pipe를 닫고, 워커는 EOF를 받으면 끝난다(11.1절 결정 SC4).
8. **캐시.** 워커는 너겟을 읽거나 쓰지 않는다(`Rockpile.ko.md` 4절. 계획 §27.7 개정은 Rockpile 17절에 있다). 메인 프로세스가 조회·게시한다.
9. **실패.** 워커가 비정상 종료하면 그 묶음의 job을 종료 종류·코드와 함께 실패로 보고하고 다른 묶음은 계속한다.

### 11.1 SH1 설계(개정 23, 2026-11-23)

SH1은 사용자 지시("에셋 처리 끝나고 남은 것들 이어가자", 2026-11-23)로 착수한다. 범위는 16절 SH1 행이고, 결정은 권장안으로 확정했다(애매한 곳은 UE 5.8 `ShaderCompileWorker`·`ShaderPreprocessor`·`ShaderCompilerCommon`·RenderCore `ShaderCore`를 따랐다). 결정 번호는 SC1–SC14다(5.4절 측정 S1–S14와 구분한다. 측정은 늘 "측정 Sn"으로 적는다). 교차 검증 1회(필수 11·권장 16)를 반영했다.

**UE 대응표**(이름은 Rocks식, UE 이름은 이 표에만 둔다).

| Rocks | UE 5.8 |
|---|---|
| `ShaderSourceTree`(가상 경로 → 디렉터리 mount) | `AddShaderSourceDirectoryMapping`·`GShaderSourceDirectoryMappings`(`Runtime/RenderCore/Private/ShaderCore.cpp:4218–4250`), `CheckVirtualShaderFilePath`(`:1620–1670`) |
| `PreprocessShader` | `PreprocessShader`(`Developer/ShaderPreprocessor/Private/ShaderPreprocessor.cpp`, stb_preprocess) |
| `Platform.rsh` | `Engine/Shaders/Public/Platform.ush` |
| `BuildShaderJobKey` | `FShaderCompileJob::GetInputHash`(`ShaderCore.cpp:4612–4671`) |
| `ShaderCompileOptions` | `FShaderCompilerEnvironment`의 컴파일 플래그 부분 |
| `ShaderCompileManager` | `FShaderCompilingManager`(`Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp`) |
| `ShaderWorkerPool`·워커 채널 | `FShaderCompileThreadRunnable`(같은 파일) |
| `ShaderFormatBackend`(형식 컴파일 인터페이스) | `IShaderFormat`(`Developer/TargetPlatform/Public/Interfaces/IShaderFormat.h`) |
| `ShaderWorkerExit`(워커 종료 코드) | `FSCWErrorCode`(`Runtime/RenderCore/Internal/ShaderCompileWorkerUtil.h:11–30`) |
| `ShaderCompilerCommon` 모듈 | `Developer/ShaderCompilerCommon`(형식 모듈 공용 도구) |
| 동반 타깃(`CompanionTargets`) | 에디터 빌드가 ShaderCompileWorker를 함께 빌드(`Programs/UnrealBuildTool/ProjectFiles/VisualStudio/VCProject.cs:2254` `bEditorDependsOnShaderCompileWorker`) |

결정:

- **SC1 모듈.** 3절 표대로다. 의존 방향은 `ShaderCompileWorker`(프로그램) → `ShaderWorker` → `ShaderFormat*` → `ShaderCompilerCommon`이고 정적 링크다(동적 모듈 아님). `ShaderCompilerCommon`은 두 프로세스가 같이 쓰는 것만 가진다: 형식 정의 읽기(지금 `ShaderCompiler`에 있는 `ShaderFormatDefinition`을 동작 불변으로 옮긴다), 워커 메시지 형식·종료 코드, 형식 컴파일 인터페이스 `ShaderFormatBackend`(형식 정의의 `compiler` 칸 struct `ShaderFormatCompiler`와 이름이 겹치지 않게 한다). `ShaderCompiler`는 메인 프로세스 전용(소스 트리·전처리기·키·옵션·너겟·관리자·워커 풀)이고 AssetNugget에 의존한다. 워커 쪽(`ShaderWorker`·`ShaderFormat*`·`Programs/ShaderCompileWorker`)은 `ShaderCompiler`·`AssetNugget`에 의존하지 않는다(11절 8항을 모듈 의존으로 강제 — 정책 `shader-worker-isolation`). 정책 `shader-tool-table`·`shader-tool-binary-load`의 허용 목록에 `ShaderCompilerCommon`(도구 표만)·`ShaderWorker`(도구 열기)를 더한다. 기존 `ShaderFormatVulkan`은 Sandbox의 개발용 컴파일 훅(`graphics::ShaderSourceCompiler`, Graphics 의존)을 SH2까지 그대로 두고, 같은 모듈에 `VULKAN_SM5` `ShaderFormatBackend`를 더해 워커가 링크한다. 훅과 Graphics 의존은 SH2에서 쿠킹 경로가 생길 때 지운다.
- **SC2 소스 트리와 가상 경로.** `ShaderSourceTree`가 가상 루트 → 실제 디렉터리 mount 표를 가진다. 루트는 `/Engine`(엔진 `Engine/Shaders`, 그 아래 `Public`·`Private`·`Shared`)·`/Plugin/<이름>`·`/Project`다. mount 규칙은 UE `AddShaderSourceDirectoryMapping`과 같다: 가상 디렉터리는 `/`로 시작하고 `/`로 끝나지 않으며 `.`이 없고 중복 mount는 거부, 실제 디렉터리는 있어야 하고(PlatformBase `DirectoryExists`) 그 아래 `Generated` 디렉터리가 있으면 거부한다(모든 mount — UE `ShaderCore.cpp:4245–4248`). `/<루트>/Generated/…`는 메모리 원문(생성 HLSL, SH3) 전용이다. 파일 이름 규칙은 3절(`.rsf`·`.rsh`, `Shared`만 `.h`)이고 경로 성분은 ASCII `[A-Za-z0-9_.-]`만(`..`·`.`·빈 성분·`\` 거부). 시험은 임시 fixture 디렉터리를 `/Plugin/<이름>`으로 mount한다.
- **SC3 전처리기(결정 6).** `PreprocessShader(tree, entryPath, defines, out)` — C 전처리기 부분집합을 Rocks가 직접 구현한다(외부 라이브러리 금지 규칙과 결정 6).
  - 입력 정규화: 파일 앞 UTF-8 BOM을 떼고 CRLF·CR을 LF로 바꾼 뒤 처리한다(include 폐포의 내용 SHA-256은 원래 파일 byte로 계산한다). UTF-8이 아니면 오류.
  - 지원: 줄 이음(`\` + 줄바꿈), 주석 제거(문자열 안 제외), `#include "…"`(`/`로 시작하면 가상 절대 경로, 아니면 포함한 파일 디렉터리 기준 상대 — UE `ShaderPreprocessor.cpp:436–441`), `#pragma once`(정규화된 가상 경로 기준), `#define`·`#undef`(객체형·함수형, `#`·`##`, `__VA_ARGS__`), `#if`·`#ifdef`·`#ifndef`·`#elif`·`#else`·`#endif`(`defined`, 64-bit 정수 산술·비교·논리·삼항), `#error`(진단으로 실패), `#line` 출력, `__LINE__`·`__FILE__`(가상 경로). 그 밖의 `#pragma`(예: `pack_matrix`·`warning`)는 그대로 출력에 남긴다(컴파일러가 해석한다).
  - 컴파일러 내장 매크로: Rocks가 먼저 전처리하므로 `__SHADER_TARGET_MAJOR`·`__SHADER_TARGET_MINOR`·`__SHADER_TARGET_STAGE`·`__HLSL_VERSION`·`__spirv__`·`__DXC_VERSION_*` 같은 이름이 `#if`에서 조용히 0이 된다. UE는 이 값을 직접 정의해 넣지만(`Developer/Windows/ShaderFormatD3D/Private/ShaderFormatD3D.cpp:196–201`) Rocks는 `D3D11_SM5`의 대체 경로처럼 전처리 뒤에 컴파일러가 정해지는 경우가 있어 값이 하나로 정해지지 않는다. 그래서 `#if`·`#ifdef`·`#ifndef`·`#elif`·`defined`에 `__`로 시작하는 이름(`__LINE__`·`__FILE__` 제외)이 나오면 위치와 함께 오류로 막고, 작성자는 SC10의 `ROCKS_*` 정의를 쓴다. 조건 밖 원문에 쓴 내장 이름은 그대로 남아 실제 컴파일러가 펼친다.
  - 받지 않는 것: `#include <…>`, 절대 OS 경로 include, trigraph, `#include_next`, `__DATE__`·`__TIME__`(결정성). 만나면 위치와 함께 오류다.
  - 출력: 원문 줄 구조를 유지하고 파일이 바뀌거나 줄이 건너뛸 때 `#line N "<가상 경로>"`를 낸다. 결과는 결정적이다(같은 입력 같은 byte, 줄 끝 `\n`, 탭·공백 보존). include 폐포(가상 경로·내용 SHA-256 목록, 정렬)를 함께 돌려준다(셰이더 맵 키, 3절).
  - 한도: include 깊이 64, 매크로 전개 깊이 256, 파일 하나 16 MiB, 출력 64 MiB(이름 있는 상수). 넘으면 위치와 함께 오류.
  - 정합성(측정 S7): fixture 원문을 (가) Rocks 전처리 결과로 DXC 컴파일, (나) DXC가 직접 원문을 전처리해 컴파일(include handler가 같은 가상 경로를 주고, SC10 정의를 `-D`로 같이 준다)해 DXIL byte가 같고, 일부러 넣은 오류의 진단 위치(가상 경로·줄)가 같은지 본다. 내장 매크로를 `#if`에 쓴 fixture는 Rocks 쪽 오류를 확인한다. 시험 runner(EngineTests)가 도구를 연다(9절 정책 허용).
- **SC4 워커 통신과 워커 채널.** 메인 프로세스가 P1 `Process`로 워커를 띄운다: `ShaderCompileWorker --work-dir <dir> --parent-pid <pid> --worker <n> [--keep-input] [--test-hooks]`.
  - 입력 pipe는 lifeline이자 제어 통로다. 관리자는 `input.<seq>` 파일을 임시 이름에 다 쓴 뒤 no-replace 이동으로 놓고 `<seq>\n` 한 줄을 쓴다. 워커는 그 줄을 읽어(blocking) 입력을 읽고 지우며(`--keep-input`이면 남긴다), 결과를 임시 파일에 쓴 뒤 `output.<seq>`로 옮기고 출력 pipe에 `done <seq>\n`을 쓴다. 관리자는 그 줄을 받은 뒤에만 출력을 읽는다. 파일 존재 polling이 없다(UE는 입력 파일을 기다리며 잔다). 입력 EOF면 워커는 자기 자식을 정리하고 끝난다(11절 6항).
  - 출력 pipe(stdout)는 제어 줄 전용이다. 워커 로그는 작업 디렉터리의 `worker.log` 파일로 보내고 stderr는 `Null`로 둔다(읽지 않는 pipe가 차서 워커가 멈추는 교착을 막는다 — `Process.h`의 pipe 계약).
  - **워커 채널**: P1 `Process`는 한 객체를 한 번에 한 스레드만 다루고 다른 스레드에서 막힌 Read를 깨우는 취소가 없다(`Process.h` 스레드 계약, 후속). 그래서 관리자는 워커마다 전용 채널 스레드를 두고 그 스레드가 그 `Process`의 시작·쓰기·읽기·종료를 모두 한다. 다른 스레드는 채널의 요청 큐로만 일을 준다. 진행 중 묶음은 취소하지 않고 끝을 기다린다(워커 강제 종료는 채널 스레드 자신이 한다 — SC7). 관리자 종료는 큐에 종료를 넣고, 채널 스레드가 진행 중 묶음 뒤 입력 pipe를 닫고 종료 코드를 회수한다. P1에 교차 스레드 깨우기가 생기면 진행 중 묶음 취소를 더한다.
  - 작업 디렉터리: 관리자는 시작할 때 자기 `<PID>` 디렉터리를 비우고(PID 재사용으로 남은 `input.*`·`output.*`가 no-replace 이동을 막지 않게), 다른 `<PID>` 디렉터리 가운데 그 PID가 살아 있지 않은 것을 지운다(PID 재사용 때문에 살아 있다고 보인 것은 남긴다 — 보수적).
- **SC5 메시지 형식.** 입력 파일 `{magic "RSWI", u32 메시지 버전, Record TLV}`, 출력 파일 `{magic "RSWO", u32 메시지 버전, Record TLV}`(little-endian). 입력 Record의 머리: 도구 표 SHA-256(도구 표 전체의 정규 인코딩 SHA), 형식별 처리기 버전 표(형식 id → 형식 정의의 `processor.version` 정수), 메인 프로세스의 `build::ToolchainFingerprint()`와 `deterministic::Version`. 본문: 요청 종류(`ToolQuery`·`Compile`), 쓰는 형식 정의 byte(형식 id별 — 키에 넣은 바로 그 snapshot byte, 워커가 같은 파서로 읽는다), job 목록(키, 형식 id, 단계, 엔트리, 전처리 원문, 옵션, 레이아웃 표 정규 byte). 출력 Record: job마다 상태(성공·컴파일 오류·도구 오류·`UnsupportedCapability`)·값 목록(10절 ValueId)·진단, `ToolQuery`면 도구별 확인 결과와 system 도구 지문. 크기 상한(입력·출력 파일 각 1 GiB, 이름 있는 상수)을 넘는 묶음은 만들지 않는다.
- **SC6 종료 코드와 버전 불일치.** `ShaderWorkerExit`: `Success` 0, `GeneralFailure` 1, `BadMessageVersion` 2, `BadFormatVersion` 3, `BadToolTable` 4, `BadInputFile` 5, `CannotDeleteInput` 6, `CannotSaveOutput` 7, `BadArguments` 8, `OutOfMemory` 9, `BadToolchain` 10(UE `FSCWErrorCode` 대응 — 위 표. `BadToolchain`은 Rocks가 더한 것이다: 키 머리의 툴체인 지문은 메인 프로세스 값이므로 다른 컴파일러로 빌드된 오래된 워커를 여기서 잡는다). 메시지 버전·처리기 버전 표·도구 표 SHA·툴체인 지문·결정적 수학 버전이 워커 자기 것과 다르면 출력 없이 해당 코드로 끝나고, 관리자는 "ShaderCompileWorker를 다시 빌드하라"(버전·툴체인)·"도구 표가 다른 빌드다"(도구) 진단으로 그 묶음 job을 실패시킨다(11절 2항). 그 밖의 비정상 종료(코드 밖 값·강제 종료·충돌)는 종료 종류·코드와 함께 그 묶음 job만 실패로 보고하고 다른 묶음은 계속한다(11절 9항). 워커는 오류 대화상자를 띄우지 않는다(Launch 크래시 수집 설정, abort·assert 대화상자 억제 — 도구 probe 규칙과 같다).
- **SC7 시험 hook과 프로세스 감시 시험.** `--test-hooks`로 띄운 워커만 시험 job 종류를 받는다: `Hold`(출력 pipe에 `held <seq>\n`을 쓰고 입력 pipe의 다음 줄까지 막힌다), `SpawnChild`(P1으로 지정 probe를 자식으로 띄우고 PID를 출력 pipe에 쓴 뒤 `Hold`), `Crash`(접근 위반), `Assert`(Fatal 경로). 모두 명시 gate다.
  - 강제 종료: 관리자 채널에 시험 hook(함수)을 걸면 채널 스레드가 `held` 줄을 받았을 때 hook을 부르고, hook이 종료를 고르면 채널 스레드가 그 워커를 끝낸 뒤 보고를 확인한다(교차 스레드 Terminate가 없다 — SC4).
  - 충돌·대화상자: `Crash`·`Assert` job의 묶음이 실패로 보고되고 다른 묶음은 성공하며, 대화상자 없이 끝나는지(시험이 시간 상한 안에 끝남) 본다.
  - 부모 종료: 중간 부모 probe가 워커를 띄우고, 워커가 손자를 띄워 PID들을 알린다. 시험은 중간 부모에게 명령 줄을 보내 **스스로** 비정상 종료하게 하고(시험이 중간 부모 `Process`를 Terminate하면 Windows job object가 자손을 함께 죽여 시험이 저절로 통과하므로), 워커·손자의 종료를 기다린 뒤에야 중간 부모 `Process`를 닫는다.
  - 기다림은 `WaitForProcessExit(pid, timeoutMs)`다. 같은 이름·시그니처가 지금 `TestSupport/HostFailure.h`에 Windows만 있다. 이것을 PlatformBase로 옮겨(Windows `OpenProcess(SYNCHRONIZE)`+대기, macOS `kqueue` `EVFILT_PROC`·`NOTE_EXIT`, Linux `pidfd_open`+`poll`) TestSupport가 부르게 한다(공용 유틸리티 중복 금지). sleep·polling이 없고 시간 상한은 실패 판정에만 쓴다. 한계: PID로 여는 대기라 그 사이 PID가 재사용되면 다른 프로세스를 기다린다(11절 6항과 같은 한계 — 시험은 PID를 받은 직후 기다림을 연다).
- **SC8 단계.** 새 단계 enum을 만들지 않고 형식 정의의 `ShaderStageBits`(vertex 1·pixel 2·compute 4) 한 비트를 job 단계로 쓴다. 키·메시지·명령이 이 값과 형식 정의 어휘(`vertex`·`pixel`·`compute`)를 쓴다. Graphics `graphics::ShaderStage`(0부터)와의 대응은 SH2의 쿠킹 경로가 정한다.
- **SC9 컴파일 옵션.** `ShaderCompileOptions`(`ROCKS_STRUCT`, `ShaderCompiler` 모듈): `hlsl2021`(bool, 기본은 형식 정의 `language.hlsl2021`), `optimization`(enum `O0`·`O1`·`O2`·`O3`, 기본 `O3`), `debugSymbols`(bool, 기본 false — SH1은 true를 `Unsupported`로 거부하고 SH4에서 값 3을 만들 때 연다), `matrixLayout`(enum, `Row` 하나 — 7.2절 6항 고정). 키에는 `WriteReflected`로 기본값 포함 전체가 들어간다(10절 6행). 명령·API가 같은 옵션을 두 번 주거나 모르는 이름·값이면 job 생성 오류다.
- **SC10 전처리 정의와 `Platform.rsh`.** 메인 프로세스가 job마다 정의를 넣고 전처리한다: `ROCKS_FORMAT_<형식 id>` = 1, `ROCKS_BACKEND_<D3D12|D3D11|VULKAN|METAL|GLES>` = 1, `ROCKS_COMPILER_DXC` 또는 `ROCKS_COMPILER_FXC` = 1(형식 정의의 1차 경로), `ROCKS_SHADER_MODEL_MAJOR`·`ROCKS_SHADER_MODEL_MINOR`(형식 정의 `compiler.shaderModel` — 내장 `__SHADER_TARGET_*` 대신), `ROCKS_HLSL2021` = 0·1, `ROCKS_VERTEX_SHADER`·`ROCKS_PIXEL_SHADER`·`ROCKS_COMPUTE_SHADER` 중 하나 = 1, 그리고 job이 준 정의(퍼뮤테이션 — P14). job이 준 정의가 `ROCKS_`로 시작하거나 같은 이름이 두 번이면 job 생성 오류다. 정의는 이름순으로 원문 앞에 놓인 것처럼 적용하고, 전처리기가 엔트리 원문 앞에 `/Engine/Public/Platform.rsh`를 **강제로 include**한다(작성자가 쓰지 않는다 — UE 원문이 `Common.ush`로 `Platform.ush`를 끌어오는 자리를 고정한 것). SH1의 `Platform.rsh`는 이 정의에서 나오는 형식·컴파일러 매크로와 `ROCKS_IMAGE_FORMAT(f)` 전개(형식 정의 `compiler.imageFormat`, 5.4절 SH0 결과)만 가진다. 바인딩 매크로(`ROCKS_ROOT_CONSTANTS` 등)는 SH2다.
  - `D3D11_SM5`의 경로: `hlsl2021`이 참이면 FXC를 건너뛰고 바로 DXC → SPIR-V → SPIRV-Cross HLSL 5.0 → FXC다(옵션으로 정해지므로 결정적). 거짓이면 FXC를 먼저 쓰고, FXC가 **컴파일 오류**(오류 blob이 있는 `E_FAIL`)를 낼 때만 대체 경로를 탄다. 그 밖의 FXC 실패(`E_OUTOFMEMORY`, 다른 HRESULT, 도구 내부 오류)는 도구 오류로 job 실패이고 게시하지 않는다(같은 키에서 다른 출력이 나오지 않게). 두 경로 모두 FXC 정의(`ROCKS_COMPILER_FXC`)로 전처리한 같은 원문을 쓴다.
- **SC11 관리자와 워커 풀.** `ShaderCompileManager`가 job을 받아 전처리 → (system 도구 형식이 있으면) 도구 조회 → 키 계산 → 너겟 일괄 조회 → 미적중을 묶음으로 나눠 워커 채널에 보냄 → 결과 게시 순서로 돈다. 조정은 관리자의 조정 스레드가 하고(너겟 요청을 기다리는 스레드 — 10절), 워커 채널 스레드는 묶음 실행만 한다.
  - 도구 조회(11절 3항): 세션에서 처음 컴파일을 보낼 때 워커 하나에 `ToolQuery`를 보내 저장소 도구 확인과 system 도구 지문을 얻는다. system 도구(Apple `MetalToolchain`)를 쓰는 형식이 job에 있으면 키를 계산하기 **전에** 한다(지문이 키 2행에 들어간다). 도구 조회에서 `metal`·`metallib`의 절대 경로를 `xcrun --find`로 한 번 고정하고 그 파일을 SHA-256한다.
  - 워커 수는 설정(기본 `min(논리 코어 - 1, 8)`, 최소 1), 묶음 상한은 job 32개·원문 합 4 MiB(이름 있는 상수, 측정 S8: 워커 열기 비용이 Release 약 80 ms라 묶음으로 나눠 갚는다). 결과는 job 순서 그대로 돌려준다(워커 배정 순서와 무관).
- **SC12 필요 시 컴파일.** Development 실행이 쿠킹된 셰이더에 없는 job을 만나면 관리자의 `CompileNow(job)`(동기, 한 건)을 부른다(UE의 개발 중 셰이더 컴파일과 같은 자리). SH1은 API·시험까지이고 런타임 연결은 SH2다(쿠킹된 라이브러리 경로와 함께). Release 실행 파일은 이 모듈에 닿지 않는다(9절 정책).
- **SC13 명령.** Commandlet 호스트에 `shader-compile`을 둔다: `shader-compile [--project <dir>] [--profile <엔진 프로필 id>] [--format <id>]... [--define NAME=VALUE]... [--option NAME=VALUE]... [--out <dir>] <가상 경로>:<엔트리>:<vertex|pixel|compute>...`. SH1의 `--profile`은 엔진 컴파일 프로필만 받는다(기본 호스트 프로필). job마다 키(hex)·적중 여부·산출 파일을 출력하고, `--out`이 있으면 `<out>/<형식>/<이름>.<엔트리>.<단계>.<확장자>`에 값 0을 쓴다. `<이름>`은 가상 경로의 `/`를 `+`로 바꾼 것이다(`+`는 가상 경로 문자가 아니므로 이름이 겹치지 않는다 — SC2). 종료 코드 0은 전부 성공, 1은 job 실패가 있음, 2는 사용법 오류.
- **SC14 동반 타깃.** 워커는 독립 프로그램 타깃 `ShaderCompileWorker`다. 이 실행 파일을 쓰는 타깃(Commandlet·Sandbox·EngineTests)이 같이 빌드하도록 Rocks.Build `TargetRules`에 `CompanionTargets`(함께 빌드해 같은 bin에 두는 프로그램 타깃 목록)를 더한다(UE 에디터 ↔ ShaderCompileWorker 대응, BuildTargets 문서에 함께 적는다). 구성은 요청한 타깃과 같다(UE는 에디터 구성과 관계없이 워커를 Development로 빌드하지만 — `VCProject.cs:2257` — Rocks는 구성을 하나 더 빌드하지 않는다. Debug 워커의 도구 열기 비용 약 1.2 s(측정 S8)는 받아들이고 시험은 워커를 적게 띄우게 짠다). 워커 실행 파일은 호출하는 실행 파일과 같은 폴더에서 찾는다. 시험 hook은 `--test-hooks`로만 켠다.

하위 단계(각각 첫 검증 1회 뒤 engine-runtime에 병합):

| 단계 | 범위 | 시험 |
|---|---|---|
| SH1a | `ShaderSourceTree`(SC2)·`PreprocessShader`(SC3)·`Engine/Shaders/Public/Platform.rsh`(SC10의 매크로 부분)·전처리 정의 조립(SC10) | 가상 경로·mount 음성 대조(`Generated`·문자·중복), BOM·CRLF, 지시문 하나씩·중첩·`#pragma once`·상대 include·매크로(`#`·`##`·가변 인자·재귀 금지)·조건식, 내장 매크로 조건 거부, 예약·중복 정의 거부, `#line`·`__FILE__`, 한도 경계, 결정성(같은 입력 같은 byte, 작업 디렉터리 무관), 측정 S7(DXC 대조 — 도구가 없으면 SKIP) |
| SH1b | `ShaderCompilerCommon` 분리(형식 정의 동작 불변 이동)·`ShaderCompileOptions`(SC9)·`BuildShaderJobKey`(10절)·셰이더 Record 값 표·너겟 위치(10절)·정책 `shader-worker-isolation`과 허용 목록 갱신 | 키 입력별 무효화와 불변(10절 완료 기준), 키 golden(`NuggetKeyTrace`, 실행 중 지문으로 기대값), 임시 root의 fixture `Config/AssetNuggets.json`으로 설정·위치(실제 `Engine/Intermediate`를 쓰지 않는다), 값 표 왕복 |
| SH1c | 워커 메시지(SC5)·종료 코드(SC6)·`ShaderWorker`·`Programs/ShaderCompileWorker`·`ShaderFormatBackend`·`D3D12_SM6`(DXC DXIL, 서명 검사)·`VULKAN_SM5`(DXC SPIR-V·`spirv-val`) 경로·도구 조회·워커 채널(SC4)·PlatformBase `WaitForProcessExit`(SC7)·시험 hook·동반 타깃(SC14) | 메시지 왕복·손상 거부, 버전·처리기·도구 표·툴체인 불일치 종료 코드와 진단, 도구 조회, 강제 종료 보고(gate), `Crash`·`Assert` 보고와 대화상자 없음, 부모 종료 시 워커와 손자 종료 |
| SH1d | `ShaderCompileManager`·`ShaderWorkerPool`(SC11)·너겟 일괄 요청·`CompileNow`(SC12)·`shader-compile`(SC13) | 같은 입력 같은 byte(반복·워커 수·묶음 크기·작업 디렉터리 변경), 빈 캐시와 찬 캐시 결과 동일(system 도구 없는 형식의 찬 캐시는 워커 0개), 묶음 하나 실패가 다른 묶음에 번지지 않음, DigestChanged·Corrupt·값 0 없는 Record 재컴파일, `required` 오류, 명령 출력·종료 코드·출력 이름 |
| SH1e | 나머지 경로: `ShaderFormatD3D`의 FXC(`D3D11_SM5`, SC10의 대체 경로 규칙)·`ShaderFormatOpenGL`(ESSL 320 — SH2 재작성 전 출력)·`ShaderFormatMetal`(MSL 2.4 + metallib, `MetalToolchain`이 없으면 `METAL_SM5` job 전체가 `UnsupportedCapability` — MSL만 내고 성공하지 않는다). SH0에 남은 macOS 측정(측정 S1·S3·S9)을 `shader-compile --out`으로 옮긴 뒤 `Programs/ShaderToolSpike`를 지운다(9절 진척의 약속) | 형식별 산출(원문·정의·옵션을 스파이크와 같게 맞춘 fixture에서 스파이크와 같은 byte), FXC 대체 경로(컴파일 오류만), macOS 밖 metallib `UnsupportedCapability`, 실수 출력·상수 접기 fixture(10절) |

SH1 출력은 SH2 정규화(재배치·리플렉션·GLES 재작성) 전의 형식 산출물이다. SH2가 그 처리를 더할 때 형식 정의의 `processor.version`을 올리므로 SH1 너겟은 키가 달라져 다시 만들어진다.

**SH1a 구현 메모(개정 24, 2026-11-23).** SC2·SC3·SC10을 구현했고 측정 S7을 쟀다(5.4절).

- 구조: `ShaderCompiler` 공개 헤더 `ShaderSourceTree.h`(mount·가상 경로·읽기)·`ShaderPreprocessor.h`(`PreprocessShader`·`PreprocessedShader`·한도)·`ShaderPreprocessDefines.h`(`BuildShaderPreprocessDefines`·`ShaderDefineSet`). 전처리기는 기능별 파일이다: 텍스트(정규화·논리 줄·토큰), 매크로(정의·숨김 집합·전개), 식, 지시문·출력. 재귀 금지는 숨김 집합(Prosser, C 6.10.3.4)이고, 매크로 깊이는 숨김 집합 크기와 인자 미리 전개 중첩 둘 다로 센다. 소스 트리·전처리·정의 조립은 오류 코드 하나(`ShaderSourceError`)와 고정 버퍼 결과(`ShaderSourceStatus` — 가상 경로·줄·영어 세부)를 함께 쓴다. `Engine/Shaders/Public/Platform.rsh`가 생겼다.
- 스펙이 정하지 않은 곳(UE 5.8 또는 권장안으로 정함):
  - 상대 include의 `.`·`..`는 UE(`ShaderPreprocessor.cpp:436–441`, `CollapseRelativeDirectories`)처럼 포함한 파일 디렉터리에 붙여 접은 뒤 가상 경로 규칙을 적용한다(가상 루트 위로 나가면 오류). `/`로 시작하는 include는 접지 않고 그대로 검사한다.
  - `Shared`(`.h`만) 규칙은 모든 루트의 `<루트>/Shared/`에 쓴다. 엔트리는 `.rsf`만 받는다.
  - 다른 내용의 매크로 재정의는 오류(`MacroRedefined`)다. clang은 경고 뒤 바꾸지만 job 정의·플랫폼 정의가 조용히 바뀌는 것을 막는다. 같은 내용은 받는다.
  - 조건식의 `true`·`false`는 1·0이다(C++ 규칙. DXC와 같음을 S7 fixture로 확인했다). 문자 리터럴은 조건에서 받지 않는다. 결정성 거부에 `__TIMESTAMP__`를 더했고, job 정의 이름의 `__` 접두도 예약으로 막는다.
  - 출력 공백: 원문 토큰은 앞 공백을 그대로, 전개 토큰은 공백 하나를 두고, 붙이면 다른 토큰이 되는 자리에는 공백을 넣는다(clang `-E`의 붙임 방지와 같은 목적). 같은 파일에서 8줄 이하 건너뜀은 빈 줄, 넘거나 파일이 바뀌면 `#line`이다(clang `-E`와 같은 기준). `#line`은 토큰을 낼 때만 쓰므로 토큰이 없는 파일(지금의 `Platform.rsh`)은 출력에 이름이 나오지 않는다. 다른 `#pragma`는 펼치지 않고 그 줄 그대로 둔다.
  - 한도는 `ShaderPreprocessLimits`(기본값이 SC3 상수)로 받는다. 시험이 파일·출력 경계를 작은 값으로 정확히 잰다. 출력 한도는 한 줄의 전개 중간 결과(`#if` 식처럼 출력하지 않는 전개)에도 쓴다.
  - SC10 파생 매크로는 `Platform.rsh`가 만든다: `ROCKS_SPIRV`(Vulkan·Metal·GLES면 1, D3D면 0)와 `ROCKS_IMAGE_FORMAT(Format)`(SPIR-V면 `[[vk::image_format("Format")]]`, D3D면 빈 것). 형식 정의 읽기가 `imageFormat`을 경로와 묶으므로(D3D 경로 `Empty`, SPIR-V 경로 `VkImageFormat`) backend 정의에서 같은 값이 나온다. 그래서 SC10 정의 목록에 새 정의를 더하지 않았다. `Platform.rsh`는 backend·컴파일러·단계 정의가 하나씩인지 `#error`로 검사한다.
- S7 도구: `ShaderToolLoader`에 `CompileWithDxc`(IDxcCompiler3 + include handler, Windows 구현만, 다른 호스트는 `UnsupportedHost`)를 더했다. 도구를 여는 워커 자리 모듈이라 정책 `shader-tool-binary-load` 안이고, SH1c 형식 모듈이 다시 쓸 수 있다. 시험은 시험 runner(`Programs/FoundationTests/Engine/ShaderPreprocessDxcTests.cpp`)에 둔다 — 모듈 `Tests` 폴더(`Developer/ShaderCompiler/Tests`)는 도구 열기 허용 위치가 아니다. (나)는 주 파일에서 `Platform.rsh`와 엔트리를 include하고 `-I .`를 준다: DXC(clang)는 Windows에서 `/Engine/…`를 절대 경로로 보지 않아 포함한 파일 기준 후보(`.\Plugin\S7\Engine\…`)만 찾기 때문이다. handler는 `.\`·`\`·`/./`를 가상 경로로 바꾸고 트리에 없는 후보를 거절한다. 버린 대안: 형식 모듈(SH1c)을 앞당겨 DXIL 경로를 만드는 것(단계 범위 밖), 시험 runner에 Windows 전용 폴더를 더하는 것(정책 `platform-boundary` 예외가 늘어난다).
- 한계(후속): Windows는 대소문자만 다른 가상 경로로 같은 파일을 연다. 폐포와 `#pragma once`는 철자가 다르면 다른 파일로 보고, 대소문자를 가리는 macOS에서는 그 철자가 실패한다(디렉터리 열거로 철자를 확인하는 것이 후속이다). 지원하지 않는 확장(GNU 이름 붙은 가변 인자 `args...`·`#warning`·`#line` 입력은 오류, `__VA_OPT__`·`_Pragma`·`, ## __VA_ARGS__` 쉼표 삼키기는 C 표준대로 처리하거나 그대로 남긴다). 컴파일러 진단의 줄은 원문과 같지만 열은 공백 정규화 때문에 다를 수 있다.

**SH1b 구현 메모(개정 25, 2026-11-23).** SC1의 공용 모듈 분리, SC9 옵션, 10절 job 키·Record 값 표·너겟 위치, 정책 `shader-worker-isolation`을 구현했다.

- 모듈: `Developer/ShaderCompilerCommon`을 만들고 `ShaderFormatDefinition`(헤더·구현·시험)을 동작 불변으로 옮겼다(include `<ShaderCompilerCommon/ShaderFormatDefinition.h>`, API `ROCKS_SHADERCOMPILERCOMMON_API`). `ShaderCompiler`는 `ShaderCompilerCommon`·`AssetNugget`·`HostConfig`에 의존하고 리플렉션 헤더 `ShaderCompileOptions.h`를 가진다(identity 명세 `ShaderCompiler.rocksids.json`). 키를 계산하는 쪽은 `RegisterShaderCompilerTypes`로 타입을 등록한 TypeRegistry와 ObjectRegistry를 `ShaderKeyReflection`으로 준다(AssetCooker `CookConfigContext`와 같은 방식).
- 옵션: `ShaderCompileOptions`(`hlsl2021`·`optimization` `O0`–`O3`·`debugSymbols`·`matrixLayout` `Row`). 이름=값 파서는 손으로 쓴다(ReflectionText 대입 파서는 필드 경로·컨테이너용이라 중복·SH1 미지원 값 같은 job 생성 오류를 따로 내기 어렵다). 이름은 필드 이름 그대로, 값은 `true`·`false`와 열거자 이름이며 공백을 받지 않는다. `debugSymbols=true`는 파서와 키 계산 모두 `Unsupported`다.
- 키: `BuildShaderJobKey(input, trace)` = 공통 머리(`ShaderJobProcessor` Guid 둘) + `WriteShaderJobKeyBody`(2–7행). 2–7행 함수를 따로 내놓아 시험이 머리의 결정적 수학 버전·툴체인 지문을 고정값으로 쓰는 기록 경로(리터럴 키 golden)를 만든다. 도구 설정 byte는 `hasDigest`인 Runtime·Redist·Tool 파일 SHA만 잇는다. 저장소 도구에 이 호스트 파일이 없으면(예: macOS의 FXC) 키도 `UnsupportedCapability`다. system 도구 지문은 `ShaderSystemToolFingerprint`(이름·버전 줄·`metal`·`metallib` SHA)로 받고, 형식 정의의 system 도구 이름과 맞는 지문이 없으면 `UnsupportedCapability`다(SH1c 도구 조회가 채운다). 입력은 snapshot의 digest만 받으므로 키 함수는 디스크를 읽지 않는다.
- 값: `ShaderNuggetValue`(Code 0·BindingLayout 1·GlesCopyTable 2·DebugSymbols 3)와 `ShaderJobValues`(번호별 byte + 있음 비트 — 빈 값도 있는 값이다). `MoveShaderValuesToNugget`은 Code 없는 job을 거부하고(실패 job 비게시), `MoveShaderValuesFromNugget`은 Code가 없으면 `NotFound`, 모르는 번호·중복·오름차순 아님은 `InvalidArgument`(둘 다 손상 — 다시 컴파일)다.
- 위치: `ResolveShaderNuggetRoot`(프로젝트 루트, 없으면 `FindEngineDirectory`의 `Engine`)와 `OpenShaderNuggetCache`(`ReadNuggetCacheSettings` → `NuggetCache::Open`). 저장소 `.gitignore`에 `/Engine/Intermediate/`를 더했다.
- 정책: `shader-tool-table` 허용에 `Developer/ShaderCompilerCommon`·`Developer/ShaderWorker`, `shader-tool-binary-load` 허용에 `Developer/ShaderWorker`를 더했다. `shader-worker-isolation`은 워커 쪽(`Programs/ShaderCompileWorker`·`Developer/ShaderWorker`·`Developer/ShaderFormat*`)의 규칙이 `ShaderCompiler`·`AssetNugget`을 이름으로 참조하거나 소스가 `<ShaderCompiler/…>`·`<AssetNugget/…>`를 include하면 실패다(`ShaderCompilerCommon`은 허용). 지금 `ShaderFormatVulkan`(Graphics·ShaderToolLoader·SpirvReflect 의존)은 통과한다.
- 한계: 리플렉션 문맥을 호출자가 준다(관리자 SH1d가 세션마다 한 벌 만든다). SPIRV-Reflect 항목(2행 끝)은 SH2에서 더한다.

**SH1c 구현 메모(개정 26, 2026-11-23).** SC4–SC7·SC14와 SC1의 워커 쪽 모듈, `D3D12_SM6`·`VULKAN_SM5` 경로를 구현했다.

- 모듈: `ShaderCompilerCommon`에 `ShaderWorkerProtocol`(메시지·`ShaderWorkerExit`·도구 표 SHA·요청 머리)과 `ShaderFormatBackend`(+ 프로필 이름·job 실패 도우미). 새 `ShaderFormatD3D`(`D3D12_SM6` — DXC DXIL, container hash가 0이거나 0x02×16이면 ToolError)와 `ShaderFormatVulkan`의 `VulkanShaderFormatBackend`(DXC SPIR-V + `spirv-val`). DXC 호출은 SH1a의 `ShaderToolLoader::CompileWithDxc`를 쓴다. 워커 본체 `ShaderWorker`와 프로그램 `Programs/ShaderCompileWorker`(Launch — 크래시는 `<work-dir>/Crashes`에 프로세스 안 덤프, OS·CRT 대화상자 억제). 채널은 `ShaderCompiler`의 `ShaderWorkerChannel`. `WaitForProcessExit`를 PlatformBase로 옮겼다(Windows `OpenProcess(SYNCHRONIZE)` 대기, macOS `kqueue` `NOTE_EXIT`, Android `pidfd_open` + `poll`; TestSupport 사본은 지웠다).
- 메시지: 최상위 Record 하나, 형식·job·결과 목록은 RecordList, 작은 튜플 목록(처리기 버전·hook 인자·값·도구 확인)은 Bytes 값 안의 Archive다. 옵션은 `ShaderWorkerJobOptions`(필드 값)로 싣는다 — `ShaderCompileOptions`는 메인 프로세스 모듈이라 워커가 볼 수 없다(격리 정책).
- 버전 대조(SC6 보충): 결정적 수학 버전 불일치도 `BadToolchain`이다. 처리기 버전 표 항목이 같은 요청의 형식 정의 `processor.version`과 다르면 `BadInputFile`, 워커 처리기 버전과 다르면 `BadFormatVersion`이다. 처리기가 아직 없는 형식(Metal·OpenGL)과 FXC 경로(`D3D11_SM5`)는 버전을 대조하지 않고 그 job이 `UnsupportedCapability`다(SH1e).
- 제어 줄: 워커는 `done <seq>`·`held <seq>`·`child <pid>`만 쓴다. Hold는 입력 pipe의 다음 줄(내용 무관)로 풀린다. 채널은 모르는 줄을 받으면 워커를 끝내고 `ChannelFailed`(워커를 다시 빌드하라)다. 워커가 끝난 묶음 뒤에는 다음 묶음에서 워커를 다시 띄운다.
- DXC 인자: `-E`·`-T <단계>_<모델>`·`-HV 2021|2018`·`-O0`–`-O3`·`-Zpr`·`-WX`, Vulkan은 `-spirv -fspv-target-env=vulkan1.1`과 형식 정의가 켜면 `-fvk-use-dx-position-w`.
- 동반 타깃: `TargetRules.CompanionTargets`를 Rocks.Build에 더했다(`RuleLoader.WithCompanions`가 동반 Program·Monolithic 타깃의 모듈을 같은 빌드에 더한다 — 동반 타깃의 플러그인·동반 타깃, Modular 타깃은 거부). 지금은 EngineTests만 `ShaderCompileWorker`를 동반한다. Modular 시험 runner(EngineModular)는 동반을 받지 않으므로 워커 시험이 SKIP이다. Commandlet·Sandbox 동반은 그 사용처(SH1d `shader-compile`, SH2)와 함께 더한다.
- 미룬 것: Apple system 도구 지문(`xcrun --find`·`metal -v`)은 Metal 경로와 함께 SH1e — 지금 도구 조회는 system 도구 이름만 `found = false`로 돌려준다. 다른 PID 작업 디렉터리 정리·워커 풀·묶음 크기는 SH1d.

## 12. P6a·P14·P6b 형식 정의·셰이더 타입·TargetProfile·쿠킹 매니페스트

엔진 컴파일 프로필은 형식 정의와 함께 P6a(12.1절)가, TargetProfile 필드는 A4가 쓰는 프로필 파일에 P6b(12.3절)가 더한다.

### 12.1 P6a 형식 정의와 엔진 컴파일 프로필

- **형식 정의.** `Engine/Config/ShaderFormats/<id>.json`에 둔다: 이 형식이 쓰는 도구 목록(10절 키 3항의 정적 목록), 언어 버전 기본값, 컴파일러(DXC·FXC·SPIRV-Cross 경로), 셰이더 모델·target env·ESSL 버전, 바인딩 규약 버전, ShaderFormat 처리기 버전, 상한(7.2절 4항)·지원 기능, 생산 호스트 규칙. UE에서는 기능·형식 이름 부분이 `DataDrivenPlatformInfo.ini`의 `[ShaderPlatform]`(`Language`·`MaxFeatureLevel`·`ShaderFormat`·기능 플래그, `Engine/Config/Windows/DataDrivenPlatformInfo.ini:78–139`)에 있고, SM 선택은 ShaderFormat 코드에 있다(`ShaderFormatD3D.cpp:102–150`, FXC·DXC 분기 `D3DShaderCompiler.cpp:1520–1522` `DoesShaderModelRequireDXC`). Rocks는 둘 다 형식 정의에 둔다. 형식 정의 SHA-256이 job 키에 들어간다(10절). 형식 정의에는 결정 2(언어 버전 기본값)·3(셰이더 모델)·5(생산 호스트)가 들어가므로 P6a는 그 결정 뒤다(15절 기한).
- **엔진 컴파일 프로필.** 프로젝트 프로필 없이 하는 컴파일(엔진 내장 라이브러리, Development 필요 시 컴파일)이 쓰는 호스트별 프로필이다. 런타임 backend 선택이 없는 컴파일 전용이라 TargetProfile과 스키마를 나눈다: `Engine/Config/ShaderFormats/Engine-<호스트>.json` = `{ "id", "formats": [...] }`. `Engine-Windows`는 `D3D12_SM6`·`D3D11_SM5`·`VULKAN_SM5`·`GLES31_PC`·`GLES31_ANDROID`, `Engine-MacOS`는 `METAL_SM5`·`GLES31_ANDROID`다. 그 호스트에서 쓰거나 그 호스트가 빌드하는 대상(Android 포함)이 쓰는 형식 가운데 그 호스트가 만들 수 있는 것만 넣는다(10절, 15절 결정 5에 따라 바뀐다). job 키 9항이 이 프로필에서 나온다(프로필 공통 상한은 개정 17에서 폐기, 7.2절 4항). SH1이 쓰므로 P6a에서 만든다.
- 완료 기준: 스키마 검증(모르는 속성 거부, 도구 목록이 도구 표에 있는지, 엔진 컴파일 프로필의 형식이 형식 정의에 있는지), 형식 정의 변경의 키 무효화(10절).

진척(2026-11-17, `feature/shader-tools`):
- [x] 형식 정의 `Engine/Config/ShaderFormats/{D3D12_SM6,D3D11_SM5,VULKAN_SM5,METAL_SM5,GLES31_ANDROID,GLES31_PC}.json`과 자리만 둔 `VULKAN_ANDROID`·`METAL_IOS`(schema·id·status·backend만). 속성: `schema`(1)·`id`(파일 이름과 같음)·`status`·`backend`·`processor`(ShaderFormat 처리기 이름·버전)·`bindingConvention`(바인딩 규약 버전)·`tools`(job 키 3항 정적 목록)·`language.hlsl2021`(결정 2)·`compiler`(경로와 경로별 값)·`stages`·`limits`(7.2절 4항)·`features`·`deviceRequirements`·`producers`(생산 호스트 규칙, 결정 5).
  - `compiler`: `path`(`DxcDxil`·`Fxc`·`DxcSpirv`·`DxcSpirvMsl`·`DxcSpirvEssl`), `shaderModel`(D3D12 `6_6`, D3D11 `5_0`, SPIR-V 경로 `6_0`), `imageFormat`(ROCKS_IMAGE_FORMAT 전개, D3D 경로 `Empty`·SPIR-V 경로 `VkImageFormat`), `spirvTargetEnv`(`vulkan1.1`), `dxPositionW`, D3D11 `fallbackPath`(`DxcSpirvHlsl50Fxc`), Metal `mslVersion`(2.4)·`mslPlatform`, GLES `esslVersion`(320)·`fixupClipSpace`·`flipVertexY`·`unsupportedImageFormats`. 경로마다 쓰는 속성만 받는다.
  - `limits` 값: D3D12 상수 버퍼 14·SRV 텍스처 128·구조화 버퍼 128·storage image 8·샘플러 16·UAV 합 8(Tier 1 FL 11.0), D3D11 상수 버퍼 13(+루트 상수)·SRV 텍스처 16(개정 13에서 128을 16으로, 결정 RN13), Vulkan 12·128·4·4·16(SRV 텍스처는 개정 14에서 16 → 128, 나머지는 핵심 최소 보장, render target 8), Metal 12·128·4·4·16(7.2절 4항 buffer 예산), GLES 12·16·4·4·16, render target 4, 단계 사이 vec4 15(`GL_MAX_VARYING_VECTORS`), 에뮬레이션 vec4 VS 256·PS 224·CS 128(ES 3.2 최소 보장 `GL_MAX_VERTEX_UNIFORM_VECTORS`·`GL_MAX_FRAGMENT_UNIFORM_VECTORS`·`GL_MAX_COMPUTE_UNIFORM_COMPONENTS`/4). 모든 형식이 UAV는 CS만, 루트 상수 128 byte, 단계 사이 vec4 16 이하·render target 8 이하다.
  - SH0 측정 반영: D3D12 SM 6.6과 장치 요구 `D3D12_SHADER_MODEL_6_6`(S2), `GLES31_PC`가 ESSL 320 그대로이고 장치 요구 `OPENGL_4_3`·`GL_ARB_ES3_2_compatibility`·`GL_ARB_clip_control`(S12, 310 대안 불필요), ROCKS_IMAGE_FORMAT 전개(D3D 경로에 `VkImageFormat`을 두면 거부)와 GLES `unsupportedImageFormats: [r11g11b10f]`.
  - 생산 호스트: `D3D12_SM6`은 macOS DXC 서명(S1 macOS)을 재기 전까지 Windows만, `D3D11_SM5`는 Windows, `VULKAN_SM5`·GLES는 두 호스트, `METAL_SM5`는 두 호스트이고 둘 다 `MetalToolchain` system 도구가 필요하다.
- [x] 엔진 컴파일 프로필 `Engine-Windows.json`(`windows-x64`: `D3D12_SM6`·`D3D11_SM5`·`VULKAN_SM5`·`GLES31_PC`·`GLES31_ANDROID`)·`Engine-MacOS.json`(`macos-arm64`: `METAL_SM5`·`GLES31_ANDROID`). 스키마는 `{schema, id, host, formats}`로 `host`를 더했다(생산 호스트 검사에 쓴다).
- [x] 읽기 코드 `Engine/Source/Developer/ShaderCompiler`(`ShaderCompiler/ShaderFormatDefinition.h`, Core JSON): `ParseShaderFormatDefinition`·`LoadShaderFormatDefinition`·`ParseEngineCompileProfile`·`ResolveEngineCompileProfile`·`LoadEngineCompileProfile`·`CommonLimits`(프로필 공통 상한). 검증: 모르는 속성(최상위·중첩·그 경로에 쓰이지 않는 속성·자리만 둔 형식의 속성), 어휘 밖 값, 도구가 도구 표(P4)에 있는지와 경로가 쓰는 도구가 목록에 있는지, system 도구가 형식 도구 목록에 있는지, backend–처리기–경로 짝, 프로필의 형식이 형식 정의에 있는지·자리만 둔 형식인지·프로필 호스트가 생산 호스트인지. 오류는 JSON 경로와 줄을 돌려준다.
- [x] 형식 정의 SHA-256 = 파일 byte의 SHA-256(job 키 3항). 시험: 같은 byte는 같은 값, 상한·HLSL 2021·처리기 버전·공백 하나를 바꾸면 다른 값.
- [x] 시험(EngineTests `ShaderFormat_*` 8건): 저장소 디렉터리 전체 읽기(정의 8·프로필 2), 결정·측정 값, Windows 프로필 공통 상한(상수 버퍼 12·텍스처 16·storage 4·4·샘플러 16·UAV 합 8·render target 4·단계 사이 15·에뮬레이션 256/224/128), 음성 대조(모르는 속성 6·도구 6·값 불일치 21·프로필 해석 3·프로필 스키마 4).
- [x] 개정 13(2026-11-18, Renderer RQ22 (1)·머티리얼 MSR12): `D3D11_SM5`의 `sampledTextures`를 16으로 내렸다. `D3D12_SM6`·`METAL_SM5` 128과 GLES 16은 그대로다.
- [x] 개정 14(2026-11-18, Renderer RQ11·RQ22 (1)·결정 RN3 (가)): `VULKAN_SM5`의 `sampledTextures` 16 → 128, `renderTargets` 4 → 8. 근거는 GX1 측정(RenderBase 4.4절: GTX 1060 `maxColorAttachments` 8·단계당 sampled image 1048576·set 1048576·`maxPerStageResources` 4294967295, lavapipe 8·1015808·1015808·1015808)이다. 장치 요구는 별도 어휘 없이 `limits` 자체를 장치 능력과 대조한다(7.2절 4항 "상한은 장치 요구다"). 이 형식의 PS 합산 요구는 12 + 128 + 4 + 4 + 8 = 156으로 Vulkan 핵심 최소 보장 128을 넘으므로, 그런 장치는 `VULKAN_SM5`를 쓸 수 없다. 장치 표본이 둘뿐이라 다른 공급자(AMD·Intel)의 값은 미측정이다. 프로필 공통 상한은 GLES가 들어 있어 그대로다(텍스처 16·render target 4).
- [x] 개정 16·17(Renderer RQ14), 구현 개정 18(2026-11-18): `features` 어휘에 런타임 확인 기능 `vertexViewportLayerOutput`을 더해 `D3D12_SM6`·`VULKAN_SM5`·`METAL_SM5` 형식 정의에 넣었다. 읽기 코드(`ShaderFormatDefinition.h`)에 `ShaderFeatureVertexViewportLayerOutput`, 런타임 확인 기능 묶음 `RuntimeCheckedShaderFeatures`와 `IsRuntimeCheckedShaderFeature`를 두었다(7.2절 7항, 15절 결정 12). SH2 전에 앞당겼다.
- [ ] job 키 계산(P5)과 상한 검사(워커: 모든 상한을 job 형식 상한으로, 개정 17)는 이 값을 쓰는 다음 단계에서 한다.
- [x] 개정 21(2026-11-20, 측정 S14): `D3D11_SM5` 형식 정의 `features`에 `vertexViewportLayerOutput`을 더하고 형식 정의 시험의 D3D11 단언을 바꿨다.
- [x] 개정 17(7.2절 4항), 구현 개정 18(2026-11-18): `CommonLimits`와 그 시험(Windows 프로필 공통 상한 단언)을 지우고, 시험을 형식별 상한 단언(D3D12·Vulkan 128, D3D11·GLES 16, Vulkan render target 8, GLES 에뮬레이션 vec4 등)으로 바꿨다.

### 12.2 P14 셰이더 타입·퍼뮤테이션·라이브러리 형식

- **셰이더 타입.** 이름, `.rsf` 가상 경로, 엔트리, 단계, 요구 기능(2절), 퍼뮤테이션 도메인, 거르기 함수를 가진다. 정적 등록 표로 모은다(UE `IMPLEMENT_GLOBAL_SHADER`, `Runtime/RenderCore/Public/GlobalShader.h:433`). 등록 표의 순서는 결과에 영향을 주지 않는다(이름으로 정렬해 쓴다). 개정 16(Renderer RQ13, RenderBase SX4, 머티리얼 시스템 MSR5)에서 다음 두 표지를 더했다.
  - **머티리얼 셰이더 표지.** 표지가 있는 타입은 머티리얼 쿠킹이 머티리얼 셰이더 집합마다 곱한다(Renderer 14.3절 pass 셰이더). 없는 타입은 엔진 셰이더다. 머티리얼 셰이더 타입은 pass가 요구하는 custom 출력 이름 목록을 선언하고, 그 출력이 없는 머티리얼은 거르기에서 빠진다(머티리얼 시스템 8.3절).
  - **정점 입력 축 표지.** 표지가 있는 타입은 정점 입력 타입(RenderBase 10절)마다 따로 컴파일한다. 메시 pass 셰이더와 엔진 기본 정점 입력 타입의 fallback 셰이더(RenderBase 11.1절)가 쓴다. 전체 화면·compute 셰이더는 표지가 없고 정점 입력 타입 자리가 "없음"이다.
  - UE는 이 조합을 Global·Material·MeshMaterial 타입 종류로 나눈다(`Runtime/RenderCore/Public/Shader.h:1219–1226`). Rocks는 표지 둘로 같은 구분을 한다.
- **거르기 함수.** 타입마다 하나다. 서명은 머티리얼 시스템 8.3절·10.4절 D5와 Renderer 14.3절이 정한 것으로 통일한다.
  - 머티리얼 셰이더: `bool ShouldCompile(const MaterialShaderTraits&, VertexInputTypeId, PermutationId, FormatId)`. 정점 입력 축이 없는 타입은 `VertexInputTypeId`에 "없음"을 받는다.
  - 엔진 셰이더: `bool ShouldCompile(VertexInputTypeId, PermutationId, FormatId)`. 정점 입력 축이 없으면 역시 "없음"이다.
  - 정점 입력 타입의 거르기: `bool ShouldCompile(const MaterialShaderTraits&, FormatId)`(머티리얼 시스템 8.3절). 엔진 셰이더에는 기본값 특성(머티리얼 없음)을 넘긴다.
  - **결합.** 컴파일 여부 = 요구 기능 ⊆ 형식 `features`(2절) AND 셰이더 타입 거르기 AND 정점 입력 타입 거르기(정점 입력 축이 있을 때). UE도 정점 입력 타입 거르기와 셰이더 타입 거르기를 AND로 쓴다(`Runtime/Engine/Private/Materials/MaterialShader.cpp:3274, 3287`, `Runtime/Engine/Private/Materials/MeshMaterialShader.cpp:178–186`).
  - **결정성.** 거르기는 순수 함수다. 인자와 아래 형식 사실만 읽고 설정·장치·등록 순서·실행 순서를 읽지 않는다. 쿠커(Developer)와 런타임 완결성 검사(RenderBase 9.2절)가 같은 결과를 내야 하기 때문이다.
  - **형식 사실.** 거르기는 `FormatId`로 형식 정의의 `limits`·`features`를 읽는다. 예: GBuffer +D·+E profile은 `renderTargets` ≥ 8 형식에서만(Renderer 7.1절, 결정 RN3·RN4), VS layer·viewport 출력 퍼뮤테이션은 `features`에 `vertexViewportLayerOutput`이 있는 형식에서만(7.2절 7항). 기능 등급(Mobile·SM5·SM6, 머티리얼 시스템 MSR9)이 형식 정의에 들어가면 형식 사실에 함께 넣는다(자리만 둔다). ShaderCore가 `FormatId` → 형식 사실 조회를 제공한다. 쿠커는 형식 정의 파일에서, 런타임은 라이브러리 머리에 담긴 같은 값에서 채운다. 장치 능력(`DeviceCaps`)은 거르기 입력이 아니다(런타임 확인 기능은 셰이더 집합이 확인한다, 7.2절 7항).
- **퍼뮤테이션.** 차원(bool·정수 범위·열거)을 선언하고 차원 값의 조합에서 퍼뮤테이션 ID를 계산한다. 각 차원은 전처리 정의 하나로 원문에 들어간다(UE `SHADER_PERMUTATION_BOOL`·`TShaderPermutationDomain`, `ShaderPermutation.h:272,629`). 형식과 퍼뮤테이션마다 위 거르기로 컴파일 여부를 정한다(UE `ShouldCompilePermutation`, `GlobalShader.h:100–102`). 머티리얼이 정하는 값(마스크·WPO·PDO)은 차원이 아니라 `MaterialShaderTraits`로 거른다(Renderer 14.3절).
- **퍼뮤테이션 수 보고(Renderer RQ13).** 거르기 결과를 열거하는 결정적 함수를 둔다: 셰이더 타입마다 (정점 입력 타입, 퍼뮤테이션, 형식) 조합 가운데 통과한 수와 그 단계 수. 엔진 셰이더는 `shader-compile --engine`(12.3절)이, 머티리얼 셰이더는 쿠커가 머티리얼마다 이 값으로 축별 수·job 수·이전 쿠킹 대비 증감을 보고한다(머티리얼 시스템 8.5절, 계획 14.1절 "증가량을 cooker가 보고"). 입력 표는 Renderer 14.3절 pass 셰이더·퍼뮤테이션 차원이다.
- **라이브러리 형식.** 머리(형식 버전, 형식 id, 형식 정의 SHA-256, 거르기가 읽는 형식 사실(`limits`·`features`), 항목 수), 항목 표(셰이더 타입 이름 hash·정점 입력 타입 이름 hash(없음은 0)·퍼뮤테이션 ID·단계 → 코드 SHA-256, 그 항목의 요구 기능 가운데 런타임 확인 기능 비트(개정 17, RenderBase 9.2절이 장치 셰이더를 만들기 전에 `DeviceCaps`와 대조한다)), 코드 표(코드 SHA-256 → 오프셋·크기, 같은 코드는 한 번만), 코드 부분(바이트코드·바인딩 layout·GLES 바인딩 머리). UE `FSerializedShaderArchive`(셰이더 맵 hash 표·셰이더 hash 표·항목 표, `Runtime/RenderCore/Public/ShaderCodeArchive.h:206–230`)에 해당한다. 머티리얼 셰이더 집합의 묶음 id 열(RenderBase SX5, 머티리얼 시스템 MSR2)과 항목별 레이아웃 서명(SX6)은 이 개정에서 넣지 않았고 라이브러리 형식 버전 1 확정 전에 따로 넣는다.
- **런타임 조회.** (셰이더 타입, 정점 입력 타입, 퍼뮤테이션 ID, 단계)로 찾는다(RenderBase SX4. UE 셰이더 키도 vertex factory를 별도 축으로 둔다, `Runtime/RenderCore/Public/Shader.h:644–671`). 등록된 타입이 요구하는 조합(위 결합 거르기를 통과한 것)이 라이브러리에 없으면 `IncompatibleCook`이다. 머티리얼 셰이더의 예외(기본 머티리얼 대체)는 머티리얼 시스템 MSR10이다.
- 완료 기준: 퍼뮤테이션 ID의 결정성(등록 순서와 무관), 같은 코드의 중복 제거, 라이브러리 왕복·손상 검출(`CorruptEntry`), 빠진 항목 조회의 `IncompatibleCook`. 개정 16에서 더한 것: 정점 입력 타입 축 왕복(같은 셰이더 타입·퍼뮤테이션의 정점 입력 타입별 항목), 결합 거르기 음성 대조(정점 입력 타입이 거른 조합·요구 기능이 없는 형식의 조합이 라이브러리에 없고 조회도 요구하지 않음), 쿠커와 런타임의 거르기 결과 일치(같은 형식 사실), 형식 사실(`renderTargets`)을 바꾼 형식의 열거 변화, 수 보고 값 = 열거 결과.

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

완료 기준: 프로필 스키마 검증(모르는 속성 거부, 프로필에 형식 옵션을 적으면 오류, backend·형식 목록 검사), 장치 기능에 따른 형식 선택, 형식 상한 초과가 그 형식 job만의 쿠킹 오류인지, 매니페스트 v2 왕복, 형식이 빠진 쿠킹을 연 런타임의 `IncompatibleCook`, 프로젝트 없이 엔진 내장 라이브러리를 여는 시험.

## 13. P7 MetaCompiler 셰이더 파라미터

15절 결정 4(2026-11-18 확정, MetaCompiler 방식)의 내용이다. 바탕인 A0-B(B1–B8)는 구현되었다.

- **선언.** `ROCKS_SHADER_PARAMETERS(Name)` 구조체 종류를 MetaCompiler에 더한다. 파서·descriptor는 B4 `ROCKS_STRUCT`([리플렉션 기반](ReflectionFoundations.ko.md) 4절)를 바탕으로 한다. 멤버는 ShaderCore 셰이더 전용 정렬 타입(7.2절 5항)과 셰이더 자원 값 타입(텍스처·샘플러·버퍼 SRV·UAV)만 받고, 다른 타입(Core `Matrix4`·`Vector3` 포함)은 MetaCompiler 오류다.
- **identity와 등록.** 공통 규칙대로 셰이더 파라미터 구조체도 identity 명세(`<Module>.rocksids.json`)에 항목을 두고, `ROCKS_STRUCT`와 같은 경로로 `TypeSchema`·TypeRegistry에 등록한다. 셰이더 파라미터 종류는 그 위에 허용 타입 제약과 HLSL 생성만 더한다. 셰이더 자원 멤버를 담을 리플렉션 값 종류는 `ValueKind`에 더한다. `ValueKind`는 닫힌 열거라 값을 덧붙이고 TypeRegistry 경계를 올리는 방식이다(ReflectionFoundations 3절).
- **생성물.**
  - C++: 멤버별 offset·크기 static_assert, `ShaderBindingLayout`과 대조할 레이아웃 표.
  - HLSL: `<out>/Generated/<Module>/Shaders/<Name>.rsh`(기존 `<out>/Generated/<Module>` 규칙). 이름 있는 `cbuffer`로 낸다(7.2절 1항). 가상 경로 `/Engine/Generated/<Module>/<Name>.rsh`(플러그인·프로젝트는 각자의 `Generated`)로 연결하며, UE `/Engine/Generated/`에 해당한다(`ShaderCore.cpp:2490,3081,3096`). 생성 HLSL은 셰이더 맵 키의 include 폐포에 들어간다. 설치 SDK로 내보내는 경로는 후속이다.
- **레이아웃 서명.** 정규화한 레이아웃(멤버 이름·타입·offset·크기, 자원 종류·순번)의 SHA-256이다. job 키 8항(10절)이다.
- **`ShaderParameterLayout`(데이터 형식).** 위 레이아웃 표를 C++ 타입과 무관한 데이터 형식으로 정한다: 멤버(이름·타입·offset·크기), 자원(종류·순번, 텍스처·샘플러 짝), 서명. MetaCompiler 생성물은 이 형식의 상수 표를 내고, [머티리얼 시스템](MaterialSystem.ko.md)의 머티리얼 컴파일러는 C++ 구조체 없이 같은 형식의 **동적 layout**을 실행 중 데이터로 낸다(결정 MD3, 요구 MSR4). 워커의 리플렉션 대조(7.2절 5항), 바인딩 지도([렌더 기반](RenderBase.ko.md) 9.3절, 머티리얼 요구 MRB2), GLES 복사 표가 두 출처를 같은 경로 하나로 다룬다. UE도 머티리얼 uniform buffer는 매크로가 아니라 실행 중 layout으로 만든다(`Runtime/Engine/Private/Materials/MaterialUniformExpressions.cpp:524`). 형식 정의는 P7 착수 때 확정하고, 머티리얼 단계 MS2 전이어야 한다.
- 그리기별 상수(7.2절 2항 루트 상수)도 이 종류로 선언하고 종류 표시 "그리기별 상수"를 붙인다(RenderBase 결정 R7). 표시가 있는 구조체는 `ROCKS_ROOT_CONSTANTS`로, 없는 구조체는 이름 있는 cbuffer로 생성하며, 표시가 있는데 128 byte를 넘거나 16 byte 배수가 아니면 MetaCompiler 오류다.

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
| ReflectionFoundations | 3절 값 종류, 4절 struct | — | 셰이더 파라미터 종류가 `ROCKS_STRUCT` 경로를 쓰고 셰이더 자원 값 종류를 더한다(13절, 결정 4) |
| PlatformLayer | P1 표 "동적 모듈", 모바일 절 | 동적 모듈 구현(P10, "동적 라이브러리" 절), Android는 시험 실행만 | P13 Android 실행 기반, P12 macOS 창(PlatformWindow) |
| RepositoryLayout | 트리(:14·:29)·:52–56 | Engine에 Shaders·Config 없음, ThirdParty는 Core만 원본(:14 목록도 낡음) | 3절 구조와 8절 루트·`binary`·`headers` 종류 |
| CompilerDiagnostics | :9·:25·:29·:36 | 생성 파일도 경고 검사, 외부 원본도 같은 경고 프로필로 컴파일, Core 루트만, "이 절 밖의 경고 억제 예외는 없다" | 15절 결정 7의 답에 따라: 두 루트, Rocks가 컴파일하지 않는 미리 빌드된 바이너리(오프라인 도구 빌드 포함) 승인 절과 그 공개 헤더의 경고 0 조건, 셰이더 경고 절(작성자 원문과 SPIRV-Cross가 다시 쓴 중간 원문 모두 경고를 오류로) |
| Compression | 3.1·3.3·3.5 | Core 루트, `.c` 번역 단위만 | Engine 루트와 `kind: binary`·`headers` 언급(규칙 자체는 그대로) |
| CoreDataFoundations | 1.2절(:20) | ThirdParty `kind`는 `code`·`data` | `binary`·`headers`를 더한다(8절) |
| 정책 검사(코드) | `PolicyChecks.cs` `PythonFreeMainPath`(:313–349) | `Programs`·`Build`·`.github`의 Python 호출 금지 | 오프라인 도구 빌드 workflow를 명시 제외(8절 4항, P3 구현 때) |
| 코드(SH2 때) | `Engine/Source/Runtime/Graphics/Graphics.Build.cs:4–5`, `Programs/Sandbox/Sandbox.Build.cs:4·14`, `D3D12Graphics.Build.cs:21–22`, `DeviceTypes.h:14–15` | OpenGL·Vulkan·Metal 자리 설명, `d3dcompiler` 링크 | VulkanGraphics 의존 추가, OpenGL ES 설명, `d3dcompiler` 제거 |

## 15. 결정 필요 사항

2026-11-18 결정 4 확정으로 이 절의 결정은 모두 정해졌다. 같은 날 개정 16에서 더한 결정 12도 권장안으로 정했다. 아래는 각 결정의 기한(이 결정이 없으면 시작할 수 없는 작업 직전) 기록이다: 결정 7·11은 셰이더 구현 착수(A6 완료) 전, 결정 6은 SH1 전(측정 없이 정하고, 자체 구현의 정합성은 SH1 완료 기준인 S7로 확인한다), 결정 2·3·5는 형식 정의에 들어가므로 SH0 측정 S2·S9 뒤 P6a 전, 결정 4는 P14 전이었다. 이 문서의 "확정"은 측정에 기대지 않는 내용의 승인이며, 측정 뒤에 정하는 결정과 그에 걸린 14절 개정은 해당 기한에 한다.

1. **확장자** — **결정됨(2026-11-13): `.rsf`(엔트리)·`.rsh`(include).** UE `.usf`/`.ush`의 역할 분리를 따르고 Rocks 명명(`.rasset`·`.rpkg`·`.rnug`)에 맞춘다. C++·HLSL 공용 헤더는 `.h`다(3절). 고려했던 대안은 `.hlsl`/`.hlsli`였다.
2. **HLSL 2021 범위** — **결정됨(2026-11-17): 형식 정의 기본값 + 셰이더 타입 선택.** `D3D12_SM6`·`VULKAN_SM5`·`METAL_SM5`는 켜고 `D3D11_SM5`는 끈다. `D3D11_SM5`에서 2021 셰이더는 SPIRV-Cross 경로로 간다. GLES 형식은 UE `OPENGL_ES3_1_ANDROID`(키 없음)처럼 끈다(형식 정의 `language.hlsl2021`). 결정 전 기록: UE는 플랫폼 값 `EnablesHLSL2021ByDefault`(0 끔, 1 Global 셰이더 전부, 2 모든 셰이더, `Runtime/RHI/Public/DataDrivenShaderPlatformInfo.h:119`)에 레이 트레이싱 셰이더 강제(`Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp:3489–3498`)와 타입별 선택을 더한다. PCD3D_SM6·VULKAN_SM5·VULKAN_SM6·VULKAN_SM5_ANDROID·METAL_SM5·METAL_SM6·METAL_SM6_IOS는 1, PCD3D_SM5는 0(키 없음)이다(`Engine/Config/Windows/DataDrivenPlatformInfo.ini:130`, `Engine/Config/VulkanPC/DataDrivenPlatformInfo.ini:131·257`, `Engine/Config/Android/DataDrivenPlatformInfo.ini:128`, `Engine/Config/Mac/DataDrivenPlatformInfo.ini:53·94`, `Engine/Config/IOS/DataDrivenPlatformInfo.ini:106`). 권장: 형식 정의의 기본값 + 셰이더 타입 선택. `D3D11_SM5`에서 2021 셰이더는 SPIRV-Cross 경로를 탄다(`D3DShaderCompiler.cpp:807,873–890`).
3. **D3D12 셰이더 모델** — **결정됨(2026-11-17): SM 6.6**(UE `PCD3D_SM6`과 같다). Windows 10에서는 Agility SDK(`D3D12Core.dll`)를 `redist` role로 재배포하며, 그에 필요한 런타임 의존 파일 복사는 P6b 또는 SH2에서 한다(9절). 결정 전 기록: 6.6(UE와 같음, `ShaderFormatD3D.cpp:107–110,139–142`) 또는 6.0~6.2 + bindless 프로필 6.6. 6.6은 Windows 10에서 Agility SDK가 Runtime 의존이 된다(9절). SH0에서 개발 기계 GPU·WARP 지원과 Windows 10 요건을 실측한 뒤 정한다(측정 S2). 측정 결과(S2, 2026-11-17): 이 기계(Windows 11 build 26200)의 GTX 1060 3GB와 WARP가 Agility SDK 없이 최대 SM 6.8, resource binding tier 3이다. Windows 10의 OS 런타임 지원은 Windows 10 기계가 없어 미측정이다.
4. **파라미터 선언** — **결정됨(2026-11-18, 권장안): MetaCompiler 셰이더 파라미터 종류(13절).** 사용자 지시 "결정사항들은 권장안대로 하면됨". 리플렉션이 단일 출처이고 생성 HLSL을 디스크에 둔다. 레이아웃 표는 `ShaderParameterLayout` 데이터 형식으로 정해, C++ 구조체 없이 만드는 머티리얼 동적 layout도 같은 형식을 낸다(13절, [머티리얼 시스템](MaterialSystem.ko.md) 결정 MD3·MSR4). 결정 전 기록: MetaCompiler 종류(권장) 또는 UE 매크로 방식(`SHADER_PARAMETER_*`, `Runtime/RenderCore/Public/ShaderParameterMacros.h`). 매크로 방식은 매크로 메타데이터와 머티리얼 동적 layout이 두 형식이 되어 변환층이 필요하다. UE도 머티리얼 uniform buffer는 매크로가 아니라 실행 중 layout으로 만든다(`Runtime/Engine/Private/Materials/MaterialUniformExpressions.cpp:524`).
5. **형식별 생산 호스트** — **결정됨(2026-11-17): metallib은 Windows에서도 만들 수 있다.** Apple Metal Developer Tools가 있으면 만들고 없으면 `UnsupportedCapability`다. 형식 정의의 생산 호스트 규칙(`producers`의 `systemTools`)으로 둔다. 엔진 컴파일 프로필 `Engine-Windows`에는 Windows가 쓰지도 빌드하지도 않는 `METAL_SM5`를 넣지 않는다(12.1절). 결정 전 기록: 키가 호스트별이므로(10절) DXIL·SPIR-V·ESSL은 각 호스트가 직접 만든다. 한 호스트만 만들 수 있는 형식: DXBC는 Windows. metallib은 macOS(Xcode Metal 도구)를 기준으로 하고, Windows에도 Apple Metal Developer Tools(UE 5.8이 지원, 기대 버전 5.3, `MetalShaderFormat.cpp:724–725,1030–1137`)가 있으면 만들게 할지 정한다(권장: 허용, 형식 정의의 생산 호스트 규칙, 측정 S9). 만들 수 없는 호스트에서는 `UnsupportedCapability`이고, 원격 워커가 생기면 그쪽으로 보낸다.
6. **전처리기** — **결정됨(2026-11-17): Rocks 자체 구현을 `Developer/ShaderCompiler`에 둔다.** 정합성은 측정 S7로 SH1에서 확인한다. 결정 전 기록: Rocks 자체 구현을 `Developer/ShaderCompiler`에 둔다(권장, UE처럼 메인 프로세스에서 실행. UE 전처리기 stb_preprocess는 Epic 저작권 원본이라 가져올 수 없다, `Developer/ShaderPreprocessor/Private/stb_preprocess/preprocessor.h:1`) 또는 라이선스를 확인한 외부 C 전처리기를 P3 C 원본 규칙으로 들인다. DXC `-P`는 메인 프로세스가 DXC를 열게 되어(9절 규칙 위반) 쓰지 않는다. 자체 구현의 정합성은 측정 S7로 확인한다.
7. **경고 정책** — **결정됨(2026-11-16): 아래 (1)·(2)의 권장안.** CompilerDiagnostics에 "Rocks 빌드가 컴파일하지 않는 미리 빌드한 바이너리" 승인 절을 더하고(P3 구현 때 14절 CompilerDiagnostics 행대로 개정), 셰이더 컴파일 경고는 작성자 원문과 SPIRV-Cross가 다시 쓴 중간 원문 모두 오류로 한다. 결정 전 기록: CompilerDiagnostics는 "외부 원본도 엔진과 같은 경고 프로필로 컴파일한다"(:25), "생성 파일이라는 이유로 경고 검사를 생략하지 않는다"(:9), "이 절 밖의 경고 억제 예외는 없다"(:36)고 한다.
   - (1) 오프라인 도구 빌드: 결정 8(UE 방식)로 DXC·SPIRV-Cross·SPIRV-Tools는 업스트림 빌드 설정(예외·RTTI 사용, 업스트림 경고 설정)으로 엔진 빌드 밖에서 컴파일한다. DXC(LLVM)는 예외 없이·Rocks 경고 프로필로 빌드할 수 없다. 권장: CompilerDiagnostics에 "Rocks 빌드가 컴파일하지 않는 미리 빌드된 바이너리" 승인 절을 더한다(조건: 원본 태그·patch·빌드 스크립트 고정, Rocks가 include하는 공개 헤더는 경고 0, 엔진 컴파일 인자에 억제를 더하지 않음).
   - (2) 셰이더 컴파일 경고: 권장은 공통 규칙대로 작성자 원문과 SPIRV-Cross가 다시 쓴 중간 HLSL 모두 경고를 오류로 하는 것이다. 중간 원문 경고는 SPIRV-Cross 옵션이나 추적 가능한 patch(8절 4항)로 없앤다. UE는 작성자 원문에만 `-WX`를 걸고(`Developer/ShaderCompilerCommon/Private/ShaderConductorContext.cpp:425–429`), SPIRV-Cross 출력을 FXC로 컴파일할 때는 경고를 오류로 하지 않는다(`D3DShaderCompiler.cpp:959–961`).
8. **도구 공급 방식** — **결정됨(2026-11-13): UE 방식.** DXC·SPIRV-Cross·SPIRV-Tools를 엔진 빌드 밖 스크립트로 직접 빌드해 바이너리로 배포하고, 엔진은 받은 바이너리를 쓰기만 한다(8절 4항). 조사 근거: upstream DXC는 macOS 빌드를 공식 CI로 돌리고(업스트림 `azure-pipelines.yml`), SPIRV-Cross는 C API 공유 라이브러리를 CMake 옵션으로 만든다. 고려했던 대안은 LunarG Vulkan SDK 바이너리였다(SPIRV-Cross C API 공유 라이브러리 포함 여부 미확인, 설치기라 다시 묶어야 함, 패치 불가).
9. **SH2 범위** — **결정됨(2026-11-13), 2026-11-14 개정.** Graphics API를 컴파일된 코드로 바꾸는 변경이 D3D12·Vulkan self-test를 함께 통과할 때 들어간다. 처음 결정은 같은 변경에서 OpenGL을 지우는 것이었으나, 2026-11-14 OpenGL ES 필수·UE 방식 결정에 따라 OpenGLGraphics는 지우지 않고 같은 변경에서 GLES 미리보기 형식을 받게 바꾼다(UE `OpenGLDrv`가 Windows 미리보기와 Android를 함께 맡는 것과 같다). self-test는 D3D12·Vulkan·GLES 미리보기를 대조한다.
10. **셰이더 키의 툴체인 지문** — **결정됨(2026-11-13): 공통 규칙 준수.** 셰이더 job 키에도 AssetSystem 9절 공통 항목(툴체인 지문·결정적 수학 버전·대상 프로파일 id와 출력 설정 전체 포함)을 그대로 넣는다(10절). UE 5.8은 셰이더 job 키에 엔진을 빌드한 컴파일러·호스트를 넣지 않지만(`ShaderCore.cpp:4612–4671`), Rocks는 공통 규칙을 우선한다. 비용은 호스트 사이 셰이더 너겟 비공유와, 프로파일 출력 설정이 바뀔 때의 재컴파일이다.
11. **셰이더 구현 착수 위치** — **결정됨(2026-11-16): (나).** 사용자 결정 "쿠킹 셰이더 위에서(정공법)", "원래 목표는 미니멀하게 테스트하고자 했던건데 이제 의미없음. 렌더코어 제대로 구현하는게 나음". 화면 마일스톤은 셰이더 구현(SH0–SH3)과 렌더 기반을 함께 세우는 단계이며, 범위는 [렌더 기반](RenderBase.ko.md) 18.1절이다. 아래는 결정 전 선택지 기록이다. 고정 순서(A0-B → A0-O → A0-G → A1–A6 → 화면 마일스톤) 안에서 셰이더 구현(6절 4항)을 어디에 둘지다. 셰이더는 A1–A4가 필요하고 고정 순서가 A1–A6을 한 묶음으로 두므로 A6 뒤다.
    - (가) 화면 마일스톤 뒤(권장): 고정 순서를 그대로 둔다. 화면 마일스톤은 지금의 backend별 원문 경로로 그리고, SH2가 Renderer 내장 셰이더와 Graphics 파이프라인 생성을 바꾼다. 화면 마일스톤의 범위는 아직 정의되어 있지 않으므로(4절) 이 권장은 화면 마일스톤이 내장 셰이더 몇 개로 그리고 material(셰이더 그래프)을 들이지 않는다는 조건에서다. (가)를 고르면 5.2절 G2(ES 공통 API) 규칙을 화면 마일스톤의 OpenGL 작업까지 넓히고, 화면 마일스톤의 새 셰이더도 7.2절 규칙(짝 규칙·허용 타입·`ATTRIBUTEn`)대로 쓴다.
    - (나) 화면 마일스톤 본체 앞: 화면 마일스톤을 처음부터 쿠킹된 셰이더와 최종 바인딩 규약 위에 쌓는다. 화면 마일스톤이 P2·P3·P4·P5·P6a·P6b·P9–P11·P14·SH0–SH2만큼 늦어진다. 화면 마일스톤이 material이나 많은 셰이더를 들인다면 이쪽이 낫다.
12. **선택 기능의 형식 id 규칙** — **결정됨(2026-11-18, 권장안).** Renderer RQ14(VS `SV_RenderTargetArrayIndex`·`SV_ViewportArrayIndex`)에서 나온 결정이다. (가) 권장: 형식 정의 `features`는 "이 형식이 그 기능을 쓰는 셰이더를 컴파일한다"는 뜻이고, 기능 어휘마다 장치 보장 기능(형식의 `deviceRequirements`가 보장, `compute`·`waveOperations`)과 런타임 확인 기능(`vertexViewportLayerOutput`)을 나눈다. 런타임 확인 기능은 형식 선택에서 장치를 거르지 않고, 셰이더 집합이 `DeviceCaps`로 장치 셰이더 생성을 막으며 호출자가 대체 경로를 고른다. 형식 id는 나누지 않는다(7.2절 7항). UE 조건의 두 범주 가운데 "정적 표시"는 장치 보장 기능, "실행 때 감지 + 장치 값"은 런타임 확인 기능에 대응한다(`Runtime/RenderCore/Private/RenderUtils.cpp:1672–1678`). (나) 기능마다 형식 id를 따로 둔다(예: layer 출력 형식). 장치를 확실히 가르지만 형식·쿠킹 산출물이 기능 조합만큼 늘고, 대체 경로가 있는 기능에는 이득이 없다. (다) 기능을 형식의 장치 요구로 넣는다. 그 기능이 없는 장치가 형식 전체를 못 쓴다. 기한: SH2(형식 정의 읽기 코드에 어휘 구분을 넣을 때).

## 16. 이후 단계(선행 작업 뒤)

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| SH0 | 도구 스파이크: 지금의 내장 샘플 셰이더(`Programs/Sandbox/Private/SampleRenderer.cpp`의 HLSL)를 DXIL·SPIR-V·DXBC·MSL·ESSL로 만들고 검증 도구를 돌린다. Windows·macOS 양쪽에서 실행 | P3·P4·P10 | 형식별 산출, `spirv-val` 통과, DXIL 서명 hash 검사(macOS DXC 포함, S1), FXC 고정본 로드, 리플렉션 덤프, `ROCKS_IMAGE_FORMAT`의 DXC 지원 확인, S2·S3·S6·S8·S9·S10·S12(데스크톱)·S13·S14 측정. 남은 것(2026-11-20): macOS 부분(S1·S3 호스트 사이·S9)과 기계 하나로 못 재는 S2·S3·S12 일부(5.4절 끝) |
| SH1 | 소스 트리·가상 경로·`Platform.rsh`·전처리기(결정 6)·ShaderCompileWorker(P1, 11절)·두 층 키(P5)·에셋 너겟·`shader-compile` 명령·Development 필요 시 컴파일. 설계·하위 단계 SH1a–SH1e: 11.1절(개정 23) | SH0·P1·P5(A2·A3·P6a)·결정 6 | 같은 입력 같은 바이트, 빈/찬 캐시 동일, 워커 강제 종료 보고, 부모 종료 시 워커와 그 자식 종료, 형식 버전 불일치 진단, 도구 조회, 전처리 정합성(측정 S7) |
| SH2 | 리플렉션 정규화·워커 규칙 검사(7.2절)·Graphics API(`ShaderCodeKind`·`CreateShader`·파이프라인 layout·root signature·래스터(컬링·앞면)와 깊이 상태)·셰이더 타입과 퍼뮤테이션(P14)·쿠킹된 라이브러리 로드(P6b)·VulkanGraphics 최소 구현(offscreen·내장 셰이더, P9)·OpenGLGraphics GLES 미리보기 전환(P11)·샘플 렌더러 원문·런타임 `D3DCompile`·GLSL 원문 제거(결정 9) | SH1·P2·P4·P6b·P9·P11·P14·A0-G·결정 2·3·4·5 | Sandbox self-test에서 D3D12·Vulkan·GLES 미리보기가 허용 오차 안 일치(Vulkan·GL은 CI에서 NOT RUN일 수 있다), A0-G 범위를 Vulkan이 받음(G3), 행렬 옵션·앞면 매핑 기록(S5), Release에 컴파일러 없음(P4), 7.3절 SH2 시험 |
| SH3 | 파라미터 구조체(P7)·생성 HLSL·오프셋 시험 | SH2·P7 | 7.3절 (a)·(f)를 생성 구조체로 |
| SH4 | 감시 재컴파일(`DirectoryWatch`)·오류 위치·디버그 심볼·PSO 교체 | SH2 | 저장 후 10초 안 반영(계획 I5) |
| SH5 | D3D11Graphics → MetalGraphics | SH2·P12·P15 | 각 backend가 D3D12와 허용 오차 안 일치, 7.3절 (c)·(e)·(g)의 D3D11·Metal 부분 |
| SH6 | Android: `GLES31_ANDROID` 기기 실행, `VULKAN_ANDROID` 형식과 VulkanGraphics Android | SH2·P13·P11 | Android(실기기 또는 S11 에뮬레이터)에서 D3D12 기준 이미지와 허용 오차 안 일치, 7.3절 (c)·(i)의 Android 부분, S12(기기) |

SH0 진척(2026-11-17, `feature/shader-tools`, Windows 부분 구현):
- [x] 스파이크 `Programs/ShaderToolSpike`(Program 타깃, `ShaderToolSpike --out <디렉터리> [--sm 6_0]`). 도구는 P4 `ShaderToolSet`으로 사용자 저장소에서 연다. 산출물은 `<out>` 아래에 두고 저장소에는 이 요약만 남긴다. `manifest.json`(산출물 이름·크기·SHA-256, 결정성 비교용)과 `report.json`(검사·측정값)을 쓴다.
- [x] 형식별 산출(내장 Renderer VS·PS): DXIL `vs_6_0`·`ps_6_0`(`-Qstrip_reflect`), SPIR-V(`-spirv -fspv-target-env=vulkan1.1 -fvk-use-dx-position-w`), DXBC(FXC 고정본 `vs_5_0`·`ps_5_0`), MSL 2.1 macOS, ESSL `#version 320 es`의 `GLES31_ANDROID`(`fixup_clipspace`·`flip_vert_y` 켬)와 `GLES31_PC`(끔), HLSL 5.0. HLSL 5.0 출력은 FXC로 다시 컴파일된다(D3D11 대체 경로 확인).
- [x] 검증: `spirv-val`(vulkan1.1) 통과, DXIL container digest 검사 통과(0·preview 우회 값이면 실패 처리), 리플렉션 덤프(DXIL `ID3D12ShaderReflection`, SPIR-V SPIRV-Reflect: 바인딩·상수 버퍼 오프셋·입출력 시맨틱과 location).
- [x] `ROCKS_IMAGE_FORMAT`: DXC 1.9.2609가 `[[vk::image_format]]`을 지원한다. `rgba8`·`rgba8snorm`·`rgba16f`·`r11g11b10f`·`r32f`·`r32ui`·`unknown`이 SPIR-V 형식 그대로 나오고, 지정이 없으면 `float4`에서 `Rgba32f`로 추론한다. DXIL은 attribute를 무시하며 `-Wignored-attributes` 경고를 낸다. ESSL 320은 `r11g11b10f`를 받지 않고(SPIRV-Cross 오류 "image format not supported in ES profile"), `unknown`은 형식 한정자 없는 image를 낸다.
- [x] SPIRV-Cross 실수 출력(10절): 극값 fixture 8개(최소·최대 비정규수, 최소 정규수, 최대 유한값, 0.1, 1/3, 1+ulp, 음의 비정규수)의 ESSL·MSL 철자가 C locale에서 독립 계산한 `%.32g`(정확한 반올림)와 같고 원래 bit로 다시 읽힌다. 쉼표 소수점 locale(`de-DE`)에서도 출력 byte가 같다(`fixup_radix_point`). Windows UCRT 기준이며 다른 CRT(macOS)는 미측정이다.
- [x] 측정 S2·S3(Windows 부분)·S6·S8·S12(데스크톱): 5.4절 결과 열.
- 규약 검토 자료(리플렉션 덤프에서 확인한 것, 결정 아님):
  - SPIR-V에서 원문의 `register(t0)`·`register(s0)`가 같은 set 0·binding 0이 된다(texture와 sampler가 한 binding을 나눠 Vulkan descriptor layout으로 쓸 수 없다). 7.2절 4항 워커 재배치가 필요하다는 근거다. DXIL은 종류별 register 공간이라 겹치지 않는다.
  - 상수 버퍼 `DrawConstants`는 DXIL·SPIR-V 모두 80 byte, `transform` 0·`color` 64다. SPIRV-Cross ESSL은 `layout(row_major) mat4`와 `vec * M`, HLSL 5.0은 `column_major`로 낸다(행렬 옵션은 S5, 7.2절 6항). 이 측정은 2026-11-17 행 벡터 전환 전의 열 우선 원문으로 했다.
  - SPIR-V 입출력은 location 0부터 선언 순서이고, DXC는 `-fspv-reflect` 없이는 시맨틱 decoration을 내지 않아 시맨틱은 변수 이름(`in.var.TEXCOORD0`)에만 남는다. SPIRV-Cross HLSL 5.0은 시맨틱을 location 기준 `TEXCOORDn`으로 다시 쓴다(`COLOR0` → `TEXCOORD1`).
  - ESSL의 결합 sampler는 이름이 `_29`처럼 id이고 binding이 없다. 7.2절 GLES binding 재매김과 이름 규칙을 워커가 정해야 한다.
- [x] SH0 남은 측정(2026-11-20, `feature/sh0-rest`): S10(문서 조사, 8절 2항), S13(`TypedBuffer.ps`·`.cs`, 7.2절 4항), S14(`LayerViewport.vs`, 7.2절 7항). 스파이크 산출물은 68개이고 반복 실행의 manifest가 같다. 측정을 위해 P4 SPIRV-Cross 함수 표에 `spvc_compiler_get_combined_image_samplers`·`spvc_compiler_get_name`을 더했고, 스파이크 MSL 출력은 `texture_buffer_native`를 켠다(7.2절 4항 Metal).
  - 규약 검토 자료(결정 아님): `SV_InstanceID`를 쓰는 VS의 ESSL에는 ES에 draw parameters가 없어 SPIRV-Cross가 `uniform int SPIRV_Cross_BaseInstance`(기본값 0)를 더한다. base instance가 늘 0이라(7.2절 7항) 값은 맞고, SH2에서 워커가 SPIRV-Cross `support_nonzero_base_instance`를 꺼 이 uniform을 없앨지 정한다.
- [ ] macOS: S1(macOS DXC 서명), S3 Windows–macOS 사이 byte 비교, S9, macOS CRT의 실수 출력. 스파이크 macOS 구현(`Private/Mac`, DXC는 `WinAdapter.h`)은 macOS 산출물(P3)과 함께 한다.
- [ ] S3의 FMA3 유무·OS 버전 차이와 S12의 다른 GPU 공급자는 기계가 하나라 미측정이다.

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
- 개정 8(2026-11-14): GLES 기기 요구(ES 3.2 필수)의 근거를 2절에 기록했다(사용자 요청). Android 배포 대시보드의 OpenGL ES·Vulkan 분포(2025-11-24까지), Android의 Vulkan 요구 조건, ES 3.0·3.1 기기 세대(GPU별은 미확인), Google의 OpenGL ES·ANGLE 방향, ES 3.0·3.1을 넣지 않는 이유.
- 개정 9(2026-11-17): 사용자 결정(좌표계·단위·행렬 규약을 UE 5.8과 같게)에 맞춰 7.2절을 고쳤다.
  - 6항: 행 벡터·행 우선 규약으로 바꾸고 형식별 행 우선 옵션의 UE 근거를 확인했다. S5는 옵션을 고르는 시험에서 규약을 확인하는 시험으로 바꿨다.
  - 8항: 앞면 규약(화면 반시계·뒤면 컬), 음수 행렬식 반전, backend 매핑 근거를 더했다.
  - 근거: 좌표 전환 세션의 차이표(UE 5.8.4 소스·probe)와 이 문서의 UE 소스 재확인.
- 개정 10(2026-11-17): 좌표 전환 C7(렌더 코드 반영)을 구현 상태로 적었다.
  - 6항: 런타임 D3D12는 `D3DCOMPILE_PACK_MATRIX_ROW_MAJOR`, OpenGL은 `transpose = GL_TRUE`로 행렬을 행 우선으로 담는다. 손으로 쓴 GLSL에 `layout(row_major)`를 쓰지 않은 이유(기본 블록 uniform)도 적었다.
  - 8항: Graphics `CullMode`와 기본값 None, backend 매핑, 샘플 렌더러의 음수 행렬식 반전을 적었다.
  - S5: D3D12·WARP·OpenGL 런타임 경로 결과를 적었다.
- 개정 11(2026-11-17): P11을 구현 상태로 적었다([OpenGL ES backend](OpenGLESGraphics.ko.md)).
  - 5.1·5.3: P11 상태와 달성. Windows 미리보기는 ES 3.2 profile 우선, GL 4.3 core 대체(스펙의 "GL 4.3 core 이상"과 다른 점).
  - 6항: OpenGL의 행렬 업로드를 `transpose = GL_TRUE`에서 루트 상수 `vec4` 배열(쿠킹된 ESSL과 같은 방식)로 바꿨다.
  - 8항: 창 `GL_LOWER_LEFT` 규칙이 `GL_UPPER_LEFT` + offscreen 스왑체인 + Y 반전 blit으로 바뀌었다(SH2에서 P11로 앞당김). 앞면은 클립 공간 규약별(`GLES31_PC` `GL_CCW`, `GLES31_ANDROID` `GL_CW`).
  - S5·S12: GLES 미리보기 결과와 ES profile 측정을 더했다.
- 개정 12(2026-11-18): 결정 4(파라미터 선언)를 권장안 MetaCompiler 방식으로 확정했다(사용자 지시 "결정사항들은 권장안대로 하면됨"). 15절 결정 4와 기한 문장, 서문의 남은 결정, 5.3절 P7 제목, 13절 서두, 14절 ReflectionFoundations 행의 "고를 때" 조건을 정리했다. 13절에 `ShaderParameterLayout` 데이터 형식 요구를 더해 머티리얼 동적 layout(머티리얼 시스템 결정 MD3·MSR4)과 연결했다.
- 개정 13(2026-11-18): [렌더러](Renderer.ko.md) RQ22·RQ23과 [머티리얼 시스템](MaterialSystem.ko.md) MSR1·MSR12·MSR13(결정 RN13·MD4 개정, 사용자 결정 "형식별 상한")을 반영했다.
  - 7.2절 4항: SRV 텍스처·샘플러 수는 프로필 공통 상한이 아니라 job 형식 상한으로 검사하고, 오류에 셰이더 타입·퍼뮤테이션·형식·머티리얼·pass를 담는다. 형식 정의 값(D3D12·Metal 128, D3D11·GLES 16, Vulkan은 GX1 뒤)과 Vulkan을 넓힐 때의 set·단계 합계 한도를 적었다.
  - 7.2절 9항: 짝 규칙의 예외로 공유 샘플러 `SharedWrapSampler`·`SharedClampSampler`(UE `SSM_Wrap_WorldGroupSettings`·`SSM_Clamp_WorldGroupSettings`)를 후속에서 SH2로 앞당겼다. 텍스처마다 샘플러 출처 하나, 리플렉션 출처 필드, backend별 위치(GLES는 유닛마다 `glBindSampler`), 상태는 렌더러가 view마다 정한다. 정적 샘플러는 후속으로 남는다.
  - 7.3절 (b)·(h): job 형식 상한과 공유 샘플러 시험을 더했다.
  - 12.1절: `D3D11_SM5` `sampledTextures` 128 → 16(형식 정의 파일도 함께 고쳤다). `VULKAN_SM5` 확장은 GX1 측정 뒤 할 일로 남겼다.
- 개정 14(2026-11-18): GX1 측정으로 `VULKAN_SM5` `sampledTextures` 128·`renderTargets` 8을 정했다(Renderer RQ11·RQ22, 결정 RN3 (가), 권장안 기본 지시). 7.2절 4항에 "상한은 장치 요구다" 규칙(형식 `limits`를 런타임 형식 선택이 `DeviceCaps`와 대조, Vulkan set·`maxPerStageResources` 합산 포함)을 두고 장치 요구 어휘 추가 계획을 없앴다. 12.1절 진척을 고쳤다.
- 개정 15(2026-11-18): Renderer RQ10 반영. 7.2절 4항 GLES에 typed buffer SRV가 `samplerBuffer`로 텍스처 유닛을 차지하고 SRV 텍스처 수·샘플러 16 안에 든다는 규칙, typed buffer UAV는 `imageBuffer`·CS만이라는 규칙을 더했다. 5.4절에 SPIRV-Cross 출력 확인 측정 S13을 더했다.
- 개정 16(2026-11-18): Renderer RQ13·RQ14 반영.
  - RQ14(7.2절 4·7항): `SV_Target0–7`의 상한을 job 형식 `renderTargets`로 검사한다(프로필 공통 상한 4로 검사하면 MRT 8 형식 전용 GBuffer +D·+E가 모든 형식에서 오류가 되는 빈틈을 고쳤다). VS `SV_RenderTargetArrayIndex`를 런타임 확인 기능 `vertexLayerOutput`으로 연다: 형식 `features`에 넣되 장치 요구가 아니고, 셰이더 집합이 `DeviceCaps`로 확인하며, 형식 id를 나누지 않는다(15절 결정 12, 권장안). UE 근거(정적 표시·실행 때 감지·장치 값)를 달았다. 측정 S14와 시험 (j)를 더했다.
  - RQ13(12.2절, RenderBase SX4, 머티리얼 시스템 MSR5와 함께): 머티리얼 셰이더 표지·정점 입력 축 표지, 거르기 서명(`MaterialShaderTraits`·정점 입력 타입·퍼뮤테이션·형식, 머티리얼 시스템 8.3절과 같음)과 AND 결합, 거르기가 읽는 형식 사실 조회(라이브러리 머리에 담아 쿠커·런타임 결과를 같게), 퍼뮤테이션 수 보고, 정점 입력 타입을 넣은 라이브러리 항목 키·런타임 조회 키. 묶음 id 열(SX5·MSR2)과 레이아웃 서명(SX6)은 남겼다.
- 개정 17(2026-11-18): 개정 16 교차 검증 반영.
  - 7.2절 4항: 모든 상한을 job 형식 상한으로 검사한다. 프로필 공통 상한은 셰이더 타입 단위라 거르기 뒤 데스크톱 전용 퍼뮤테이션에도 GLES 상한을 걸었다. 공통 상한을 폐기하고 job 키 9항의 공통 상한 형식 목록, P6b 완료 기준, 시험 (b)를 고쳤다. "상한은 쿠킹 코드의 바인딩 수에 박히므로 늘 장치 요구다" 근거를 적었다. 읽기 코드 정리는 12.1절 진척(SH2).
  - 7.2절 7항: VS 출력 규칙을 layer·viewport 둘로 넓혀 어휘를 `vertexViewportLayerOutput` 하나로 했다. Renderer 결정 RN10 (가)의 2D atlas는 layer가 아니라 viewport 출력이 맞다. D3D12는 이 기능이 늘 된다(UE `D3D12Device.cpp:272`)는 점, ES는 `GL_NV_viewport_array2` 확장만 있고 Rocks·UE가 쓰지 않는다는 점, UE 조건의 두 범주(정적 표시 OR (설정 AND 장치 값))를 고쳐 적었다. Graphics `DeviceCaps` 이름·viewport 수·D3D12 값은 Graphics 쪽 일로 적었다. S14·시험 (j)·결정 12를 맞췄다.
  - 12.2절: 라이브러리 항목에 런타임 확인 기능 비트(RenderBase 9.2절이 대조), 형식 사실에 기능 등급 자리(머티리얼 시스템 MSR9).
- 개정 18(2026-11-18): 12.1절 진척 두 항목(런타임 확인 기능 `vertexViewportLayerOutput`, `CommonLimits` 제거)을 SH2 전에 구현했다.
- 개정 19(2026-11-18): [렌더 기반](RenderBase.ko.md) SX2 반영. 7.2절 2항에 결정 R7 확정(루트 상수 배치는 선언 명시, 128 byte·16 byte 배수, 크기 기준 자동 배치는 버림)과 파이프라인 layout의 크기 선언(Graphics `drawConstantBytes`·`SetDrawConstants`)을 적었다. Graphics 고정 구조체 `DrawConstants`를 없앴으므로 7.2절 5항·6항과 13절의 해당 문구를 고쳤다(13절은 그리기별 상수도 파라미터 구조체 종류 표시로 선언).
- 개정 20(2026-11-19): 5.2절에 텍스처 스트리밍 결정 T6(명시 LOD·`Load` helper와 진단)의 셰이더 요구를 더했다([텍스처 스트리밍](TextureStreaming.ko.md) 개정 4).
- 개정 21(2026-11-20): SH0 남은 측정 S10·S13·S14(Windows) 결과를 5.4절에 적고 규칙에 반영했다. 남은 측정을 5.4절 끝과 16절 SH0 행에 정리했다.
  - S10 → 8절 2항: `d3dcompiler_47.dll`은 SDK REDIST 목록에 있어 고치지 않은 파일을 데스크톱 응용과 함께만 재배포할 수 있다. Runtime에는 넣지 않고, 개발 도구 묶음에는 넣을 수 있으며, DLL만 따로 게시하지 않는다. 고정 버전 10.0.22621.2428은 그대로다.
  - S13 → 7.2절 4항: GLES typed buffer 모양이 예상대로이고 typed buffer SRV는 결합 sampler 목록에 없으므로 워커가 따로 유닛을 매긴다. 읽고 쓰는 ES image는 `r32f`·`r32i`·`r32ui`만이라는 워커 검사를 더했다. Metal typed buffer는 `texture_buffer_native`로 낸다.
  - S14 → 7.2절 7항: 모든 형식의 변환을 확인했고 FXC `vs_5_0`이 받으므로 `D3D11_SM5`에 `vertexViewportLayerOutput`을 넣었다(형식 정의·시험, 12.1절). SPIR-V 확장은 그 기능을 요구한 셰이더에만 허용한다.
- 개정 22(2026-11-21): 8절 2항 FXC의 bootstrap 원본을 버전 구분 없는 `Redist/D3D/x64`에서 버전별 `bin/10.0.22621.0/x64` 사본으로 바꿨다(SDK 10.0.26100 설치가 `Redist` 본을 덮어써 SHA 불일치). 버전별 사본은 REDIST 목록 밖이라 재배포 묶음은 `Redist` 본을 따로 고정한다. 설치 마법사·`.vsconfig`가 SDK 10.0.22621도 설치한다([설치 마법사](SetupWizard.ko.md)).
- 개정 23(2026-11-23): P5를 A3 실제 API에 맞추고 SH1 설계를 더했다(사용자 지시 "에셋 처리 끝나고 남은 것들 이어가자").
  - 10절: job 키를 `NuggetKeyWriter` 기록 순서로 다시 썼다(공통 머리 `WriteNuggetKeyHeader`, 도구는 `WriteNuggetEncoder` 목록, 키를 Record 키로 그대로). 너겟 위치(엔진만 쓸 때 root = 엔진 폴더), Record 값 표(ValueId 0–4), 실패 job 비게시, 일괄 요청과 결과 상태 처리, 완료 기준(세대·처리기·단계·엔트리 무효화, 키 golden, 빈·찬 캐시).
  - 5.1·5.2절: A1–A3 구현 상태, N1–N3 반영 기록.
  - 3절·11절·11.1절: 모듈(`ShaderCompilerCommon`·`ShaderWorker` 추가), 워커 pipe 제어 줄과 관리자 쪽 유휴 종료, SH1 결정 SC1–SC14와 하위 단계 SH1a–SH1e, UE 대응표.
  - 교차 검증 1회(필수 11·권장 16) 반영: 결정 번호를 측정과 구분(SC), 키 golden은 실행 중 지문으로, 전체 Record 조회, 진단을 Record에서 뺌(값 표 0–3), D3D11 대체 경로는 FXC 컴파일 오류만, 입력 머리에 툴체인 지문(`BadToolchain`), system 도구 조회를 키 계산 앞으로, 내장 매크로 조건 거부와 `ROCKS_SHADER_MODEL_*`, 인터페이스 이름 `ShaderFormatBackend`, 워커 채널 스레드(P1 스레드 계약), 옵션은 `ShaderCompileOptions` 리플렉션 기록, 도구 role·Apple 지문 규칙, snapshot byte, 엔진 프로필 한정, `ShaderStageBits`, `WaitForProcessExit`를 PlatformBase로 옮김, 작업 디렉터리 정리, 동반 타깃 구성, 스파이크 삭제 순서, BOM·CRLF, 예약 정의, `Platform.rsh` 강제 include, `Generated` 범위, 출력 이름.
  - 16절: SH1 행에 하위 단계 참조.
- 개정 24(2026-11-23): SH1a 구현 메모(11.1절 — 구조, 상대 include 접기·`Shared` 범위·재정의 오류·`true`·출력 공백과 `#line` 기준·한도 묶음·`ROCKS_SPIRV`, S7 도구 `CompileWithDxc`와 시험 위치, 대소문자 한계)와 측정 S7 결과(5.4절: DXIL·SPIR-V byte 동일, 오류 위치 동일).
- 개정 25(2026-11-23): SH1b 구현 메모(11.1절 — `ShaderCompilerCommon` 분리, 옵션 파서, 키 2–7행 함수와 system 도구 지문 입력, 값 표 도우미, 너겟 위치, 정책 `shader-worker-isolation`).
- 개정 26(2026-11-23): SH1c 구현 메모(11.1절 — 워커 메시지·종료 코드 보충, 워커·형식 모듈, 제어 줄, DXC 인자, 동반 타깃, system 도구 지문 SH1e로).
