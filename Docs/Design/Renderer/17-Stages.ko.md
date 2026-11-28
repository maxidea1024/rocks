# 렌더러(Renderer) 설계 — 17. 단계와 완료 기준

[목차](README.ko.md)

## 17. 단계와 완료 기준

### 17.1 선행

| 선행 | 문서 | 필요한 것 |
|---|---|---|
| 화면 마일스톤 전체 | RenderBase 18.1절 | SH0–SH4, GX1–GX11, RB1–RB7, RG1–RG2 |
| SH2 | 셰이더 시스템 16절 | 쿠킹 셰이더·파이프라인 |
| GX2 | RenderBase 4.4절 | structured·typed buffer·UAV·깊이·MSAA·array·cube·mip view(18절 RQ1–RQ2) |
| GX4 | RenderBase 4.4절 | MRT·깊이 attachment·dispatch·indirect(RQ3) |
| RB2–RB6 | RenderBase 6·9–11절 | 자원 수명·셰이더 집합·상수 블록·공유 slot·정점 입력 타입·그리기 유틸리티 |
| RG2 | RenderBase 8절 | 프레임 안 풀 재사용·그래프 안 업로드 |
| 머티리얼 | 머티리얼 시스템 15.2절 | RS0b 전 최소(개정 15): (1) MS3d4 묶음 원천·asset-cook 연결과 MS3f 라이브러리 항목 — RS0a pass 타입으로 머티리얼 셰이더가 Project 라이브러리에 실제로 들어간다, (2) MS4 최소 — 머티리얼(인스턴스 재정의는 뒤)의 런타임 렌더 상태(`BlendMode` Opaque·Masked, 양면, `ShadingModel` Unlit·DefaultLit), 상수 식 실행으로 만든 상수 블록 byte, 텍스처·샘플러 바인딩, (3) 엔진 기본 머티리얼(고정 AssetId, /Engine 쿠킹). RS2 전까지 필요 없는 것: MS3e 보고, MS3g 사용 대상 유도(RS0–RS2는 정적 메시 정점 입력 하나), MS5 에디터. MS7(반투명)은 RS5, MS8(SF3)은 RS9와 함께 간다. MS4의 두 경로 기준 영상(MS-T6)은 RS2 완료 기준 (a)와 함께 본다 |
| 메시 에셋 | AssetSystem(메시 단계) | 정적 메시(위치·패킹 법선·UV·section·bounds·LOD) |
| WorldIntegration | 계획 3.1절 | 렌더 컴포넌트 → `SceneChangeList`·`ViewDesc` |

### 17.2 단계

화면 마일스톤 다음 첫 단계는 RS0이고(개정 15부터 RS0a·RS0b — RS0a는 머티리얼을 기다리지 않는다), 그다음은 Forward+(RS1)가 Deferred(RS2)보다 먼저다(결정 RN7). 이유: GLES·모바일 기본 경로가 Forward+이고(계획 11.1절) light grid·조명 함수가 Deferred의 기반이기 때문이다.

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| RS0a | 렌더 단독(머티리얼 결과 없이): `RenderData`·`RendererShaders`·`Renderer` 모듈. **`RendererShaders` 셰이더 타입을 먼저 낸다(21장 RS0a1)**: 머티리얼 pass 타입 넷(`DepthPassVertex`·`DepthPassPixel`·`ForwardOpaqueVertex`·`ForwardOpaquePixel`)과 엔진 타입(위치 전용 depth·velocity), 지원 머티리얼 판정·traits 거르기(`materialShouldCompile`, 셰이더 시스템 SSD86), 정적 메시 정점 입력 타입 `StaticMesh`, 머티리얼 문맥 공급 VIEW·VERTEX·PRIMITIVE. 이어서 `RenderScene`(변경 목록 적용), `RenderView`·`ViewHistory`, 가시성 V1, 그리기 항목 캐시·상태 묶음, `GpuScene`, depth prepass(불투명, 엔진 타입 `DepthPositionOnlyVertex` — 머티리얼 없이)·velocity, HZB 생성, 최소 tone map, 공유 상수 slot 등록(화면 마일스톤 SM10 남은 부분). 설계 21장 | 17.1절에서 머티리얼 행을 뺀 것 | (a) 기록 backend: 같은 변경 목록 열 → 같은 장치 호출 열(결정적), 적용 순서(제거→추가→갱신) 단언 (b) 가시 집합 = CPU 참조(bounds 전수) (c) GPU Scene 영역 재사용이 slot GPU 완료 gate 전에는 없음 (e) reversed-Z·앞면·velocity 부호 시험(정지 카메라 0, 알려진 이동의 velocity 값) (f) pass 타입 등록과 traits 거르기 시험(머티리얼 쿠킹이 이 타입으로 묶음을 컴파일한다 — MS3f의 선행) |
| RS0b | Unlit·emissive base pass를 머티리얼로 그린다(그리기 항목이 머티리얼의 셰이더 집합·렌더 상태·상수 블록·텍스처를 쓴다), 런타임 Project 셰이더 라이브러리 열기(RR9), 엔진 기본 머티리얼로 머티리얼 없는 메시와 지원하지 않는 머티리얼을 그린다(머티리얼 단위 대체 — 21.4절) | RS0a, 머티리얼(17.1절 머티리얼 행 — 개정 15에서 최소 항목을 적었다) | (d) D3D12·Vulkan·GLES 미리보기 Unlit 장면 기준 영상(채널 ±8, 2%, RenderBase 8절 기준) |
| RS1 | Forward+ 직접광: 광원 장면, light grid CS, `ForwardOpaque`(DefaultLit), directional·point·spot, sky light 상수 | RS0, GX2 typed buffer UAV | (a) grid 셀 목록 ⊇ CPU 참조 판정(보수), overflow 계수 정확 (b) 광원 0개 view의 grid 생략 (c) 기준 영상 3 backend (d) GLES texel buffer 경로 픽셀 = D3D12 경로(허용 오차) |
| RS2 | Deferred: GBuffer 기본 layout, `DeferredLightingPass`(7.3절) | RS1 | (a) **계획 API02**: 같은 머티리얼·메시·광원 장면의 Deferred·Forward+ 영상 대조(채널 ±8·2%로 시작, 14.7절 MRD10, 머티리얼 MS-T6과 같은 시험), 깊이·법선·velocity 일치(Forward+는 보조 출력과 GBufferA 대조) (b) GBuffer 인코딩 왕복 CPU 시험(법선·shading model bit) (c) GLES 4 MRT 다중 pass 영상 |
| RS3 | 그림자: CSM·spot atlas·point 6면, PCF, caster 판정 V5 | RS1 | (a) 두 경로 그림자 영상 대조 (b) atlas overflow 시 강등·생략 계수와 표시 (c) caster 집합 ⊇ CPU 참조 (d) 직교 깊이 clamp 미지원 backend의 공개 대체 확인 |
| RS4 | 2단계 occlusion(V2–V4), GPU 인스턴스 culling, 그리기 항목마다 간접 draw 하나(15.2절) | RS0, GX4 indirect | (a) 계획 13.1절 V_ref ⊆ V_submit(가시 누락 0), 느린 strafe·이동·삭제 occluder·cut·teleport 시퀀스 (b) 이전 HZB 없음·무효에서 전체 그리기 (c) 재검사 복구 계수 (d) GLES에서 base instance 0 간접 draw와 루트 상수 offset으로 같은 영상 |
| RS5 | 투명 3종·정렬·블렌드 모드, distortion, sky atmosphere LUT·pass, height fog | RS1, 머티리얼 MS7(SF2 반투명 블렌드 모드) | (a) 두 경로 투명 영상 대조 (b) 정렬 결정성(같은 입력 같은 순서) (c) fog·sky가 두 경로에서 같은 영상 |
| RS6 | `PostChain` 1차: 노출·bloom·tone map·LUT, FXAA, spatial upscale(U0), TAA·TAAU(U1), `Upscaler` interface | RS0, 계획 12절 | 계획 12.3절 시퀀스(정지·회전·고속·resize·cut), cut 잔상 0 프레임, history reset 조건 시험 |
| RS7 | G0 기준 조명: 반사 capture·sky light, SSAO·GTAO, decal(Deferred GBuffer decal F10·Forward+ DBuffer F8a), `StaticLighting::Baked` 표본·GBufferE | RS2·RS3, bake 스펙(계획 14.10절) | 계획 LT02(direct·indirect·mixed shadow 항 분리, 두 경로·backend 비교), 이중 가산 0(정적·동적 배율 시험) |
| RS8 | Forward+ MSAA 1·2·4, resolve(계획 11.1절) | RS1·RS6 | MSAA × temporal 조합의 resolve 대조(깊이·velocity 전경 선택), alpha-to-coverage |
| RS9 | 고급 shading model(GBufferD), rect light·IES·광원 함수 atlas | RS2·RS3, 머티리얼 MS8(SF3) | 계획 14.2절 고급 직접 조명 검증, 모델별 Deferred 인코딩·Forward 평가 대조 |
| RS10 | screen-space: SSR·contact shadow, Forward+ 보조 출력, 간접광 합성(F12) | RS2·RS6 | 순환 graph 거부(음성 대조), 두 경로 screen-space 영상 대조 |
| RS11 | GI·RT 연결점: `IndirectLightingInputs`, `RayScene` 자리, `RenderMode::PathTracing` 골격(누적·리셋·후처리 연결) | RS10, 각 방식 스펙 | 방식 없음 view와 방식 있는 view의 직접광 동일, PathTracing 리셋 조건 시험, 미지원 장치 `Unsupported` |
| RS12 | skinned 메시: 정점 입력 타입, 현재·이전 pose(velocity), 두 경로·그림자·투명. 본 행렬은 LOD 통합 bone map 기준으로 `RGBA32Float` typed buffer에 현재·이전 pose를 쓴다(섹션 분할 없음 — RenderBase 결정 R18), morph 희소 목록 적용(compute 또는 CPU)(MeshAnimationAssets MR2) | RS3·RS6, 애니메이션 skinning(계획 15절), MeshAnimationAssets MA5 | 계획 15절 "Deferred·Forward·shadow·picking에서 동일 pose", CPU skinning 기준 결과 대조, 계획 12.1절 U1 skinned motion(velocity) 시험 |
| RS13 | 디버그·편집 표시(4.2절): view별 단순 요소 수집(일반·디버그·편집), 인스턴스 종류 "디버그 선 묶음"·"아이콘 sprite", PC9a·PC9b 합성, Shipping의 base pass·depth prepass 단순 요소 경로, 깊이 우선 그룹 World·Foreground | RS0(PC8 뒤에 붙임), RS6(PostChain 정식 자리) | (a) World 그룹은 장면 깊이로 가려지고 Foreground는 늘 위 (b) 디버그 그리기 빌드(합성 pass)와 Shipping(base pass)이 같은 선 집합을 그림(가림 결과 대조) (c) 에디터 view가 아니면 PC9a가 frame graph에 없음 (d) 표시 색이 tone map·FXAA 영향을 받지 않음 (e) 3 backend 기준 영상 |

- 모든 GPU 시험은 검증 계층 오류 0(RenderBase 8절)과 렌더 lane·인라인 모드 결과 동일을 함께 요구한다.
- 기준 영상은 계획 29.2절대로 장면 manifest·카메라 경로·seed·build hash를 고정한다. Vulkan·GLES가 CI에서 NOT RUN이면 로컬 결과를 기록한다(RenderBase 18.1절과 같다).
- 볼륨 안개·구름·가상 그림자·VirtualGeometry·OIT·DOF·motion blur·HDR 출력은 별도 스펙이다.
