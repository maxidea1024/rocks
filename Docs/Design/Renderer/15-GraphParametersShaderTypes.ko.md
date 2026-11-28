# 렌더러(Renderer) 설계 — 15. 렌더 그래프·파라미터 구조체·셰이더 타입 사용 방식

[목차](README.ko.md)

## 15. 렌더 그래프·파라미터 구조체·셰이더 타입 사용 방식

### 15.1 렌더 그래프

- 프레임 packet 하나 = 그래프 하나(RenderBase 7.3절). view마다 6.2절 pass를 선언 순서대로 추가한다(재배치 없음, 결정 R5).
- 장면 텍스처는 그래프 자원이고 `SceneTextureSet`을 `GraphSharedData`로 pass 사이에 넘긴다(UE blackboard 대응, RenderBase 2절).
- history(TAA·HZB·노출·path tracing 누적)는 `ExportTexture`로 `ViewHistory` 소유자에게 넘기고 다음 프레임 `ImportTexture`한다(RenderBase 7.4절).
- 스트리밍 텍스처·GPU Scene 버퍼·shadow atlas(프레임 사이 유지 시)는 import 자원이다.
- 1차는 단일 graphics queue(RG1·RG2). light grid·AO·HZB의 async compute는 RG4 뒤 선택이다(UE light grid async 선택 `R/LightGridInjection.cpp:47–52`).

### 15.2 파라미터 구조체와 공유 상수 slot

결정 R4(파라미터 구조체가 셰이더 상수·자원 바인딩과 그래프 의존을 함께 맡는다)를 그대로 쓴다. UE uniform buffer는 상수와 자원을 함께 담지만, RenderBase 9.4·9.5절의 공유 slot은 **상수 블록**이고 GLES에서는 uniform `vec4` 배열로 바뀌므로 자원을 담을 수 없다. 그래서 묶음마다 둘로 나눈다.
- **상수 부분**: 공유 상수 slot(RenderBase 9.5절)에 정적 등록한다.
- **자원 부분**: 공유 자원 구조체 `SharedResourceStruct`(RenderBase 9.6절)로 정의하고, 그 자원을 쓰는 pass의 파라미터 구조체가 포함한다. 그래프 의존도 이 멤버로 생긴다.
- **그리기마다 바뀌는 것**(`PrecomputedLighting`)은 공유 slot이 아니라 그리기별 바인딩이다: 상수 블록 하나와 그리기별 자원 구조체 `DrawResourceStruct`(RenderBase 9.3·9.6절, 개정 8). UE도 그리기마다 묶는다(`R/LightMapRendering.cpp:312–360`).

| 묶음 | 상수 부분(slot) | 자원 부분(공유 자원 구조체) | 수명 | UE 5.8 대응 |
|---|---|---|---|---|
| `View` | 행렬(현재·이전, jitter 유무), 카메라 이동 보정 상위·하위, render 크기, 시간, 노출, mip bias, 정적 간접 배율 둘 | — | `PerFrame`(view마다) | view uniform buffer(`EN/Public/SceneView.h:1157`) |
| `Scene` | 인스턴스 수 | GPU Scene typed buffer들, 인스턴스 번호 목록 | `PerFrame`(`ViewSet`마다) | GPU Scene(`SH/SceneData.ush:351`) |
| `ForwardLight` | grid 크기·Z 인자, local 광원 수, directional 목록(최대 4) | 광원 데이터·grid typed buffer | `PerFrame` | `ForwardLightStruct`(`R/SceneRendering.h:728–763`) |
| `Shadow` | CSM 행렬·분할, atlas 크기 | shadow atlas(비교 샘플러 짝) | `PerFrame` | `R/SceneRendering.h:765–779` |
| `SceneTextures` | — | 깊이·GBuffer·AO·velocity·HZB | 그래프 수명 | scene textures uniform buffer |
| `Fog` | height fog 인자 | aerial perspective LUT | `PerFrame` | `FogStruct`(`R/FogRendering.cpp:85`) |
| `PrecomputedLighting` | 정적 조명 상수(slot 아님, 그리기별 상수 블록) | 라이트맵·프로브 텍스처(그리기별 자원 구조체) | `Persistent`(그리기마다 바인딩) | `EN/Public/LightmapUniformShaderParameters.h:16`, `EN/Public/SceneManagement.h:725` |

- 공유 상수 slot은 다섯(`View`·`Scene`·`ForwardLight`·`Shadow`·`Fog`)이다. pass 셰이더 하나의 상수 버퍼는 slot 5 + `PrecomputedLighting` 1 + 머티리얼 1 + 머티리얼 파라미터 컬렉션 최대 2 + pass 1 = 최대 10이고 형식 정의 상수 버퍼 상한(12–14) 안이다(RenderBase 9.5절, 개정 8). pass마다 실제 쓰는 멤버만 바인딩 layout에 남는다(RenderBase 9.6절 미사용 멤버 거르기).
- **GLES uniform 예산.** GLES PS 에뮬레이션 상한은 `vec4` 224개다(형식 정의). 위 상수의 추정 크기는 `View` 약 40, `ForwardLight` 약 20, `Shadow` 약 20, `Fog` 약 8, `PrecomputedLighting` 약 8, `Scene` 1로 합계 약 100이다. 워커는 이 합에 머티리얼 상수·머티리얼 파라미터 컬렉션·pass 상수·루트 상수(최대 8)를 더한 단계 합을 job 형식 상한으로 검사한다(RenderBase 9.5절). 실제 크기는 구조체를 정한 뒤 확인한다(**미확인**, 추정값). 컬렉션 크기 상한은 이 합의 나머지에서 정한다(머티리얼 시스템 9.7절).
- **그리기별 값.** 인스턴스 offset은 루트 상수(`ROCKS_ROOT_CONSTANTS`, 128 byte, 셰이더 시스템 7.2절 2항)로 넘긴다. `SV_InstanceID`에 base instance가 들어가는지가 backend마다 다르므로 시스템 값에 기대지 않는다(셰이더 시스템 7.2절 7항).
- **GPU가 만드는 간접 draw(RS4).** GLES 3.1 간접 draw 명령의 base instance 자리는 0이어야 하므로 base instance로 offset을 넘길 수 없다. 1차 규칙: 그리기 항목 하나에 간접 draw 하나를 CPU가 기록하고 그 직전에 루트 상수로 인스턴스 offset을 둔다. GPU culling은 인스턴스 수와 인스턴스 index 목록만 쓴다. draw 여러 개를 한 호출로 묶는 것(D3D12 command signature의 루트 상수, Vulkan multi-draw)은 backend별 최적화로 후속이다. GLES base instance 규칙의 사양 원문 대조는 **미확인**(RQ3).

### 15.3 셰이더 타입

| 종류 | 예 | 등록 | 거르기 |
|---|---|---|---|
| 전역 셰이더(머티리얼 없음) | light grid CS, HZB CS, `DeferredLightingPass` PS, 하늘·안개 PS, tone map·TAA·bloom | `RendererShaders` 정적 등록(셰이더 시스템 12.2절) | 형식·퍼뮤테이션 |
| pass 셰이더(머티리얼 결합) | 14.3절 표 | `RendererShaders` 정적 등록, 머티리얼 쿠킹이 곱한다 | 형식 × 정점 입력 타입(SX4) × `MaterialShaderTraits`(14.3절) |

- 전역 셰이더 퍼뮤테이션 예: `DeferredLightingPass` {GBuffer profile 4} × {동적 GI 표본 0·1} × {contact shadow 0·1}, light grid CS {HZB 판정 0·1}.
- GPU Scene 읽기: 정점 셰이더가 typed buffer에서 인스턴스 변환을 읽는다(GLES VS storage block 0 때문에 모든 backend 같은 방식, 결정 RN9). UE ES3.1은 primitive UBO 경로를 따로 둔다(`SH/SceneData.ush:40–46, 1293`).
- 정점 입력 타입(RenderBase 10절)은 1차로 정적 메시 하나, skinned는 RS12다.
