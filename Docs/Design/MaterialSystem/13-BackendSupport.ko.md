# 머티리얼 시스템(MaterialSystem) 설계

## 13. backend 지원표와 제약

형식 id는 셰이더 시스템 2절이다. 숫자 상한은 형식 정의(셰이더 시스템 12.1절 `limits`)가 정본이고, 아래는 머티리얼 쪽 영향만 적는다.

| 항목 | `D3D12_SM6` | `D3D11_SM5` | `VULKAN_SM5` | `METAL_SM5` | `GLES31_ANDROID`·`GLES31_PC` |
|---|---|---|---|---|---|
| 기능 등급 | SM6 | SM5 | SM5 | SM5 | Mobile |
| 머티리얼 텍스처 수 | (pass, 퍼뮤테이션)별 `128 − 렌더러 합계`(Renderer 7.4절, 결정 RN13·MD4) | `16 − 렌더러 합계` | `형식 상한 − 렌더러 합계`(형식 정의를 넓히는 MSR12 뒤, 그 전에는 16) | `128 − 렌더러 합계` | `16 − 렌더러 합계`. 넘으면 이 형식 쿠킹만 오류 |
| 머티리얼 자기 샘플러 수 | 샘플러 16 − 렌더러 샘플러(짝 규칙). 넘는 텍스처는 공유 샘플러(MSR13) | 같음 | 같음 | 같음(Metal 장치 상한 16) | 같음 |
| 상수 블록 | cbuffer | cbuffer | uniform buffer | buffer | uniform `vec4` 배열 에뮬레이션(타입별 배열, 7.2절 4항). uniform 벡터 수 상한 안 |
| 머티리얼 보간 | `TEXCOORDn` n ≤ 15 − pass 예약 | 같음 | 같음 | 같음 | varying 상한 − pass 예약 |
| VS·PS storage buffer 읽기 | 가능(형식 상한 4) | 가능 | 가능 | 가능 | **불가**(ES 3.2 최소 보장 0, 7.2절 4항). 이런 노드는 GLES에서 쿠킹 오류 |
| VS·PS UAV 쓰기 | 모든 형식에서 금지(`uavStages`는 CS) | 금지 | 금지 | 금지 | 금지 |
| half 정밀도 | 없음(모든 형식 float32, 7.2절 5항) | | | | 없음. 모바일 성능 영향은 측정 MM3 |
| HLSL 2021(임의 HLSL 노드) | 켬 | 끔(SPIRV-Cross 경로로 감) | 켬 | 켬 | 끔 |
| wave 연산 | 가능(형식 기능) | 불가 | 불가 | 불가 | 불가 |
| GBuffer MRT(GBufferD가 필요한 shading model) | 8, 지원 | 8, 지원 | 8(결정 RN3 장치 요구), 지원 | 8, 지원 | 4, Deferred `Unsupported`·Forward+ 지원(결정 RN4) |
| scene color·depth 읽기(반투명) | pass가 제공 | 같음 | 같음 | 같음 | pass가 제공(framebuffer fetch는 후속) |
| 여러 closure 저장(SF4) | 연구 gate 대상 | 불가 | 불가 | 불가 | 불가 |
| 가상 텍스처 | 후속 | 후속 | 후속 | 후속 | 후속 |

UE 대조:
- 셰이더당 샘플러 기본 16이고(`Engine/Source/Runtime/RHI/Private/DataDrivenShaderPlatformInfo.cpp:163`), PCD3D_SM6은 32다(`CFG/Windows/DataDrivenPlatformInfo.ini:136`). OpenGL은 16을 넘으면 컴파일 오류다(`Engine/Source/Developer/ShaderFormatOpenGL/Private/OpenGLShaderCompiler.cpp:868–877`). UE 머티리얼은 공유 샘플러로 슬롯을 아낀다(`RE/Classes/Engine/EngineTypes.h:282–293`, `RE/Private/Materials/HLSLMaterialTranslator.cpp:7364–7388`).
- UE 번역기의 ES3_1 shading model 금지 검사는 `#if 0`으로 꺼져 있다(`RE/Private/Materials/HLSLMaterialTranslator.cpp:2175–2199`). 모바일 forward에서 GBuffer scene texture·SceneColor 읽기는 오류다(`:8320–8326, 8372–8375`). Rocks는 V6 capability 검사로 같은 일을 한다.
- 모바일의 shading model 거르기는 `r.Mobile.ShadingModelsMask`로 한다(`Engine/Source/Runtime/Core/Private/HAL/ConsoleManager.cpp:4018–4023`). Rocks는 기능 등급별 지원 preset 표를 형식 정의 기능 목록에 두고 V6로 검사한다.
- UE 모바일은 Substrate도 Blendable이다(`RC/Private/RenderUtils.cpp:2231–2232`). Rocks의 closure 1개 저장과 같은 조건이다.

**D3D11(FXC) 주의.** 생성 원문이 FXC에서 실패하면 셰이더 시스템 규칙대로 SPIRV-Cross 경로로 다시 간다(셰이더 시스템 2절). 그래서 생성 원문은 HLSL 2021 문법을 쓰지 않는다(임의 HLSL 노드가 요구한 경우 제외).
