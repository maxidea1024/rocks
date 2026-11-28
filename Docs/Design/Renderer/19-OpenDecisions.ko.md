# 렌더러(Renderer) 설계 — 19. 결정 필요 사항과 미확인 목록

[목차](README.ko.md)

## 19. 결정 필요 사항과 미확인 목록

### 19.1 결정 사항

| ID | 결정 | 선택지와 권장 | 기한 |
|---|---|---|---|
| RN1 | Deferred 조명 구조 | (가) 권장: 전체 화면 조명 pass 하나가 directional 목록 + light grid local 광원을 Forward+와 같은 함수로 평가, 그림자 직접 표본(7.3절). 측정 MR1로 확정. (나) UE 5.8 기본: 광원별 pass(구·원뿔 기하) + 그림자 광원별 screen mask. (다) tiled compute deferred(UE에서 제거됨, GLES image 형식 제약) | RS2 전(MR1 결과로) |
| RN2 | GBuffer 법선 인코딩 | (가) 권장: UE desktop과 같은 `N·0.5+0.5` 10:10:10, 모든 backend. (나) octahedral(UE 모바일 방식) | RS2 전 |
| RN3 | Vulkan Deferred MRT 8 | (가) 권장: `VULKAN_SM5` 장치 요구에 MRT 8을 넣고 상한을 8로(데스크톱 장치 분포는 **미확인**이므로 GX1 조회 결과를 먼저 본다). (나) MRT 8 형식 id를 따로 둔다. (다) Vulkan Deferred는 기본 layout만(D·E는 `Unsupported`). **결정됨(2026-11-18, 권장안 (가))**: GX1 측정(GTX 1060·lavapipe 모두 8)으로 `VULKAN_SM5` `renderTargets` 8(셰이더 시스템 개정 14) | RS7 전(RS2는 기본 layout으로 진행 가능) |
| RN4 | GLES Deferred 범위 | (가) 권장: 기본 layout(MRT 4)만, 다중 pass, GBufferD·E가 필요한 기능은 GLES Deferred에서 `Unsupported`(Forward+는 지원). (나) GLES Deferred를 두지 않음(계획 11.5절 "모든 backend에서 두 경로 검증"과 충돌) | RS2 전 |
| RN5 | shading model·블렌드 모드·영역 번호표 소유 | (가) 권장: 렌더러가 번호·GBuffer 의미를 소유하고 머티리얼 스키마가 참조. 개정 3: 번호표 C++ 정의는 `RenderData`, HLSL 상수는 `/Engine/Shared/` 공용 헤더, GBuffer 의미는 `RendererShaders`(3.2절, 머티리얼 시스템 10.4절 D2의 순환 해소). (나) 머티리얼 스키마가 소유하고 렌더러가 참조 | MAT0 전 |
| RN6 | velocity 형식 | (가) 권장: 모든 backend `RG16Float`. (나) UE처럼 `RG16Unorm` 인코딩(GLES는 확장 조건) | RS0 전 |
| RN7 | 화면 마일스톤 다음 순서 | (가) 권장: RS0 → RS1 Forward+ → RS2 Deferred → RS3 그림자 → …(17.2절). (나) Deferred 먼저 | RS0 착수 전 |
| RN8 | 하늘·안개 위치 | (가) 권장: 두 경로 모두 전체 화면 pass(F13), 투명은 자기 셰이더. (나) Forward+는 base pass 안에서 안개 적용 | RS5 전 |
| RN9 | GPU Scene 저장 형태 | (가) 권장: 모든 backend typed buffer(float4) 하나의 경로. MR4로 재검토. 재검토 조건에 typed buffer 원소 상한을 더한다(개정 8): 대상 기기의 `texelBufferElements`가 최소 보장 65536이면 인스턴스 약 5,000이 상한이므로(RenderBase 10.5절·결정 R16) 2D 텍스처 대체 등 저장 형태를 다시 연다(19.2절 측정). (나) 데스크톱 structured + GLES typed 두 경로 | RS0 전 |
| RN10 | point 그림자 | (가) 권장: atlas 6면 개별 그리기(모든 backend). viewport index 출력 한 번 그리기는 능력 확장 후속(셰이더 시스템 7.2절 7항, 개정 8에서 layer를 viewport로 바로잡음). (나) cube 텍스처 + layer 출력 필수(GLES 불가) | RS3 전 |
| RN11 | 모듈·이름 | 2절 이름과 3.2절 배치(`RenderData`·`RendererShaders`·`Renderer` 셋). 대안: `RendererShaders`를 두지 않고 쿠커가 `Renderer`를 링크(Host 쿠커에 장치 모듈 의존이 생긴다) | 문서 확정 |
| RN12 | Forward+ 보조 출력 배치 | 8.4절 roughness 자리(두 번째 `R8Unorm` 타깃 또는 법선 octahedral + roughness 한 타깃). 권장안은 RS10 측정 뒤 | RS10 전 |
| RN13 | 단계당 SRV 상한 대응(7.4절) | **개정 3(2026-11-18 사용자 결정): 형식별 상한.** 셰이더 원문은 모든 형식에 하나로 유지하고 자원 합치기(grid 버퍼 하나·shadow atlas 하나·atlas 묶기)로 렌더러 자원을 16 안에 둔다. 단계당 SRV 상한은 형식 정의가 정한다(D3D12·Vulkan·Metal 넓게, GLES·D3D11 SM5 16). 머티리얼 텍스처 상한은 (pass, 퍼뮤테이션, 형식)별 `형식 상한 − 렌더러 합계`이고, 모바일 형식에서 넘는 머티리얼은 그 형식 쿠킹의 명시 오류다(RQ22). 넓은 형식이 16장을 넘으려면 공유 샘플러가 필요하다(RQ23). 개정 2의 확정안은 (가) 모든 형식 `16 − 렌더러 합계`였다. 그 밖의 선택지: (나) 16-SRV 형식용 pass 셰이더 타입을 따로 둠(셰이더 타입 수 증가), (다) bindless를 앞당김 | RS1 전 |

**결정(2026-11-18, 사용자 "결정사항들은 권장안대로 하면됨").** RN1–RN11·RN13은 각 행의 권장안으로 확정한다. RN12는 권장안이 정해지지 않은 항목이라 RS10에서 정한다. RN5는 머티리얼 시스템과의 교차 검증에서 번호표 코드 위치를 정했다(개정 3).

**개정(2026-11-18, 사용자 결정).** RN13을 권장안 (가)에서 형식별 상한으로 개정했다(위 행). 이유: (가)는 GLES 보장치 16이 데스크톱 형식까지 묶어 일반 PBR 머티리얼도 무거운 퍼뮤테이션에서 쿠킹 오류가 날 수 있었다(머티리얼 시스템 10.4절 D3). 데스크톱은 쿠킹되고 모바일 형식에서만 명시 오류가 나는 구조는 UE 모바일 머티리얼 제약과 같은 성격이다.

### 19.2 미확인 목록

| 항목 | 위치 | 확인 방법 |
|---|---|---|
| UE가 정지 물체 velocity를 깊이·이전 view 행렬로 복원하는지 | 7.1절 | `SH/` TAA·velocity 원문 조사 |
| UE grid 기반 deferred 제거 사유가 성능인지 | 7.3절 | 원문 주석 외 근거 없음. MR1로 Rocks 판단 |
| UE forward에서 그림자 채널(4개)을 받지 못한 광원의 동작 | 8.3절 | `R/DynamicShadowMapChannelBindingHelper.cpp` 조사 |
| UE forward에서 sky atmosphere 전체 화면 pass 대신 하늘을 그리는 주체 | 10.2절 | `SH/BasePassPixelShader.usf`·sky 머티리얼 조사 |
| GLES에서 깊이를 읽기 전용 attachment로 붙인 채 샘플하는 것이 feedback loop인지(쓰기를 끈 경우 포함) | 8.2절, RQ6 | ES 3.2 사양 "rendering feedback loop" 절 대조. 확인 전에는 깊이 복사로 설계 |
| GLES 간접 draw base instance 0 규칙 | 15.2절 | ES 3.1·3.2 사양 `DrawElementsIndirectCommand` 대조 |
| 공유 상수의 GLES `vec4` 크기 | 15.2절 | 구조체 확정 뒤 워커 검사 |
| 기기별 typed buffer 원소 상한(`GL_MAX_TEXTURE_BUFFER_SIZE`·`maxTexelBufferElements`) | 8.1절, 결정 RN9, RenderBase 10.5절 | GX1 조회(`DeviceCaps.limits.texelBufferElements`)를 GX1 측정 표에 더하고 Android 대상 기기 표본을 잰다 |
| GLES의 on-tile MSAA 색 resolve 수단 | 8.2절, RenderBase 결정 R17 | RS8 측정(확장 유무) |
| UE path tracing에서 depth prepass·velocity 유지 여부 | 12.3절 | `DSR` path tracing 분기 전후 조사 |
| GLES texel buffer 형식(`R32Uint`·`RGBA32Float`) | 8.1절, RQ9 | **확인(2026-11-18)**: Table 8.18(Khronos glTexBuffer ES3 refpage)에 둘 다 있고 image 형식이라 typed SRV·UAV 모두 된다. GTX 1060 ES context에서 SRV 생성·VS·PS 읽기 픽셀 대조([PixelFormat](../PixelFormat/14-ImplementationStages.ko.md) 14.3절) |
| SPIRV-Cross의 HLSL `Buffer<T>` → ESSL `samplerBuffer` 변환 | RQ10 | SH0 측정 |
| 데스크톱 Vulkan 장치의 `maxColorAttachments` 분포 | 결정 RN3 | GX1 조회·장치 표본. 측정(2026-11-18): GTX 1060·lavapipe 모두 8(RenderBase 4.4절 GX1 표). 장치 표본을 넓히는 것은 남음 |
| 데스크톱 Vulkan 장치의 `maxPerStageDescriptorSampledImages`·`maxPerStageDescriptorSamplers` 분포 | 결정 RN13, RQ22 | GX1 조회·장치 표본. 측정(2026-11-18): GTX 1060 단계당 1048576·1048576, set 1048576, `maxPerStageResources` 4294967295 / lavapipe 모두 1015808(RenderBase 4.4절 GX1 표). 장치 표본을 넓히는 것은 남음 |
| D3D12 resource binding tier 1 장치가 `D3D12_SHADER_MODEL_6_6` 요구를 통과하는지(샘플러 상한 16 여부) | RQ23 | 장치 표본. 개발 기계 GTX 1060은 tier 3(셰이더 시스템 15절 결정 3 측정) |
| Metal MRT·layer 출력·깊이 clamp·HDR 능력 | 13.1절 | SH5 착수 때 Apple 능력 표 |
| GLES 깊이 clamp 확장 | 13.1절 | 확장 목록 조사(P13 기기) |
| `r.Mobile.ShadingPath` 등록 위치 | 3.1절(모바일) | 렌더러 범위 밖이라 조사 생략. 설정 읽기만 확인(`RC/Private/ReadOnlyCVARCache.cpp:53–69`) |
