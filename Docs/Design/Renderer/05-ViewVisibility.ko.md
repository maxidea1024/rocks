# 렌더러(Renderer) 설계 — 5. view와 visibility

[목차](README.ko.md)

## 5. view와 visibility

### 5.1 view

- `RenderView`는 `ViewDesc`(계획 11.1절: 경로·render 크기·출력 크기·sample 수·upscale·카메라)에서 만든다. 행렬은 1절 좌표 규약을 따른다: view space +X 오른쪽·+Y 위·+Z 전방, reversed-Z 무한 원평면 원근(수평 반각), 깊이 [0,1]·가까움 1. 그래서 깊이 clear 값은 0, 깊이 비교는 `GreaterEqual`, base pass(prepass 뒤)는 `Equal`이다.
- jitter는 투영 행렬에 넣고 jitter 없는 행렬을 따로 둔다(계획 12.2절). 이전 프레임 행렬도 jitter 없는 것과 있는 것을 모두 보존한다.
- **큰 월드.** 셰이더 계산은 카메라 상대 float이다(계획 2.2절). 그러나 GPU Scene에 view 원점을 빼서 올리면 view마다 GPU Scene이 따로 필요하고 카메라가 움직일 때마다 전체를 다시 올려야 하므로(4.2절 "변경 범위만 upload"와 충돌) 그렇게 하지 않는다. UE처럼 GPU Scene에는 view와 무관한 world 위치를 상위·하위 두 부분(상위 = 큰 격자 위치 float3, 하위 = 상대 local-to-world float)으로 두고, view 상수에 카메라 이동 보정값(상위·하위)을 둔다. 셰이더는 `(인스턴스 상위 − view 상위) + 상대 변환`으로 카메라 상대 행렬을 만든다. 이전 프레임 변환도 같은 형태로 둔다. UE 근거: GPU Scene의 double-float 위치(`SH/SceneData.ush:153–155`), view의 이동 보정(`EN/Public/SceneView.h:382–401`). 격자 크기·정밀도 시험은 RS0 완료 기준 (e)에 넣는다.
- **view 상수**는 공유 상수 slot(RenderBase 9.5절) `View`로 묶는다. UE view uniform buffer(`EN/Public/SceneView.h:1157`, 생성 `R/SceneRendering.cpp:2397–2432`)에 해당한다.
- **`ViewHistory`**: 이전 HZB, TAA history, 노출 값, occlusion 이력, path tracing 누적. 키는 계획 11.4절대로 (SessionId·WorldEpoch, ViewId+generation, 경로, 크기, sample 수, jitter 수열)다. 경로·크기·sample 수가 바뀌면 history를 버린다.
- **`ViewSet`**: 같은 장면 snapshot을 보는 view들이다. 장면 갱신·GPU Scene·그림자(같은 light/caster revision·projection·coverage·filter 키일 때만, 계획 11.4절)는 한 번 하고 view 크기 타깃·history는 view마다 따로 둔다.

### 5.2 UE 5.8 가시성

| 단계 | UE 5.8 | 근거 |
|---|---|---|
| frustum | 장면 SoA bounds 위 병렬 task(구 판정 뒤 상자-평면 8개). octree는 설정 | `R/SceneVisibility.cpp:603, 860, 3967–3974, 4043, 343, 730` |
| 거리 | 최소·최대 그리기 거리와 fade | `R/SceneVisibility.cpp:1011–1029` |
| 사전 계산 가시성 | 정적 표 조회 | `R/SceneVisibility.cpp:3532, 4494` |
| hardware occlusion query | 이전 프레임 결과를 읽는다(버퍼 1프레임) | `R/SceneVisibility.cpp:2824–2861`, `Core/Private/HAL/ConsoleManager.cpp:4259–4260` |
| HZB | prepass 깊이로 furthest·closest를 만들고 furthest를 다음 프레임용으로 뺀다. 크기는 view 크기를 2의 거듭제곱으로 올린 뒤 절반 | `R/SceneTextureReductions.cpp:111–161`, `DSR:561–610` |
| HZB 위치 | 조기 깊이가 완전하면 base pass 전, 아니면 뒤 | `DSR:2781–2786, 3137–3140` |
| GPU 인스턴스 culling | 이전 프레임 HZB로 판정(occlusion은 기본 끔) | `R/InstanceCulling/InstanceCullingContext.cpp:28–38, 853–858`, `SH/InstanceCulling/BuildInstanceDrawCommands.usf:203–215` |

UE의 occlusion 판정은 전부 이전 프레임 정보에 기댄다. 계획 11.4절은 "이전 HZB에서 탈락한 집합을 최종 폐기하지 않는다"를 요구하므로 Rocks는 아래 2단계 구조를 쓴다. 이 구조는 UE 대응이 없는 자체 설계다(계획 11.4절과 같이 표시).

### 5.3 Rocks 가시성

| 단계 | 내용 | 실행 | 단계(17절) |
|---|---|---|---|
| V1 frustum·거리 | `RenderScene` SoA bounds를 CP2 `ParallelFor`로 판정. 공간 인덱스(SpatialIndex)는 인스턴스 수가 많을 때 후보 축소에만 쓴다 | CPU | RS0 |
| V2 1차 그리기 | 이전 HZB에서 보이는 집합만 depth prepass로 그린다. 이전 HZB에서 가려진 집합은 **보류 집합**으로 남긴다 | GPU(1차는 CPU 판정) | RS4 |
| V3 현재 HZB | 1차 깊이로 HZB(furthest)를 만든다 | GPU compute | RS4 |
| V4 재검사 | 보류 집합을 현재 HZB로 다시 판정해 보이는 것을 같은 프레임 depth prepass에 추가로 그린다. 그 뒤 HZB를 갱신한다 | GPU | RS4 |
| V5 그림자 view | 광원 view마다 별도 frustum 판정(계획 11.4절). HZB는 쓰지 않는다(1차) | CPU | RS3 |

- 보수 규칙: 이전 HZB가 없거나(첫 프레임·cut·teleport·view 크기 변경) 무효이면 V2는 전부 그린다. "가시 물체 누락 0"(계획 13.1절 V_ref ⊆ V_submit)이 기준이다.
- RS0–RS3에서는 V1만 한다. HZB는 RS0에서 이미 만들되(light grid HZB 판정·화면 효과용) 인스턴스 판정에 쓰는 것은 RS4다.
- UE light grid는 furthest HZB가 유효하면 셀 판정에 쓴다(`R/LightGridInjection.cpp:996–1011`). Rocks도 V3의 HZB를 light grid에 쓴다.
