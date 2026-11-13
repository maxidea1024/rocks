# 셰이더 시스템 설계

기준일: 2026-11-13, 개정 6(초안). 셰이더 원문을 HLSL 하나로 쓰고 오프라인에서 D3D12·D3D11·Vulkan·Metal용 코드로 바꾸는 구조와, 그 구현에 앞서 갖춰야 할 **선행 작업**을 정한다. 조사 근거(UE 5.6 backend별 경로·컴파일 파이프라인·외부 도구 현황)는 조사 문서 [Rocks 셰이더 시스템](https://claude.ai/artifact/U5uNh2EWGvXKQiNYXnuc1Q)에 있고, 이 문서는 그 결론과 Rocks 적용 규칙만 담는다. UE 근거는 EpicGames/UnrealEngine `5.6` 브랜치 commit `cdda65cecacd9a1278020925c357bf4cc0b17e8c`의 원본이며 경로는 `Engine/Source/` 기준이다(`Engine/Shaders`·`Engine/Config`·`Engine/Binaries`·`Engine/Build`는 그대로 적는다).

[엔진 구현 계획](EngineImplementationPlan.ko.md)·[엔진 런타임](EngineRuntime.ko.md)과 충돌하는 부분은 이 문서 기준으로 바꾸고 계획 문서도 같이 고친다(11절). 개정은 이 문서가 확정될 때 함께 한다.

상태 표기는 AssetSystem과 같다: **구현**(동작·시험), **계약**(API·형식이 있고 미지원 값은 명시 오류), **후속**(API도 없음). 이 문서 전체는 아직 **설계**이며 P1만 구현되었다. 엔진 기반이 준비될 때까지 이 문서는 스펙으로만 두고 P2 이후는 구현하지 않는다(2026-11-13 사용자 지시). 착수할 때도 12절 결정이 먼저다.

## 1. 결정

| 항목 | 결정 | 근거 |
|---|---|---|
| 셰이더 언어 | HLSL 하나. backend별 원문(현재 `ShaderSource{hlsl, glsl}`, `Engine/Source/Runtime/Graphics/Public/Graphics/DeviceTypes.h:140`)을 두지 않는다 | UE 5.6도 `.usf`·`.ush` HLSL만 쓰고 ShaderFormat 모듈이 바꾼다 |
| 변환 도구 | DXC(HLSL → DXIL·SPIR-V, 1.8.2505 이상), SPIRV-Cross(SPIR-V → MSL, D3D11용 HLSL 5.0), SPIRV-Tools(`spirv-val`, 필수), SPIRV-Reflect, FXC(D3D11 DXBC, 버전을 고정한 `d3dcompiler_47.dll`), Apple `metal`·`metallib` | UE 5.6과 같은 도구다. UE가 감싸 쓰는 ShaderConductor는 원 저장소가 2023-01 보관 처리되어 쓰지 않고 DXC·SPIRV-Cross를 직접 호출한다. 도구는 UE처럼 엔진 빌드 밖의 스크립트로 직접 빌드해 바이너리로 배포한다(6절, 2026-11-13 사용자 결정 8) |
| 필수 backend | D3D12, D3D11, Vulkan, Metal | 2026-11-13 사용자 결정. 데스크톱 OpenGL 4.5는 제외한다: 현행 콘솔에 OpenGL을 강제하는 곳이 없고, macOS GL 상한은 4.1이며, UE 5.6에는 데스크톱 GL SM5 형식이 없다(열거값 `GLSL_430_REMOVED`만 남음, `Developer/ShaderFormatOpenGL/Public/ShaderFormatOpenGL.h:27`. 데스크톱 GL의 ES3.1 에뮬레이션 `GLSL_150_ES3_1`은 있다). Android 구형 기기용 GLES 3.x는 후속 선택이다(UE `GLSL_ES3_1_ANDROID`와 같은 SPIRV-Cross ESSL 경로) |
| 두 번째 검증 backend | Vulkan(2026-11-13 사용자 결정). VulkanGraphics 최소 구현을 SH2에 넣고, D3D12·Vulkan self-test가 통과하는 같은 변경에서 OpenGLGraphics를 제거한다(2026-11-13 사용자 결정 9) | SH2가 backend별 원문을 없애므로 OpenGL이 받을 셰이더가 없어진다. 같은 변경에서 바꾸면 두 backend 대조가 끊기지 않는다 |
| 런타임 | 쿠킹된 코드만 받는다. Release(UE Shipping에 해당) 실행 파일에 셰이더 컴파일러를 넣지 않는다 | UE RHI는 `RHICreatePixelShader(TArrayView<const uint8> Code, const FSHAHash&)`(`Runtime/RHI/Public/DynamicRHI.h:266`) |
| 컴파일 실행 | 메인 프로세스가 입력 수집·**전처리**·키 계산을 하고, 별도 ShaderCompileWorker 프로세스가 컴파일한다. 메인 프로세스는 DXC·FXC·SPIRV-Cross·SPIRV-Tools 바이너리와 ShaderFormat 모듈을 링크하지 않는다 | UE 5.6은 전처리를 "항상 메인 프로세스에서 job 제출 때" 한다(`Runtime/RenderCore/Public/ShaderCompilerCore.h:686–688`), 워커는 `Programs/ShaderCompileWorker`. 전처리기 구현은 12절 결정 6 |
| 캐시 | 에셋 너겟(AssetSystem 9절) | UE의 job 캐시와 원격 캐시 역할이다 |

이 문서가 정한 규칙: 바인딩·상수 버퍼·단계 사이 변수(5절), ThirdParty 종류·오프라인 도구 빌드·의존 규칙(6절), 런타임 파일 배치(7절), 셰이더 job 키 구성(8절). 사용자가 정한 것: 확장자 `.rsf`(엔트리)·`.rsh`(include)(결정 1), 도구 공급은 UE 방식(결정 8), SH2에서 Vulkan 추가와 OpenGL 제거(결정 9), 셰이더 job 키도 에셋 너겟 공통 규칙 준수(결정 10). 남은 사용자 결정(12절): HLSL 2021 범위, D3D12 셰이더 모델, 파라미터 선언, 생산 호스트, 전처리기.

## 2. 형식과 변환 경로

| 형식 id | backend | 경로 | UE 5.6 대응 |
|---|---|---|---|
| `D3D12_SM6` | D3D12 | DXC → DXIL(DXC 내장 validator 서명, 8절) | `PCD3D_SM6`: SM 6.6, 워크 그래프 6.8(`Developer/Windows/ShaderFormatD3D/Private/ShaderFormatD3D.cpp:117–164`) |
| `D3D11_SM5` | D3D11 | FXC → DXBC SM 5.0. HLSL 2021을 고른 셰이더와 FXC 1차 실패 셰이더는 DXC → SPIR-V → SPIRV-Cross HLSL 5.0 → FXC | `PCD3D_SM5`. DXC 선행 경로는 HLSL 2021·`CFLAG_PrecompileWithDXC`·FXC 1차 실패 셰이더가 탄다(`D3DShaderCompiler.cpp:797–899`, 조건 `:799`·`:832`) |
| `VULKAN_SM5` | Vulkan | DXC `-spirv`(target env `vulkan1.2`) → binding·location 재배치 → `spirv-val` | UE `SF_VULKAN_SM5`의 비-bindless 경로(Vulkan 1.1, `Developer/VulkanShaderFormat/Private/VulkanShaderCompiler.cpp:95–98`)에 해당한다. UE `SF_VULKAN_SM6`은 1.3·scalar layout·bindless다(Windows 기본 설정에서 bindless, `Engine/Config/Windows/BaseWindowsEngine.ini:57–60`) |
| `METAL_SM5` | Metal | DXC `-spirv` → 재배치 → `spirv-val` → SPIRV-Cross MSL → `metal`·`metallib` | `SF_METAL_SM5`. bindless 프로필은 UE `SF_METAL_SM6`처럼 DXIL → Metal Shader Converter(후속) |
| `VULKAN_ANDROID`·`GLES_ANDROID`·`METAL_IOS` | 모바일 | 자리만 둔다(Unsupported) | `SF_VULKAN_ES31_ANDROID`(`Runtime/RHI/Public/RHIShaderFormatDefinitions.inl:34`)·`GLSL_ES3_1_ANDROID`·`SF_METAL_ES3_1_IOS`·`SF_METAL_SM5_IOS`(`Engine/Config/IOS/DataDrivenPlatformInfo.ini:29,40`). `SF_METAL_ES3_1`은 Mac의 모바일 미리보기 형식이다(`Engine/Config/Mac/DataDrivenPlatformInfo.ini:110–115`) |

장치 요구 수준이 다르면(예: Vulkan 최소 보장보다 큰 descriptor 한도) 다른 형식 id로 둔다. 상한과 기능 요구는 형식 정의가 가진다(9절).

SM 6 기능(웨이브 연산 등)이 필요한 셰이더: UE는 이런 셰이더(`ShaderFormatD3D.cpp:128–136`)를 DXIL로 컴파일하고 `EShaderOptionalDataKey::ShaderModel6`를 붙이며(`D3DShaderCompilerDXC.cpp:1273–1277`), D3D11 RHI는 이 표시가 있는 셰이더를 만들지 않는다(`Runtime/Windows/D3D11RHI/Private/D3D11Shaders.cpp:54–66`). Rocks는 셰이더 타입이 요구 기능을 선언하고, 형식 정의의 지원 기능 목록에 없으면 그 형식에서 쿠킹하지 않는다(9절). 런타임에 없는 셰이더를 찾으면 `IncompatibleCook`이다.

## 3. 구조 요약

```text
Engine/Shaders/{Public,Private,Shared}       .rsf(엔트리)·.rsh(include)·.h(C++·HLSL 공용)
Engine/Config/ShaderFormats/<id>.json        형식 정의(9절)
Engine/Config/TargetProfiles/<id>.json       graphicsBackends·shaderFormats(9절)
Engine/Source/ThirdParty/<Lib>/              DXC·SPIRV-Cross·SPIRV-Tools(binary와 오프라인 빌드 스크립트), FXC(binary), SPIRV-Reflect·Vulkan-Headers(C)(6절)
Engine/Source/Runtime/ShaderCore             셰이더 타입·퍼뮤테이션 ID·파라미터 메타데이터·셰이더 전용 정렬 타입·라이브러리 로드
Engine/Source/Developer/ShaderCompiler       ShaderCompileManager·전처리기·키·에셋 너겟·워커 통신(메인 프로세스)
Engine/Source/Developer/ShaderFormat{D3D,Vulkan,Metal}   워커 안의 형식별 컴파일·재배치·리플렉션(도구는 C·COM 형태 API로 부른다)
Programs/ShaderCompileWorker                 워커 프로세스
```

가상 경로 `/Engine/{Public,Private,Shared,Generated}`·`/Plugin/<이름>`·`/Project`, 규칙은 UE `CheckVirtualShaderFilePath`(`Runtime/RenderCore/Private/ShaderCore.cpp:1481–1532`)와 같다. 키는 두 층이다: 셰이더 맵 키(엔트리 파일과 include 폐포의 내용 SHA-256, 낡음 판정)와 job 키(8절, 너겟 키). UE는 각각 SHA-1(`ShaderCore.cpp:2998–3071`)·BLAKE3(`ShaderCore.cpp:4144–4199`)다.

## 4. 선행 작업 개요

| ID | 작업 | 상태 | 막는 단계 | 의존 |
|---|---|---|---|---|
| P1 | 자식 프로세스·pipe API(`Platform/Process.h`) | **구현** 2026-11-13(Windows Debug·Asan, Android 에뮬레이터. macOS 미검증, CI 확인 예정) | SH1 워커 | — |
| P2 | backend 간 바인딩 규약 | 설계(5절) | SH2 | — |
| P3 | ThirdParty 확장: Engine 루트, `binary` 종류와 사용자 단위 저장소, 오프라인 도구 빌드(UE 방식)·게시 | 설계(6절) | SH0 | — |
| P4 | 빌드 시스템: 바이너리 링크와 런타임 의존 파일 배치 | 설계(7절) | SH0 | P3 |
| P5 | 에셋 너겟(AssetSystem A3) + 셰이더 job 키 | 설계(8절) | SH1 | AssetSystem A0-C(구현됨)·A2(`ProjectLayout`), ReflectionFoundations B7(툴체인 지문) |
| P6 | 형식 정의·TargetProfile `shaderFormats`·쿠킹 매니페스트 v2(A4의 일부) | 설계(9절) | 쿠킹 단계 | P5, A4 |
| P7 | MetaCompiler 셰이더 파라미터 종류 | 설계(10절) | SH3 | ReflectionFoundations B3–B4 |
| P8 | 계획 문서 개정 | 목록(11절) | 모든 단계의 기준 | 이 문서 확정 |
| P9 | VulkanGraphics 선행: Vulkan-Headers, 로더 링크, 검증 레이어, VulkanGraphics 설계 문서 | 설계(6절 5항) | SH2 | P3·P4 |

권장 순서: 12절 남은 결정 → P8·P2(문서) → P3·P4(빌드 시스템) → SH0 → B7·A2 → P5 → SH1 → P9 → SH2 → P7 → SH3 → SH4 → SH5. P6는 쿠커(A4)와 함께 한다.

## 5. P2 바인딩 규약

지금은 backend 사이 바인딩 규약이 없다. D3D12는 장치 하나에 root signature 하나(32-bit 상수 20개 b0, SRV 테이블 t0 하나, 샘플러 테이블 s0 하나, `D3D12Device.cpp:1083–1119`)이고, OpenGL은 uniform을 이름으로 찾는다(`OpenGLDevice.cpp:653–654`). 교차 컴파일 전에 아래 규칙을 고정한다.

### 5.1 UE 5.6 방식

| 항목 | UE 5.6 | 근거 |
|---|---|---|
| 선언 | 일반 파라미터는 register를 적지 않는다(`cbuffer Name {}`). 슬롯은 컴파일러가 정하고 리플렉션으로 읽어 이름 → (슬롯, 오프셋, 크기) 표에 둔다. 정적 UB·루트 상수·진단 버퍼·정적 샘플러는 예약 space를 register로 명시한다 | `Engine/Shaders/Public/Platform.ush:531`(일반), `:532`(정적 UB `UB_STATIC_CB_DEFINITION_START`), `Engine/Shaders/Public/Platform/D3D/D3DRootConstants.ush:12`, `Engine/Shaders/Public/Platform/D3D/D3DCommon.ush:23–28·40`, `Runtime/RenderCore/Public/ShaderCore.h:280–330`, `Developer/ShaderCompilerCommon/Private/ShaderCompilerCommon.cpp:669–681` |
| 예약 register space | 0 기본, 1·2 레이 트레이싱·워크 그래프, 3 루트 상수, 4 정적 셰이더 바인딩, 999 진단, 1001 NV 확장. 1000 정적 샘플러(D3D12) | `Engine/Shaders/Shared/HLSLReservedSpaces.h:19–54`, 1000은 `Runtime/D3D12RHI/Private/D3D12RootSignature.cpp:53–61`·`D3DCommon.ush:23–28` |
| D3D12 | Windows는 양자화된 셰이더 조합(QBSS)마다 root signature를 만들어 캐시한다(`USE_STATIC_ROOT_SIGNATURE 0`). QBSS와 root signature는 1:1이고, 표 크기를 양자화하므로 여러 조합이 하나를 공유한다(SRV·Sampler는 Tier 1만 2의 거듭제곱 올림이고 Tier 2 이상은 최대값 64/32, UAV와 16개를 넘는 CBV는 Tier 1–2에서 2의 거듭제곱 올림이고 Tier 3은 16). 단계 PS·VS·GS·MS·AS 순으로 SRV·(넘친 CBV)·Sampler·UAV 테이블, 상수 버퍼는 단계별 root CBV 최대 16개(`MAX_ROOT_CBVS = MAX_CBS = 16`)이고 넘치는 것은 CBV 테이블로 간다. 루트 상수는 4 DWORD(b0, space3). 12 DWORD를 넘으면 Verbose 로그만 남기고 64 DWORD 한도는 따로 검사하지 않는다(넘으면 직렬화 실패가 Fatal) | `Runtime/D3D12RHI/Private/Windows/WindowsD3D12RHIDefinitions.h:27`, `D3D12Util.cpp:1128–1162, 1270–1341`(1:1 `:1285–1287`), `D3D12RootSignature.cpp:162–489`(넘친 CBV `:298–308`, 루트 상수 `:432`, 경고 `:461–464`, Fatal `:582–585`), `D3D12RHIDefinitions.h:38`, `Public/D3D12RHI.h:17–20` |
| Vulkan | bindless가 아닐 때(`SF_VULKAN_SM5`·Android) descriptor set = 단계(VS 0, PS 1, GS 2…). binding은 SPIRV-Reflect로 0부터 연속 재배치(`$Globals` UB 0 → UB → input attachment → UAV → SRV → sampler). 구조화 버퍼 SRV는 storage buffer라 UAV 묶음에 들어간다. bindless일 때는 UB가 `BindlessSingleUseUniformBufferSet` 하나에 들어가고 binding = 단계×16+순번이다. push constant는 쓰지 않는다. SM6는 scalar layout도 쓴다 | `Runtime/VulkanRHI/Public/VulkanCommon.h:34–55`, `Developer/VulkanShaderFormat/Private/SpirVShaderCompiler.inl:697–740`(bindless `:698–699`), `:885–908`(구조화 SRV를 UAV 묶음으로 옮김 `:548–550`, 검사 `:901`), scalar `:1334–1335`, `VulkanShaders.cpp:656` |
| Metal(SPIRV-Cross) | UAV를 먼저 배정하며 같은 번호를 buffer·texture 공간에서 함께 차지한다("UAVs always claim all slots"). buffer는 그 뒤 구조화 SRV → 가속 구조 → UB 순, 버퍼 크기 side table은 남은 가장 낮은 buffer 인덱스. 정점 버퍼는 위에서부터(`(MaxMetalStreams-1)-i`, Mac 31) | `Developer/Apple/MetalShaderFormat/Private/MetalCompileShaderSPIRV.cpp:533–586`, `:703–740`, `:945–956`, `Runtime/Apple/MetalRHI/Private/MetalRHIPrivate.h:68–131` |
| D3D11 | 단계별 슬롯 상한 min(D3D11 한도, UE MAX_*), 단계당 상수 버퍼 14 | `ShaderFormatD3D/Private/D3DShaderCompiler.cpp:1080–1084`, `Runtime/Windows/D3D11RHI/Private/D3D11RHIPrivate.h:868` |
| 상수 버퍼 레이아웃 | C++ 쪽 정렬(4/8/16, 구조체·배열 원소 16)과 static_assert, 생성 HLSL에 명시 padding, 스칼라 배열은 float4로 묶는다(`SHADER_PARAMETER_SCALAR_ARRAY`). SPIR-V는 UE가 수정한 DXC의 `-fvk-ue5-layout`(cbuffer 엄격한 std140, DXC 기본은 vector-relaxed std140). Vulkan SM6·레이 트레이싱만 scalar | `Runtime/RenderCore/Public/ShaderParameterMacros.h:934–1444, 2025–2028`, `Engine/Shaders/Public/Platform.ush:1455`, `ShaderParameters.cpp:226–326`, `ThirdParty/ShaderConductor/.../ShaderConductor.cpp:1247–1249` |
| 정점 입력 | 시맨틱 `ATTRIBUTEn`. VS 입력만 시맨틱 번호로 location을 다시 배정하고 단계 사이 변수는 다시 쓰지 않는다 | `Engine/Shaders/Private/LocalVertexFactory.ush:52–60`, `Developer/ShaderCompilerCommon/Private/SpirvReflectCommon.cpp:202–215`, `SpirVShaderCompiler.inl:552–556`, `MetalCompileShaderSPIRV.cpp:861–900` |
| 좌표 | 셰이더는 D3D 규약(NDC y 위, z [0,1]). Vulkan은 음수 viewport 높이로 뒤집는다. SPIR-V·MSL 대상에 `-fvk-use-dx-position-w`를 넘겨 PS의 `SV_Position.w`를 D3D 의미로 맞춘다(원 DXC에도 있는 옵션) | `Runtime/VulkanRHI/Private/VulkanPendingState.cpp:503–510`, `ThirdParty/ShaderConductor/.../ShaderConductor.cpp:1250–1254` |

### 5.2 Rocks 규약

1. **선언.** 셰이더 원문은 register·`[[vk::*]]`를 적지 않는다(UE의 일반 파라미터와 같다). 예외는 루트 상수 매크로 하나이며 `Platform.rsh`가 형식별로 전개한다(2항). 정적 샘플러 같은 다른 예약 space는 후속이다.
2. **루트 상수(그리기별 상수).** `ROCKS_ROOT_CONSTANTS(Type, Name)` 하나로 선언한다. 크기는 최대 128 byte(32 DWORD, Vulkan push constant 최소 보장)이고 멤버는 5항 규칙을 따른다. 현재 `DrawConstants`(80 byte)가 첫 사용자다.

   | 형식 | 전개 | 실제 위치 |
   |---|---|---|
   | `D3D12_SM6` | `ConstantBuffer<Type> Name : register(b0, space3)` | root 32-bit constants(UE 예약 space 3과 같은 번호) |
   | `D3D11_SM5`(FXC) | `cbuffer` `register(b13)`(SM 5.0에는 space가 없다) | 그리기마다 갱신하는 상수 버퍼, 단계별 마지막 슬롯 |
   | `D3D11_SM5`(SPIRV-Cross 경로) | 아래 Vulkan 전개로 SPIR-V를 만든 뒤 `spvc_compiler_hlsl_set_root_constants_layout`으로 b13 | 위와 같다 |
   | `VULKAN_SM5` | `[[vk::push_constant]] Type Name` | push constant. DXC는 push constant에 구조화 버퍼 규칙(`sBufferLayoutRule`, 기본 vector-relaxed std430)을 쓴다(DXC `tools/clang/lib/SPIRV/DeclResultIdMapper.cpp:1543–1547`(일반 struct), `:1661`(`ConstantBuffer<T>`)) |
   | `METAL_SM5` | Vulkan 전개와 같다 | SPIRV-Cross push constant 자원(`SPVC_MSL_PUSH_CONSTANT_DESC_SET`·`SPVC_MSL_PUSH_CONSTANT_BINDING`)을 buffer 0에 지정하고 `set{Vertex,Fragment}Bytes`로 올린다 |

   UE는 Vulkan에서 push constant를 쓰지 않는다. Rocks는 그리기별 상수를 위해 쓰며 5.3절 시험 (a)·(d)로 확인한다.
3. **슬롯 정규화.** 컴파일 후 형식별 리플렉션(DXIL `ID3D12ShaderReflection`, SPIR-V SPIRV-Reflect)을 Rocks 공통 `ShaderBindingLayout`으로 바꾼다. 단위는 (단계, 종류, 단계 안 순번)이며 이름 hash·CB 크기·멤버 오프셋·텍스처와 샘플러의 짝(9항)을 함께 둔다(UE `FShaderParameterMap`·`FVulkanShaderHeader` 대응). 종류는 Vulkan descriptor 종류를 기준으로 나눈다: 상수 버퍼, SRV 텍스처(typed buffer SRV 포함), 구조화·raw 버퍼(SRV·UAV), storage image(UAV 텍스처·typed buffer UAV), 샘플러.
4. **상한과 backend별 실제 위치.**
   - **상한 검사 위치.** 형식 정의의 상한(장치 요구 수준별)과 프로필 공통 상한(프로필에 든 형식들의 최소값)은 job 안에서 검사한다. 형식 정의와 프로파일 출력 설정이 모두 키에 있으므로(8절) 프로필에 형식을 더해 상한이 내려가면 키가 바뀌어 다시 검사된다. 파이프라인 합계 한도(D3D12 Tier 1 UAV, Vulkan `maxDescriptorSet*`)는 파이프라인 layout을 만들 때(쿠킹 때 알려진 쌍, 런타임 생성) 검사한다.
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
   - **D3D12.** 파이프라인마다 VS·PS layout을 합쳐 root signature를 만들고 캐시한다. UE QBSS 구조를 따르되 처음에는 개수를 양자화하지 않는다(root signature 수가 문제가 되면 UE 양자화를 들인다). 순서는 단계별 테이블(SRV·Sampler·UAV, 필요하면 CBV) → root CBV → 루트 상수다. 예산식 `루트 상수 DWORD + 테이블 수 + 2 × root CBV 수 ≤ 64`를 만족하도록 root CBV를 채우고, 넘치는 CBV는 높은 슬롯부터 그 단계의 CBV 테이블로 내린다(UE의 넘친 CBV 테이블과 같은 방식, UE는 16개에서 자른다). 테이블은 단계당 최대 4개라 다섯 단계를 다 써도 루트 상수 32와 테이블 20의 합이 52 ≤ 64이므로 항상 만들 수 있다.
   - **Vulkan.** set = 단계(VS 0, PS 1, CS 0. 최소 보장 `maxBoundDescriptorSets` 4 안이다), binding = 종류 순서(상수 버퍼 → 구조화·raw 버퍼 → storage image → SRV 텍스처 → 샘플러)로 0부터 연속. 워커가 DXC 출력의 decoration을 SPIRV-Reflect로 고친다(UE `SF_VULKAN_SM5`의 비-bindless 경로와 같은 방식, `-fvk-*-shift`는 쓰지 않는다).
   - **Metal.** buffer 인덱스: 0은 루트 상수, 1부터 UAV(UE처럼 buffer·texture 공간에서 같은 번호를 함께 차지한다) → 구조화 SRV → 상수 버퍼 → 버퍼 크기 side table. texture 인덱스: UAV가 차지한 번호(1부터)를 비우고 SRV 텍스처는 0과 남은 가장 낮은 번호부터 쓴다. sampler는 0부터다. 정점 버퍼는 30부터 아래로 쓴다(UE와 같은 방향). 최대 정점 스트림을 8로 고정하고, VS·PS는 셰이더 buffer 인덱스 ≤ 22(정점 버퍼 8개가 30–23을 차지한다)를 컴파일 때 검사한다. 공통 상한이면 1 + 12 + 4 + 1 = 18개라 들어간다. 실제 정점 버퍼 수와의 겹침은 파이프라인 생성 때 검사한다. CS는 정점 버퍼가 없으므로 0–30을 쓴다.
   - **D3D11.** FXC 경로는 FXC가 정한 단계별 슬롯을 그대로 쓰고 상한을 넘으면 오류다. PS UAV는 공통 상한에서 금지되므로 FXC가 u0을 주어 render target 0과 겹치는 일이 없다. SPIRV-Cross 경로는 종류 구분 없이 매긴 SPIR-V binding을 `spvc_compiler_hlsl_add_resource_binding`으로 종류별 register(b·t·u·s, 단계별 0부터)에 다시 배정하고(그대로 두면 s24처럼 범위 밖 register가 나온다), 정점 입력은 `spvc_compiler_hlsl_add_vertex_attribute_remap`으로 `ATTRIBUTEn` 시맨틱을 유지한다(SPIRV-Cross 기본은 `TEXCOORD{location}`).
5. **상수 버퍼 레이아웃.** D3D cbuffer, DXC 기본 cbuffer 규칙(vector-relaxed std140), push constant 규칙(vector-relaxed std430)이 같은 오프셋을 내도록 허용 타입을 좁히고, 워커가 실제 오프셋을 검사한다.
   - 멤버 타입: 4 byte 스칼라(float·int·uint), 그 2–4성분 벡터, `float4x4`만 허용한다. bool·double·half(`min16*`)와 다른 행렬(`float3x3` 등)은 컴파일 오류다. `float3x4`·`float4x3`는 6항의 행렬 저장 결정 뒤 별도 시험으로 허용 여부를 정한다.
   - 정렬: 스칼라 4, 2성분 8, 3·4성분·행렬 16. 이 정렬이 요구하는 자리에 명시 padding 멤버를 둔다(UE와 같다).
   - 배열: 원소 크기가 16 byte 배수일 때만 허용한다. 스칼라·2·3성분 배열은 float4 배열로 묶는다(UE `SHADER_PARAMETER_SCALAR_ARRAY`·`GET_SCALAR_ARRAY_ELEMENT`와 같은 방식).
   - 구조체: 16 byte 정렬로 시작하고 크기를 padding으로 16 byte 배수로 맞춘다.
   - **워커 검사.** 모든 형식의 리플렉션 오프셋이 위 규칙으로 계산한 오프셋과 같아야 하며, 다르면(암묵 padding·다른 배치) 컴파일 오류다. 손으로 쓴 구조체(SH0–SH2의 `ROCKS_ROOT_CONSTANTS`, `DrawConstants`)도 같다.
   - 반례(이 규칙이 막는 것): `float a[2]; float b;`는 D3D에서 b가 20(마지막 원소의 남는 자리에 채움), std140에서 32이고, 명시 padding으로도 두 규칙을 함께 맞출 수 없다. `float3x3 m; float s;`는 D3D 44, std140 48이다. push constant의 스칼라 배열은 std430 stride 4, D3D root constant는 16이다. `struct S { float4 v; }; struct R { float a; S s; }`의 s는 D3D·std140 16, push constant(std430, 구조체 정렬 4) 4다. 명시 padding이 없으면 워커 검사가 거부한다.
   - C++ 타입: Core `Matrix4<float>`·`Vector3`는 4 byte 정렬이다(`Core/Source/Runtime/Core/Public/Core/Math/Matrix4.h:57–60`). 그래서 ShaderCore에 셰이더 전용 정렬 타입(`alignas(8)` 2성분, `alignas(16)` 3·4성분·`float4x4`)을 두고, 셰이더 파라미터 구조체는 이 타입만 쓴다(10절에서 MetaCompiler가 강제). `DrawConstants`의 C++ `float transform[16]`(`DeviceTypes.h:161–162`)은 이 행렬 타입으로 바꾼다.
   - Rocks는 수정되지 않은 DXC를 쓰므로 scalar block layout을 쓰지 않는다(Android Vulkan 1.1과 호환).
6. **행렬.** Rocks는 열 벡터·열 우선 저장(EngineRuntime 10.1)이므로 UE의 `-Zpr`를 쓰지 않는다. 형식별 DXC 행렬 옵션은 비대칭 행렬을 쓴 픽셀 대조 시험(SH2)으로 확정하고 이 문서에 기록한다. 시험 전에는 옵션을 단정하지 않는다.
7. **정점 입력과 단계 사이 변수.** DXC `-spirv`는 입출력 location을 시맨틱 번호가 아니라 선언 순서로 준다(기본 `-fvk-stage-io-order=decl`). D3D11·D3D12는 VS 출력과 PS 입력을 register 위치로 맞추고 register는 선언 순서로 정해진다. 그래서 두 단계를 따로 컴파일해도 맞도록 다음을 지킨다.
   - 정점 입력 시맨틱은 `ATTRIBUTEn`이다(현재 `TEXCOORDn`, `DeviceTypes.h:130–136`). n은 `VertexAttribute::location`이다.
   - 단계 사이 변수는 하나의 struct로 선언하고 VS 출력과 PS 입력이 같은 struct를 쓴다(공용 `.rsh`). struct 멤버 순서는 `SV_Position` 다음 `TEXCOORDn`을 n 오름차순으로 둔다. 그러면 선언 순서 = location 순서라 FXC 경로(선언 순서)와 SPIRV-Cross 경로(location 순서)의 signature가 같다. PS 입력에 다른 struct를 쓰려면 VS 출력의 앞부분(같은 순서)이어야 한다.
   - 허용 시맨틱: 단계 사이는 `TEXCOORDn`(n ≤ 15). 시스템 값은 `SV_Position`·`SV_Target0–7`·`SV_Depth`·`SV_VertexID`·`SV_InstanceID`·`SV_IsFrontFace`와 CS의 `SV_DispatchThreadID`·`SV_GroupID`·`SV_GroupThreadID`·`SV_GroupIndex`다. 형식별 기능이 필요한 시스템 값(PS의 `SV_PrimitiveID`는 Vulkan Geometry capability, VS의 `SV_RenderTargetArrayIndex`·`SV_ViewportArrayIndex`는 선택 기능)은 형식 정의의 기능 요구로 후속이다. 그 밖은 컴파일 오류다.
   - 워커가 SPIR-V 입력·출력 location을 시맨틱 번호 n으로 다시 쓴다. UE는 VS 입력(`ATTRIBUTEn`)만 시맨틱 번호로 다시 배정하고(`SpirVShaderCompiler.inl:552–556`, `MetalCompileShaderSPIRV.cpp:861–900`) 단계 사이 변수는 다시 쓰지 않는다. 단계 사이 변수 재작성은 Rocks가 더하는 규칙이다. Metal의 `[[attribute(n)]]`·`[[user(locnN)]]`도 이 location에서 나온다.
   - 다시 쓴 SPIR-V는 `spirv-val`로 검증한다. 그래서 SPIRV-Tools는 필수다.
   - VS·PS 쌍의 연결은 DXIL·DXBC signature와 SPIR-V location을 대조해 검사한다(쿠킹 때 알려진 쌍, 파이프라인 생성 때 Development 검사).
   - 셰이더가 쓰지 않는 attribute는 리플렉션 입력 마스크로 거른다(UE `InOutMask`).
8. **좌표와 앞면.** 셰이더는 D3D 규약으로 쓴다. Vulkan은 음수 viewport 높이(Vulkan 1.1 core)로 뒤집고, SPIR-V·MSL 대상에는 `-fvk-use-dx-position-w`를 넘긴다(UE와 같다). 셰이더에 backend 분기를 두지 않는다(계획 11.5 "한 번만 적용"). Graphics API의 앞면 감기 방향은 framebuffer(y 아래) 기준 하나로 정한다. backend별 `frontFace` 매핑은 컬링 픽셀 대조(SH2 Vulkan, SH5 Metal·D3D11)로 확정한다.
9. **텍스처와 샘플러.** 텍스처마다 샘플러를 정하는 현재 규칙(`DeviceTypes.h:145–148`)을 유지한다. HLSL은 둘을 따로 선언하므로 짝 규칙을 둔다: 텍스처 `X`는 같은 단계의 `SamplerState XSampler`와 짝이다. 짝이 없는 텍스처·샘플러와, 한 샘플러를 여러 텍스처가 쓰는 경우는 컴파일 오류다. 공유·정적 샘플러(UE space1000)는 후속이다.
10. **bindless.** 기본 프로필에 넣지 않는다. 후속 bindless 프로필은 D3D12 `ResourceDescriptorHeap`(SM 6.6), Vulkan bindless set, Metal Shader Converter로 UE 5.6 구조를 따른다.

### 5.3 시험과 완료 기준

픽셀 대조는 Sandbox self-test의 허용 오차(채널 ±8, 2%, `Programs/Sandbox/Private/GraphicsSelfTest.cpp:394–417`) 안 일치를 뜻한다. 행렬 전치·attribute 뒤바뀜이 허용 오차를 크게 넘도록 fixture를 만든다.

| 시험 | 내용 | 시기 |
|---|---|---|
| (a) | 형식마다 리플렉션 오프셋 = 규칙 오프셋 = C++ 오프셋(상수 버퍼와 루트 상수) | SH2(손으로 쓴 구조체), SH3(생성 구조체) |
| (b) | 한 셰이더 안 binding 충돌 0건, 형식 상한 초과는 컴파일 오류, 프로필 공통 상한 초과는 쿠킹 오류 | SH2(형식), P6(프로필) |
| (c) | 비대칭 행렬·attribute 순서를 바꾼 정점 입력·단계 사이 변수·컬링의 픽셀 대조 | D3D12·Vulkan은 SH2. D3D11·Metal은 SH5까지 **계약(미검증)** |
| (d) | 루트 상수 128 byte 경계, D3D12 64 DWORD 예산 초과 시 CBV 테이블로 내림 | SH2 |
| (e) | Metal 셰이더 buffer 인덱스 상한 음성 대조(컴파일), 정점 스트림 겹침 음성 대조(파이프라인 생성) | SH2(컴파일), SH5(파이프라인) |
| (f) | 5항 반례(`float a[2]; float b;`, `float3x3`, bool, 스칼라 배열, padding 없는 구조체)의 컴파일 거부 | SH2 |
| (g) | location 재작성 뒤 `spirv-val` 통과, 선언 순서를 바꾼 VS·PS 쌍의 연결 거부·일치(SPIR-V·DXIL) | SH2. D3D11 두 경로 혼합은 SH5 |
| (h) | 텍스처·샘플러 짝 규칙의 음성 대조 | SH2 |

P2 완료 기준: 이 절의 규칙과 `ShaderBindingLayout` 형식이 확정되고, 위 시험이 SH2·SH3·SH5 완료 기준에 들어 있다.

## 6. P3 ThirdParty 확장과 도구 빌드

현재 ThirdParty 기반은 C 원본 라이브러리용이다: 루트가 `Core/Source/ThirdParty`로 고정되어 있고(`Programs/Rocks.Tooling.Common/ThirdPartyManifest.cs:76`), 번역 단위는 `.c`만 받으며(`:358–364`), 모든 원본이 Rocks 경고·예외 규칙(`/W4 /WX`, `-Wall -Wextra -Wpedantic -Werror`, C++는 `-fno-exceptions -fno-rtti`)으로 빌드되고 모듈이 이를 바꿀 수 없다(`Programs/Rocks.Build/ActionGraph.cs:393–435`, `InputValidation.cs:146–179`). 미리 빌드된 바이너리를 적는 필드도 없다(`kind`는 code·data).

**도구 공급은 UE 방식이다(2026-11-13 사용자 결정 8).**

UE의 구조는 다음과 같다.
- UE는 DXC와 ShaderConductor(SPIRV-Cross·SPIRV-Tools 포함)를 엔진 빌드(UBT) 밖의 스크립트로 빌드한다. 스크립트는 CMake·Ninja와 Python `BuildAll.py`를 쓰고, macOS에서는 install name 수정과 `dsymutil`까지 한다(`Engine/Source/ThirdParty/ShaderConductor/Build_ShaderConductor_Win64.bat`, `Build_ShaderConductor_Mac.command:1–51`).
- 결과 공유 라이브러리는 `Engine/Binaries/ThirdParty/ShaderConductor/<플랫폼>`에 두고 GitDependencies로 배포한다.
- 엔진 쪽 모듈은 `Type = ModuleType.External`이고 import library 링크와 런타임 파일 복사만 한다(`ShaderConductor.Build.cs:11–68`).

Rocks도 같은 구조로 한다. 그래서 Rocks.Build에 공유 라이브러리 모듈 종류나 C++17·예외 빌드 설정을 더하지 않는다. 각 도구는 자기 공유 라이브러리 안에서 예외·STL을 쓰고, 밖으로는 C 또는 COM 형태 API만 내보낸다.
- DXC: `DxcCreateInstance`와 `IDxcCompiler3` 같은 COM 형태 인터페이스를 내보낸다. 오류는 HRESULT로 돌려준다.
- SPIRV-Cross: `spirv_cross_c.h` C API를 내보낸다.
  - CMake `SPIRV_CROSS_SHARED`로 C API를 단일 공유 라이브러리(`spirv-cross-c-shared`)로 만든다(SPIRV-Cross `CMakeLists.txt:40, 358`).
  - 예외는 기본으로 켜져 있고(`:39`), C API가 예외를 오류 값으로 바꾼다(`spirv_cross_c.cpp`의 `SPVC_BEGIN_SAFE_SCOPE`).
- SPIRV-Tools: `spirv-tools/libspirv.h` C API(`spvValidate` 등)를 공유 라이브러리로 내보낸다.

1. **Engine 쪽 루트.** `ThirdPartyRoot` 상수를 루트 목록(`Core/Source/ThirdParty`, `Engine/Source/ThirdParty`)으로 바꾼다.
   - 의존 방향은 그대로다. Core가 쓰는 라이브러리는 Core 루트에, 엔진·도구 전용은 Engine 루트에 둔다.
   - 바꿀 코드: `ThirdPartyManifest.cs:76`, `Build/Rules/ThirdPartyModuleRules.cs:37`, `Programs/Rocks.Tooling.Checks/ThirdPartyDirectiveProbe.cs:34`, `ThirdPartyManifestRefresh.cs:22`, `PolicyChecks.ThirdParty.cs`.
   - 함께 바꿀 것: lexical 제외, SDK 라이선스 복사, 도구 시험, `.gitattributes:11`·`.editorconfig:38–40`(원본 byte 보존 규칙).
   - 문서 개정은 11절에 있다.
2. **미리 빌드된 바이너리(`kind: "binary"`).** 엔진 빌드가 컴파일하지 않고 링크·복사만 하는 도구다. manifest 항목:
   - `version`, 라이선스(`spdx`·고지 파일), 출처. 출처는 Rocks 도구 빌드가 게시한 archive의 URL·`sha256`과 그 빌드의 원본 태그·patch 목록이다. 설치된 SDK에서 가져오는 도구는 그 SDK의 경로 규칙을 적는다.
   - 호스트별 파일 목록 `hosts.<플랫폼>-<아키텍처>[] = {path, sha256, size, role}`.
     - 호스트 키는 `windows-x64`·`macos-arm64`처럼 아키텍처를 포함한다(계획 §2.5: x64·ARM64 산출물을 섞지 않는다).
     - 지원 개발 호스트는 Windows·macOS뿐이다(`Programs/Rocks.Build/Toolchain.cs:15`).
     - 포매터 설치(`Build/InstallFormatter.ps1`, `out/toolchains`, 호스트 키 `windows-X64`)는 빌드 입력이 아니므로 기존 관례를 유지한다.
   - `role` 값:
     - `runtime`: 실행 파일 옆에 복사한다.
     - `import`: 링크한다.
     - `header`: 같은 버전의 `dxcapi.h`·`spirv_cross_c.h`·`libspirv.h`다. 외부 include 옵션이 금지이므로 기존 `adapterHeaders`와 같은 방식으로 경고 0을 확인한다.
     - `symbols`(PDB·dSYM), `tool`, `notice`.
   - **저장 위치.** 바이너리는 저장소와 `out/`에 두지 않는다.
     - 사용자 단위 내용 주소 저장소를 쓴다. Windows는 `%LOCALAPPDATA%\Rocks\ThirdParty\<sha256>`, macOS는 `~/Library/Application Support/Rocks/ThirdParty/<sha256>`이다. `Caches`는 OS가 지울 수 있어 쓰지 않는다. 선택 미러 경로도 둔다.
     - 명시적인 bootstrap 명령에서만 받는다. UE GitDependencies와 같은 역할이다(`Programs/GitDependencies/Program.cs:1305–1418`, UE는 SHA-1).
     - 빌드 action 안에서는 받지 않는다. 없으면 bootstrap을 안내하는 오류로 실패한다. 그래서 깨끗한 `out/`도 네트워크 없이 빌드된다(계획 §27.9).
     - CI는 manifest SHA를 키로 저장소를 캐시한다(`.github/workflows/foundation.yml`에 캐시 단계 추가).
   - 받은 파일은 임시 경로에 쓰고 archive·파일 SHA-256을 검사한 뒤 옮긴다. 검사에 실패하면 오류다. 저장소의 파일은 고치지 않는다(install name 수정과 서명은 도구 빌드에서 끝낸다).
   - 이미 있는 모듈 필드 `PrebuiltLibraries`(`Programs/Rocks.Build/NativeBuildModels.cs:102`, 링크 입력)는 그대로 쓴다. `role: import` 파일을 이 목록으로 넘긴다. 설치 SDK 내보내기는 이 필드가 있는 모듈을 지금처럼 거부한다(`Programs/Rocks.Build.Cli/InstalledSdk.cs:69`).
   - **의존 규칙.** DXC·SPIRV-Cross·SPIRV-Tools·FXC binary에 의존할 수 있는 것은 `Programs/ShaderCompileWorker`와 `Developer/ShaderFormat*`뿐이다. 메인 프로세스(`Developer/ShaderCompiler`)와 Runtime·게임 모듈의 의존은 정책 검사가 거부한다.
   - **FXC.** System32의 `d3dcompiler_47.dll`은 OS 빌드마다 다르므로 쓰지 않는다.
     - 버전을 고정한 Windows SDK를 설치하고, bootstrap이 그 SDK의 `Redist/D3D/x64/d3dcompiler_47.dll`을 SHA-256 검사 뒤 저장소로 복사한다.
     - Windows SDK는 archive가 아니라 설치기라서 다시 게시하지 않는다.
     - UE도 동봉본(`Engine/Binaries/ThirdParty/Windows/DirectX/x64/d3dcompiler_47.dll`)을 전체 경로로 연다(`Developer/Windows/ShaderFormatD3D/Private/D3DShaderCompiler.cpp:395`).
   - **Apple `metal`·`metallib`.** 저장소로 받지 않는 시스템 도구다(Xcode Metal 도구 또는 Windows용 Metal Developer Tools).
     - manifest에 `kind: binary`, `status: system`으로 적고, 기대 `metal -v` 문자열과 호스트별 실행 파일 SHA-256을 고정한다. 워커가 실행 전에 대조한다.
     - Xcode 26부터 Metal 도구는 별도 구성요소다(`xcodebuild -downloadComponent metalToolchain`). CI 설치 단계에 넣는다.
3. **오프라인 도구 빌드(UE 방식).** DXC·SPIRV-Cross·SPIRV-Tools는 라이브러리마다 `Engine/Source/ThirdParty/<Lib>/Build/`의 스크립트(Windows `.cmd`, macOS `.sh`)로 빌드한다.
   - 입력:
     - 고정한 원본 태그. DXC는 1.8.2505 이상이다(8절 서명).
     - 원본 SHA. 원본은 스크립트가 받아 SHA를 검사한다.
     - Rocks patch 목록. 예를 들어 8절의 SPIRV-Cross 실수 출력 수정이 필요해지면 여기에 넣는다.
     - 저장소에는 스크립트·patch·manifest만 둔다.
   - 도구: CMake·Ninja와 호스트 컴파일러(Windows clang-cl 또는 MSVC, macOS Xcode clang). DXC와 SPIRV-Tools 빌드는 Python으로 표를 생성한다.
   - 버전 정합: SPIRV-Tools·SPIRV-Headers는 고정한 DXC 릴리스의 submodule 커밋으로 빌드한다. 그러면 `spirv-val`이 DXC가 만든 SPIR-V와 같은 버전이다.
   - 산출물:
     - Windows x64: DLL·import library·PDB.
     - macOS arm64: dylib(install name `@rpath/<이름>`, ad-hoc 서명)·dSYM.
     - 공용 헤더·라이선스 고지.
     - 이것을 archive로 묶어 SHA-256과 함께 게시하고, 2항 manifest에 등록한다.
     - UE처럼 Windows DXC도 같은 태그에서 직접 빌드한다. 두 호스트가 같은 원본·버전을 쓴다.
   - 실행 위치:
     - 수동으로 실행하는 별도 CI workflow(`.github/workflows/third-party-tools.yml`)가 두 호스트에서 빌드·게시한다. 개발자 기계에서도 같은 스크립트로 재현한다.
     - 이 workflow는 기본 빌드·시험 경로가 아니다. 그래서 Python을 막는 정책 검사(`Programs/Rocks.Tooling.Checks/PolicyChecks.cs`의 `PythonFreeMainPath`)에 이 workflow와 `Engine/Source/ThirdParty/*/Build/`를 명시 제외로 더한다.
     - 엔진 빌드·시험과 Foundation CI는 계속 Python 없이 돈다.
   - 경고·예외: 이 빌드는 Rocks 경고·예외 규칙 대상이 아니다(UE의 External 모듈과 같다). Rocks 코드는 공개 헤더만 include하므로 헤더의 경고 0만 확인한다(2항 `role: header`).
4. **C 원본.** SPIRV-Reflect·Vulkan-Headers는 C이므로 기존 규칙 그대로 Engine 루트에 둔다.
5. **Vulkan(P9).** VulkanGraphics(SH2)에 필요하다.
   - 로더: Windows는 드라이버가 `vulkan-1.dll`을 설치한다. import library는 binary 항목으로 받는다. PlatformBase에 동적 라이브러리 API가 없으므로 동적 로드(volk 방식)는 쓰지 않는다.
   - 검증 레이어는 D3D12 디버그 레이어에 대응한다(EngineRuntime 디버그 규칙). binary 항목이며 Development에서만 켠다.
   - macOS는 Metal을 쓰므로 Vulkan(MoltenVK 등)은 범위 밖이다.
   - CI에 Vulkan 장치(소프트웨어 ICD 포함)가 없으면 Vulkan self-test는 CI에서 **NOT RUN**으로 보고하고 로컬 결과를 기록한다. 소프트웨어 ICD를 CI에 들일지는 P9에서 정한다.

완료 기준:
- 두 루트의 manifest·정책 시험.
- bootstrap의 정상 경로·SHA 불일치·중단 뒤 재시도, 그리고 저장소만으로 하는 오프라인 빌드 시험.
- binary 의존 규칙의 음성 대조.
- 도구 빌드 workflow가 Windows·macOS 산출물을 게시하고, 그 SHA가 manifest와 일치(install name·서명·dSYM 검사 포함).
- FXC 고정본 등록.
- SPIRV-Reflect가 Rocks 경고 규칙으로 Windows·macOS에서 빌드됨.

## 7. P4 바이너리 링크와 런타임 의존 파일

UE의 방식은 다음과 같다.
- DXC는 Windows에서 `GetDllHandle`로 전체 경로에서 연다(`Developer/ShaderCompilerCommon/Private/DXCWrapper.cpp:70–75`, `#if PLATFORM_WINDOWS` 안).
- ShaderConductor는 import library로 링크하되, DLL을 지연 로드하고 `RuntimeDependencies`로 복사한다(`ShaderConductor.Build.cs:44–68`).

Rocks PlatformBase에는 동적 라이브러리 API가 없고, 계획은 동적 로더를 Foundation 선행 조건으로 두지 않는다(§2.5). 그래서 모든 도구를 **링크 방식**으로 쓰고 지연 로드는 쓰지 않는다.

- 모듈 규칙에 `ThirdPartyBinaries = ["DXC", "SPIRV-Cross", "SPIRV-Tools", "FXC"]`처럼 바이너리 라이브러리를 적는다. 그러면 빌드가 두 가지를 한다.
  - manifest의 `role: import` 파일을 `PrebuiltLibraries`로 링크한다.
  - `role: runtime` 파일을 실행 파일 옆 `bin/`에 복사한다. Windows는 `dxcompiler.dll`·`spirv-cross-c-shared.dll`·SPIRV-Tools DLL·`d3dcompiler_47.dll`이고, macOS는 대응하는 dylib다.
  - 복사는 이미 있는 ASan 런타임 복사(`RuntimeFiles`, `Toolchain.cs:65–71`)와 같은 `Copy` action을 일반화한다. UE `RuntimeDependencies`에 해당한다.
- Windows 로더는 실행 파일 디렉터리를 먼저 찾으므로 복사본이 로드된다. 로드된 모듈 경로가 복사본인지 시험으로 확인한다(`d3dcompiler_47.dll`이 System32 것으로 바뀌지 않게).
- macOS는 실행 파일에 `@executable_path` rpath를 넣는다.
- 복사·링크 입력은 증분 빌드 입력이다. 파일 SHA-256이 바뀌면 다시 복사·링크한다.
- 설치 SDK 내보내기는 바이너리 의존 모듈을 지금처럼 거부한다. 호스트 도구 프로그램(ShaderCompileWorker)은 SDK의 도구 묶음으로 따로 내보낸다(후속).
- D3D12 SM 6.6을 고르고 Windows 10을 지원하면(12절 결정 3) Agility SDK가 **Runtime** 실행 파일의 의존이 된다. Agility SDK는 `D3D12Core.dll`과 실행 파일의 `D3D12SDKVersion`·`D3D12SDKPath` export로 이루어진다. 이때는 같은 복사 규칙을 Runtime 대상에도 적용한다.

완료 기준:
- 바이너리 의존 실행 파일이 깨끗한 `out/`에서 빌드·실행된다(Windows·macOS·Asan).
- DLL을 지우면 다시 복사된다.
- 로드된 DLL이 복사본이다.
- Release 대상의 링크 입력에 셰이더 컴파일러가 없음을 정책 시험으로 확인한다.

## 8. P5 에셋 너겟과 셰이더 job 키

AssetSystem A3(9절 설계)을 그대로 구현하고, 셰이더 job도 AssetSystem 9절의 **공통 키 규칙을 그대로 따른다**(2026-11-13 사용자 결정 10). 의존: A2 `ProjectLayout`(캐시 위치 `<Project>/Intermediate/AssetNuggets/`, AssetSystem 9절), ReflectionFoundations B7(툴체인 지문). 프로젝트 없이 엔진 셰이더만 컴파일할 때(Sandbox·self-test)는 엔진 루트의 `Intermediate/AssetNuggets/`를 같은 설정 규칙으로 쓴다.

**job 키.** 메인 프로세스가 계산한다(ShaderFormat 모듈을 링크하지 않으므로 형식별 값은 형식 정의에서 읽는다). 처리기 `BuildKey(KeyWriter&)`가 아래 순서·타입 태그로 기록한 바이트의 SHA-256이다. 순서와 항목은 AssetSystem 공통 항목(`AssetSystem.ko.md` 9절)을 따르고, 에셋이 아니므로 에셋 TypeId·입력 bulk 자리에 셰이더 입력을 넣는다.

1. `"RocksAssetNugget"` 형식 버전.
2. 셰이더 job 처리기 Guid·처리기 버전 Guid.
3. 결과를 만드는 외부 도구의 이름·버전·설정.
   - 형식 id와 형식 정의 파일(`Engine/Config/ShaderFormats/<id>.json`)의 SHA-256. 형식 정의는 ShaderFormat 처리기 버전, 컴파일러·셰이더 모델, 상한·기능 요구를 가진다.
   - 이 job이 쓰는 외부 도구(DXC·SPIRV-Cross·SPIRV-Tools·FXC)의 manifest SHA-256 목록, SPIRV-Reflect 원본 manifest SHA-256, Apple 도구 지문. 도구 빌드의 원본 태그·patch·컴파일러가 바뀌면 바이너리 SHA가 바뀐다.
   - 워커는 실행 전에 자기 바이너리가 manifest와 같은지 확인하고 다르면 실패한다.
4. 결정적 수학 버전(A0-C).
5. 툴체인 지문(ReflectionFoundations B7, 컴파일러 id·버전·대상 아키텍처·부동소수점 모델).
6. 입력: 전처리 결과의 SHA-256(가상 경로만 담긴 `#line`), 엔트리 이름, 단계. include 폐포는 전처리 결과에 들어 있으므로 별도 의존 목록은 없다.
7. 결과에 영향을 주는 설정 전체: 컴파일 옵션(출력에 영향을 주는 정의·플래그의 정렬 목록, 기본값 포함), 파라미터 레이아웃 서명(10절).
8. 대상 프로파일 id와 출력 설정 전체.

공통 규칙을 따른 결과:
- 툴체인 지문에 대상 아키텍처가 들어가므로 Windows x64와 macOS arm64는 같은 셰이더도 키가 다르다. 셰이더 캐시는 호스트 사이에 공유하지 않는다.
- 한 호스트만 만들 수 있는 형식(Windows의 DXBC, 12절 결정 5의 metallib)은 다른 호스트에서 해당 형식 job이 `CapabilityUnavailable`로 실패한다(계획 §27.9). 원격 워커(계획 §2.5 "macOS는 같은 action을 Windows worker에 전달", §27.7)가 생기면 그쪽으로 보낸다. 이때 키는 요청한 호스트의 것이다.
- 프로파일의 출력 설정(텍스처 LOD 그룹, 셰이더 형식 목록 등)이 바뀌면 그 프로파일의 셰이더 키가 모두 바뀐다. 반대로 셰이더 형식 목록을 바꾸면 같은 프로파일의 텍스처 키도 바뀐다. 이것은 공통 규칙의 비용으로 받아들인다.

Apple 도구 지문: `metal -v`의 버전·대상 문자열(UE도 이 출력을 해석한다, `Developer/Apple/MetalShaderFormat/Private/MetalShaderFormat.cpp:475–559`)과 `metal`·`metallib` 실행 파일의 SHA-256이다.

참고로 UE 5.6의 셰이더 job 키는 엔진을 빌드한 컴파일러·호스트를 넣지 않는다(캐시 버전 GUID, 형식 버전(DXC 버전 해시 포함), 대상, 엔트리, 컴파일 환경, 전처리 결과, 루트 파라미터 레이아웃 서명, `Runtime/RenderCore/Private/ShaderCore.cpp:4144–4199`, `Developer/Windows/ShaderFormatD3D/Private/ShaderFormatD3D.cpp:50–71`). Rocks는 이 점에서 UE와 다르게 공통 규칙을 우선한다.

**값과 결정성.** AssetSystem §10 결정성 규칙(같은 도구 빌드에서 호스트 무관하게 같은 바이트)을 셰이더에도 적용한다.

- 값은 바이트코드, 정규화된 바인딩 layout, 진단, 디버그 심볼을 부분으로 나눈다(기존 `.rnug` 부분 구조).
- 경로 누출 금지: 입력은 메모리 버퍼와 가상 이름으로 넘기고 `#line`에는 가상 경로만 둔다. 디버그 정보는 별도 부분으로 빼고 Apple 도구에는 prefix map을 쓴다. "작업 디렉터리를 바꿔도 같은 바이트" 시험을 둔다.
- SPIRV-Cross의 실수 출력(`%.32g`)에서 locale 소수점은 SPIRV-Cross가 보정하므로(`spirv_common.hpp:250–291`, `fixup_radix_point`) CRT `%g` 구현 차이만 남는다. 같은 호스트 키 안에서도 OS의 CRT 버전은 다를 수 있다. 워커를 C locale로 고정하고, 극값(비정규수·최대값·17자리 이상 필요한 값) fixture로 출력이 같은지 시험한다. 다르면 추적 가능한 SPIRV-Cross patch(6절 3항)로 고정한다.
- **상수 접기.** DXC(LLVM)와 SPIRV-Tools 최적화기가 초월 함수 상수식을 호스트 libm으로 접으면, 같은 호스트 키 안에서도 CPU(예: UCRT의 FMA3 분기, [핵심 데이터 기반](CoreDataFoundations.ko.md))에 따라 바이트가 다를 수 있다. SH0에서 상수 초월 함수 fixture를 FMA3 유무와 OS 버전을 바꿔 비교한다. 다르면 결정성 규칙을 지킬 방법(도구 빌드 patch, 해당 최적화 끄기 등)을 정하기 전에는 그 형식을 공유 캐시에 올리지 않고, 방법은 사용자 결정으로 올린다.
- **dxil 서명.** UE는 `dxil.dll`이 없으면 서명되지 않은 DXIL이 조용히 나온다고 적는다(`DXCWrapper.cpp:105–107`). 이는 UE가 동봉한 구버전 DXC의 동작이다. DXC 1.8.2505부터는 외부 `dxil.dll`을 찾지 않고 항상 내장 validator가 서명한다(DXC `docs/ReleaseNotes.md`, 1.8.2502에서 validator hash 공개). Rocks는 DXC를 1.8.2505 이상으로 고정하고 `dxil.dll`을 복사하지 않는다. 결과 DXIL container 헤더의 hash digest가 0이거나 preview용 우회 값(`PreviewByPassHash`, 0x02×16, `include/dxc/DxilContainer/DxilContainer.h:39–40`)이면 job을 실패시킨다. macOS DXC에서도 서명되는지 SH0에서 실측하고, 서명되지 않으면 `D3D12_SM6`의 생산 호스트를 형식 정의에서 Windows로 고정한다.

완료 기준: A3 시험(적중·무효화·손상·동시 기록·GC)에 더해 다음을 시험한다.
- job 키의 입력별 무효화: 옵션, 형식 정의, 외부 도구 SHA, 툴체인 지문, 결정적 수학 버전, 레이아웃 서명, 프로파일 출력 설정을 하나씩 바꾼다.
- 만들 수 없는 형식의 `CapabilityUnavailable`(macOS의 DXBC).
- 실수 출력·상수 접기 fixture.

## 9. P6 형식 정의·TargetProfile·쿠킹 매니페스트

- **형식 정의.** `Engine/Config/ShaderFormats/<id>.json`에 둔다: 언어 버전 기본값, 컴파일러(DXC·FXC·SPIRV-Cross 경로), 셰이더 모델·target env, 바인딩 규약 버전, ShaderFormat 처리기 버전, 상한(5.2절 4항)·지원 기능, 생산 호스트 규칙. UE에서는 기능·형식 이름 부분이 `DataDrivenPlatformInfo.ini`의 `[ShaderPlatform]`(`Language`·`MaxFeatureLevel`·`ShaderFormat`·기능 플래그, `Engine/Config/Windows/DataDrivenPlatformInfo.ini:78–136`)에 있고, SM 선택은 ShaderFormat 코드에 있다(`ShaderFormatD3D.cpp:146–164`, FXC·DXC 분기는 `D3DShaderCompiler.cpp`). Rocks는 둘 다 형식 정의에 둔다. 형식 정의 SHA-256이 job 키에 들어간다(8절 3항).
- **TargetProfile**(AssetSystem 10절)은 backend와 형식 id 목록만 갖는다. 언어 버전·target env·생산 호스트·장치 요구를 프로필에 두면 출처가 둘이 되기 때문이다. 각 `graphicsBackends` 항목은 그 backend의 형식을 `shaderFormats`에 정확히 하나 가져야 하며, 짝이 없거나 남는 형식은 프로필 검증 오류다.

```json
"graphicsBackends": ["D3D12", "D3D11", "Vulkan"],
"shaderFormats": ["D3D12_SM6", "D3D11_SM5", "VULKAN_SM5"]
```

- 엔진 기본 프로필에 `MacOS`(`graphicsBackends: ["Metal"]`, `shaderFormats: ["METAL_SM5"]`)를 더한다. 현재 엔진 기본은 `Windows`·`Android`뿐이다(AssetSystem 10절).
- **쿠킹 산출물.** (프로필, 형식)마다 셰이더 라이브러리 파일 하나를 만든다. UE는 라이브러리(Global·프로젝트·플러그인/청크)×형식마다 `ShaderArchive-<Library>-<Format>-<Platform>.ushaderbytecode`를 쿠킹하고(파일 이름 `Runtime/RenderCore/Private/ShaderCodeLibrary.cpp:354–357`, `<Format>-<Platform>` 문자열 `:3378–3383`), 배포는 IoStore나 네이티브 라이브러리로 연다(ushaderbytecode는 배포 형식이 아니라는 주석, `:950–963`). Rocks도 라이브러리 파일은 쿠킹 단위이고, 배포 컨테이너로 옮기는 일은 패키징 단계가 맡는다. 라이브러리 분할(플러그인·청크)은 후속이다.
- 쿠킹 매니페스트 v1은 AssetId 단위라서 v2에 "에셋이 아닌 산출물" 항목(이름, 형식 id, 파일 SHA-256·크기, cookKey)을 더한다. 라이브러리 cookKey 입력은 포함된 job 키 정렬 목록과 라이브러리 형식 버전이다.
- 런타임은 실행 장치의 backend에 맞는 형식의 라이브러리만 연다. 없거나 필요한 셰이더가 빠졌으면 `IncompatibleCook`이다.

완료 기준: 형식 정의·프로필 스키마 검증(모르는 속성 거부, 프로필에 형식 옵션을 적으면 오류, backend·형식 짝 검사), 프로필 공통 상한 초과의 쿠킹 오류, 매니페스트 v2 왕복, 형식이 빠진 쿠킹을 연 런타임의 `IncompatibleCook`.

## 10. P7 MetaCompiler 셰이더 파라미터

12절 결정 4의 권장안(MetaCompiler 방식)을 고를 때의 내용이다.

- **선언.** `ROCKS_SHADER_PARAMETERS(Name)` 구조체 종류를 MetaCompiler에 더한다. 파서·descriptor는 B4 `ROCKS_STRUCT`([리플렉션 기반](ReflectionFoundations.ko.md) 4절)를 바탕으로 한다. 멤버는 ShaderCore 셰이더 전용 정렬 타입(5.2절 5항)과 셰이더 자원 값 타입(텍스처·샘플러·버퍼 SRV·UAV)만 받고, 다른 타입(Core `Matrix4`·`Vector3` 포함)은 MetaCompiler 오류다.
- **identity와 등록.** 공통 규칙대로 셰이더 파라미터 구조체도 identity 명세(`<Module>.rocksids.json`)에 항목을 두고, `ROCKS_STRUCT`와 같은 경로로 StructSchema·TypeRegistry에 등록한다(StructSchema는 TypeId를 identity에서 얻는다, ReflectionFoundations 4절). 셰이더 파라미터 종류는 그 위에 허용 타입 제약과 HLSL 생성만 더한다. 셰이더 자원 멤버(텍스처·샘플러·버퍼)를 담을 리플렉션 값 종류는 B3 값 종류에 더한다.
- **생성물.**
  - C++: 멤버별 offset·크기 static_assert, `ShaderBindingLayout`과 대조할 레이아웃 표.
  - HLSL: `<out>/Generated/<Module>/Shaders/<Name>.rsh`(기존 `<out>/Generated/<M>` 게시 규칙을 따른다). 가상 경로 `/Engine/Generated/<Module>/<Name>.rsh`(플러그인·프로젝트는 각자의 `Generated`)로 연결하며, UE `/Engine/Generated/`에 해당한다. 생성 HLSL은 셰이더 맵 키의 include 폐포에 들어간다. 설치 SDK로 내보내는 경로는 후속이다.
- **레이아웃 서명.** 정규화한 레이아웃(멤버 이름·타입·offset·크기, 자원 종류·순번)의 SHA-256이다. job 키 7항(8절)이다.
- `DrawConstants`는 이 종류로 옮긴다.

완료 기준: MetaCompiler 음성 대조(금지 타입), identity 명세 검증(일반 구조체와 같은 규칙), 5.3절 시험 (a)·(f)를 생성 구조체로 통과.

## 11. P8 계획 문서 개정

이 문서 확정과 함께 아래를 고친다. 다른 세션이 계획 문서를 계속 고치므로 위치는 절 번호와 인용 문구로 적는다(줄 번호는 2026-11-13 기준 참고값).

| 문서 | 위치 | 현재 | 개정 |
|---|---|---|---|
| EngineImplementationPlan | 서문(:27) | "D3D11/D3D12/Vulkan/Metal/OpenGL 지원" | OpenGL 삭제 |
| 〃 | §2.2 그래픽 행(:122) | D3D11/D3D12/Vulkan/Metal/OpenGL, 공통 material IR과 backend별 shader toolchain | D3D11/D3D12/Vulkan/Metal. HLSL 단일 원문(Material IR은 HLSL을 만드는 앞단) |
| 〃 | §2.5(:185–193) | "5개 RHI", D3D11 "HLSL→FXC/D3DCompile→DXBC", OpenGL "IR→GLSL" 행 | 4개 backend, 2절 표의 경로. OpenGL 행 삭제(Android GLES는 후속 선택) |
| 〃 | §2.5(:197) | Windows용 Metal 제작 도구가 있으므로 Metal cook을 macOS 전용으로 단정하지 않는다 | 유지한다(12절 결정 5와 같은 방향) |
| 〃 | §2.5(:203·:208), §11.5(:1101·:1121·:1128), §14.10(:1376·:1381 LT02) | "5개 backend"·"5개 RHI"·"5-backend" | "4개" |
| 〃 | §3.1(:234), §3.2(:262), §14.6(:1276), §14.9(:1341), §27.7(:2287) | ShaderWorker | ShaderCompileWorker(§14.6의 의존 대상은 `Developer/ShaderCompiler` 라이브러리) |
| 〃 | §3.2(:255) | `RHI/{Null,D3D11,D3D12,Vulkan,Metal,OpenGL}` | OpenGL 삭제 |
| 〃 | §3.2(:259) | `Engine/Source/ThirdParty/ Adapters` | 외부 라이브러리 원본·manifest·adapter(6절 1항) |
| 〃 | §8.4(:723) | "D3D11/OpenGL의 제한" | "D3D11의 제한" |
| 〃 | §10.4(:931) | CPU 대체 경로 대상 "(mobile·D3D11·OpenGL)" | "(mobile·D3D11)" |
| 〃 | §11.5(:1109·:1115·:1117) | OpenGL 행, "D3D11/OpenGL은 graphics owner lane", GL clip 규약 | OpenGL 행·서술 삭제 |
| 〃 | §14.1(:1212) | DXBC·DXIL·SPIR-V·MSL·GLSL 경로 | GLSL 삭제(Android GLES는 후속) |
| 〃 | §14.7(:1319) | OpenGL의 target GLSL compile/link와 driver pipeline compilation | 삭제 |
| 〃 | §2.1(:114) | ThirdParty의 예외·RTTI 옵션은 별도 빌드, 예외가 엔진 경계를 넘지 않게 변환 | 유지한다(6절 3항의 오프라인 도구 빌드가 이 규칙의 사례) |
| CoreObjectInputDetailedPlan | :1299 | 5개 graphics API, D3D11/D3D12/Vulkan/OpenGL/Metal | 4개 |
| EngineRuntime | :3·:14·:38·:39·:190·:205–208·:213·:225·:235·:243 | 범위 "D3D12·OpenGL로 화면에", `OpenGLGraphics`, Vulkan·Metal "자리만", `Backend{D3D12, OpenGL, Vulkan, Metal}`, 런타임 `D3DCompile`·GLSL 4.50, "다른 backend 구현은 범위 밖", `--graphics=d3d12\|opengl`, OpenGL self-test, "Vulkan은 미구현" | OpenGL은 SH2까지의 임시 검증 backend로 표시하고, 셰이더 원문 규칙을 이 문서 참조로 바꾼다. SH2에서 VulkanGraphics 범위를 넣고 `Backend`에서 OpenGL 삭제, SH5에서 D3D11 추가(`DeviceTypes.h:16`) |
| AssetSystem | §10 TargetProfile(:296)·cookKey(:299), A0-G(:15) | 기본 프로필 Windows·Android, 그래픽 backend 단일 필드 | 9절 `graphicsBackends`·`shaderFormats`·`MacOS` 프로필. 키 공통 규칙(§9)과 결정성 규칙(:311)은 그대로 셰이더에 적용한다(8절). A0-G의 OpenGL swizzle은 그대로 두고 SH2의 OpenGL 제거 때 함께 지운다 |
| ReflectionFoundations | 3절 값 종류, 4절 StructSchema | — | 셰이더 파라미터 종류가 `ROCKS_STRUCT` 경로를 쓰고 셰이더 자원 값 종류를 더한다(10절, 결정 4를 고를 때) |
| RepositoryLayout | 트리(:14·:29)·:52–56 | Engine에 Shaders·Config 없음, ThirdParty는 Core만 원본 | 3절 구조와 6절 루트 추가 |
| CompilerDiagnostics | :29(`Core/Source/ThirdParty/...` 경로) | Core 루트만 | 두 루트, 오프라인 도구 빌드는 Rocks 경고 규칙 밖이고 공개 헤더만 경고 0 확인. 셰이더 경고 절 추가: `-WX`는 작성자 원문에만(UE `ShaderConductorContext.cpp:405–413`), SPIRV-Cross가 다시 쓴 중간 원문은 제외 |
| Compression | 3.1·3.3·3.5 | Core 루트, `.c` 번역 단위만 | Engine 루트와 `kind: binary` 언급(규칙 자체는 그대로) |
| 정책 검사(코드) | `Programs/Rocks.Tooling.Checks/PolicyChecks.cs` `PythonFreeMainPath` | `Programs`·`Build`·`.github`의 Python 호출 금지 | 오프라인 도구 빌드 workflow와 `Engine/Source/ThirdParty/*/Build/`를 명시 제외(6절 3항, P3 구현 때) |
| 코드(SH2 때) | `Engine/Source/Runtime/Graphics/Graphics.Build.cs:4`, `Programs/Sandbox/Sandbox.Build.cs:4·14` | OpenGLGraphics 의존·설명 | VulkanGraphics로 |

## 12. 결정 필요 사항

1. **확장자** — **결정됨(2026-11-13): `.rsf`(엔트리)·`.rsh`(include).** UE `.usf`/`.ush`의 역할 분리를 따르고 Rocks 명명(`.rasset`·`.rpkg`·`.rnug`)에 맞춘다. C++·HLSL 공용 헤더는 `.h`다(3절). 고려했던 대안은 `.hlsl`/`.hlsli`였다.
2. **HLSL 2021 범위** — UE는 플랫폼 값 `EnablesHLSL2021ByDefault`(0 끔, 1 Global 셰이더 전부, 2 모든 셰이더, `Runtime/RHI/Public/DataDrivenShaderPlatformInfo.h:117`. PCD3D_SM6·VULKAN_SM5·VULKAN_SM6·VULKAN_SM5_ANDROID·METAL_SM5·METAL_SM6은 1, PCD3D_SM5는 0. `Engine/Config/Windows/DataDrivenPlatformInfo.ini:127`, `Engine/Config/VulkanPC/DataDrivenPlatformInfo.ini:130·252`, `Engine/Config/Android/DataDrivenPlatformInfo.ini:125`, `Engine/Config/Mac/DataDrivenPlatformInfo.ini:52·91`)에 레이 트레이싱 셰이더 강제(`Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp:3290–3299`)와 타입별 선택을 더한다. 권장: 형식 정의의 기본값 + 셰이더 타입 선택. `D3D11_SM5`에서 2021 셰이더는 SPIRV-Cross 경로를 탄다.
3. **D3D12 셰이더 모델** — 6.6(UE와 같음) 또는 6.0~6.2 + bindless 프로필 6.6. 6.6은 Windows 10에서 Agility SDK가 Runtime 의존이 된다(7절). SH0에서 WARP 지원과 Windows 10 요건을 실측한 뒤 정한다.
4. **파라미터 선언** — MetaCompiler 셰이더 파라미터 종류(권장, 10절, 리플렉션 단일 출처·생성 HLSL을 디스크에 둠) 또는 UE 매크로 방식.
5. **형식별 생산 호스트** — 키가 호스트별이므로(8절) DXIL·SPIR-V는 각 호스트가 직접 만든다. 한 호스트만 만들 수 있는 형식: DXBC는 Windows. metallib은 macOS(Xcode Metal 도구)를 기준으로 하고, Windows에도 Apple Metal Developer Tools(UE 5.6이 지원, `MetalShaderFormat.cpp:605–608, 925–945`)가 있으면 만들게 할지 정한다(권장: 허용, 형식 정의의 생산 호스트 규칙). 만들 수 없는 호스트에서는 `CapabilityUnavailable`이고, 원격 워커가 생기면 그쪽으로 보낸다.
6. **전처리기** — Rocks 자체 구현을 `Developer/ShaderCompiler`에 둔다(권장, UE처럼 메인 프로세스에서 실행. UE 전처리기 stb_preprocess는 Epic 저작권 원본이라 가져올 수 없다, `Developer/ShaderPreprocessor/Private/stb_preprocess/preprocessor.h:1`) 또는 라이선스를 확인한 외부 C 전처리기를 P3 C 원본 규칙으로 들인다. DXC `-P`는 메인 프로세스가 DXC를 링크하게 되어(6절 2항 의존 규칙 위반) 쓰지 않는다.
7. **경고 정책** — 해당 없음. 결정 8로 C++ 외부 원본을 엔진 빌드에 넣지 않으므로 경고 억제 예외가 필요 없다.
8. **도구 공급 방식** — **결정됨(2026-11-13): UE 방식.** DXC·SPIRV-Cross·SPIRV-Tools를 엔진 빌드 밖 스크립트로 직접 빌드해 바이너리로 배포하고, 엔진 빌드는 링크·복사만 한다(6절 3항). 조사 근거: upstream DXC는 macOS 빌드를 공식 CI로 돌리고(`azure-pipelines.yml:134, 164–179`), SPIRV-Cross는 C API 공유 라이브러리를 CMake 옵션으로 만든다(`CMakeLists.txt:40, 358`). 고려했던 대안은 LunarG Vulkan SDK 바이너리였다(SPIRV-Cross C API 공유 라이브러리 포함 여부 미확인, 설치기라 다시 묶어야 함, 패치 불가).
9. **SH2 범위** — **결정됨(2026-11-13):** Graphics API를 컴파일된 코드로 바꾸는 변경이 D3D12·Vulkan self-test를 함께 통과할 때 들어가고, 같은 변경에서 OpenGL을 지운다. 두 backend 대조에 공백이 없다.
10. **셰이더 키의 툴체인 지문** — **결정됨(2026-11-13): 공통 규칙 준수.** 셰이더 job 키에도 AssetSystem 9절 공통 항목(툴체인 지문·결정적 수학 버전·대상 프로파일 id와 출력 설정 전체 포함)을 그대로 넣는다(8절). UE 5.6은 셰이더 job 키에 엔진을 빌드한 컴파일러·호스트를 넣지 않지만(`Runtime/RenderCore/Private/ShaderCore.cpp:4144–4199`), Rocks는 공통 규칙을 우선한다. 비용은 호스트 사이 셰이더 캐시 비공유와, 프로파일 출력 설정이 바뀔 때의 재컴파일이다.

## 13. 이후 단계(선행 작업 뒤)

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| SH0 | 도구 스파이크(P3·P4 산출물): Sprite 셰이더 하나를 DXIL·SPIR-V·DXBC·MSL 텍스트로 만들고 검증 도구를 돌린다. Windows·macOS 양쪽에서 실행 | 형식별 산출, `spirv-val` 통과, DXIL 서명 hash 검사(macOS DXC 포함), FXC 고정본 로드, 리플렉션 덤프, 같은 호스트 키 안의 실수 출력·상수 접기 예비 시험 |
| SH1 | 소스 트리·가상 경로·`Platform.rsh`·전처리기(결정 6)·ShaderCompileWorker(P1)·두 층 키·에셋 너겟·`shader-compile` Commandlet·Development 필요 시 컴파일 | 같은 입력 같은 바이트, 빈/찬 캐시 동일, 워커 강제 종료 보고 |
| SH2 | 리플렉션 정규화·워커 규칙 검사(5.2절)·Graphics API(`ShaderCodeKind`·`CreateShader`·파이프라인 layout·root signature)·GlobalShader·퍼뮤테이션, VulkanGraphics 최소 구현(offscreen·Sprite, P9), OpenGLGraphics·Renderer 원문·런타임 `D3DCompile` 제거(결정 9) | Sandbox self-test에서 D3D12·Vulkan이 허용 오차 안 일치(Vulkan은 CI에서 NOT RUN일 수 있다), 행렬 옵션 기록, Release에 컴파일러 없음, 5.3절 SH2 시험 |
| SH3 | 파라미터 구조체(P7)·생성 HLSL·오프셋 시험 | 5.3절 (a)·(f)를 생성 구조체로 |
| SH4 | 감시 재컴파일·오류 위치·디버그 심볼·PSO 교체 | 저장 후 10초 안 반영(계획 I5) |
| SH5 | D3D11Graphics → MetalGraphics | 각 backend가 D3D12와 허용 오차 안 일치, 5.3절 (c)·(e)·(g)의 D3D11·Metal 부분 |

## 14. 개정 이력

- 개정 1(2026-11-13): 초안.
- 개정 2(2026-11-13): 독립 검토 2건 반영.
  - UE 사실 확인: 약 60개 주장 중 14건을 고쳤다(모바일 형식 이름, D3D11 DXC 경로 근거, Vulkan bindless·SM5/SM6 구분, 예약 space와 register 명시, QBSS 양자화, Metal UAV 배정, `-fvk-use-dx-position-w`, 루트 상수 4 DWORD, DXCWrapper Windows 한정, `[ShaderPlatform]` 범위, ShaderArchive 파일·배포, HLSL 2021 기본값, `GLSL_430_REMOVED`).
  - 반례·정합성: OpenGL과 SH2의 모순 → Vulkan을 SH2로 앞당김. D3D12 64 DWORD 예산. 상수 버퍼 반례 → 허용 타입 제한. macOS 셰이더 생산 호스트 부재 → macOS DXC 자체 빌드와 Windows Metal 도구. DXC location 선언 순서 → 재작성과 `spirv-val` 필수. 공통 상한. SPIRV-Cross 예외 → 공유 라이브러리 격리. 경고 예외 삭제. 바이너리 manifest. FXC 지문. job 키 정의. 의존 관계. P6 출처 일원화와 MacOS 프로필. 개정 대상 누락. 교차 참조·용어.
- 개정 3(2026-11-13): 개정 2의 재검토 2건 반영.
  - 사실 9건: QBSS tier 기준, 구조화 SRV 근거, UE location 재배정 범위, UAV 상한의 파이프라인 단위, Metal 인덱스 상한, HLSL 2021 기본값 목록, DXC 1.8.2505 내장 validator, 근거 줄 번호, SPIRV-Cross locale 보정.
  - 반례·정합성 17건: 공유 라이브러리 모듈 종류를 P4에(차단), SPIRV-Tools 생성 파일·DXC 빌드의 Python과 버전 정합, 상한 표를 descriptor 종류로·UAV는 CS만, Metal buffer 배정·예산, D3D signature 연결 규칙·SV 허용 목록·vertex attribute remap, 워커 오프셋 검사·셰이더 전용 정렬 타입, job 키(모든 호스트 SHA·빌드 툴체인·SPIRV-Reflect·Apple 도구 manifest·공유 등급 기록), 상수 접기 결정성, 상한 검사를 프로필 단위로, 시험 시기를 SH2로, P9 Vulkan 선행, macOS DXIL 서명 실측, 텍스처·샘플러 짝, 저장 위치·CI 캐시·FXC 출처·header, P7 세부, 개정 대상 추가, 내부 정합성.
  - 사용자 결정으로 올림: 도구 공급 방식(결정 8), SH2 범위(결정 9), 셰이더 키의 툴체인 지문(결정 10). 전처리기 표현을 결정 6으로 통일.
- 개정 4(2026-11-13): 사용자 결정 반영.
  - 결정 8(도구 공급)을 UE 방식으로 정했다: 엔진 빌드 밖 스크립트로 DXC·SPIRV-Cross·SPIRV-Tools를 빌드해 바이너리로 배포하고 엔진은 링크·복사만 한다(UE ShaderConductor 빌드 스크립트·External 모듈 확인). 이에 따라 공유 라이브러리 모듈 종류, hostCode 종류, C++17·예외 빌드 설정, SPIRV-Tools 생성 파일 커밋, 경고 정책 결정(7)이 필요 없어졌다.
  - 결정 9(SH2에서 Vulkan 추가·OpenGL 제거를 한 변경으로)를 확정했다.
  - 결정 10에 UE 5.6 셰이더 job 키 구성(호스트·엔진 컴파일러 미포함)과 예외 없이 갈 때의 결과를 적었다.
- 개정 5(2026-11-13): 결정 10을 공통 규칙 준수로 확정했다.
  - 셰이더 job 키에 AssetSystem 9절 공통 항목을 모두 넣는다: 툴체인 지문, 결정적 수학 버전, 대상 프로파일 id와 출력 설정 전체. 공유 등급(`portable`)·영역별 프로필 digest·호스트 사이 바이트 동일 시험을 없앴다. 셰이더 캐시는 호스트 사이에 공유하지 않는다.
  - 상수 접기 결정성은 예외로 두지 않는다. 다르게 나오면 방법을 사용자 결정으로 올린다.
  - 같은 원칙으로 P7 셰이더 파라미터의 identity 면제를 없애고 `ROCKS_STRUCT` 경로를 따르게 했다. 공통 상한 검사는 job 안으로 옮겼다.
- 개정 6(2026-11-13): 결정 1(확장자 `.rsf`·`.rsh`)을 확정했다.
