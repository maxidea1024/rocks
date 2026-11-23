# 셰이더 시스템 설계 — 2. 형식과 변환 경로

[목차](README.ko.md)

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
