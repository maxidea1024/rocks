# Rocks 통합 계획 최종 교차 검증 — 렌더·콘텐츠·수학

검토일: 2026-11-10. 대상은 `Docs/Design/EngineImplementationPlan.ko.md`의 §2.5/§33.6까지 통합된 본문이다. 재현 가능한 위치 기록을 위해 `Docs/Reviews/rocks_review_snapshot.md`(340,016 bytes, SHA-256 `0e81354db67d76e4e1b903c4822dba1a1660531f3623a808b8abd9c9f5e03b5f`)를 기준으로 아래 줄 번호를 부여했다. 보고서 작성 직전 현 본문에서 해당 문구와 계약 부재를 다시 확인했다. 원본은 수정하지 않았다.

이는 설계 문서의 독립 반례 검토이며 구현·벤치마크·GPU 영상 검증의 완료 판정이 아니다. 기능 수를 늘리는 제안은 제외했다. P0는 발견하지 않았고, 필수 기능 사이의 연결 계약 4건(P1)과 검증·표현 정리 2건(P2)을 보완 권고한다. FR03/FR04는 기존 규약이 잘못되었다는 단정이 아니라, 서로 다른 구현으로 해석될 수 있는 식별·격리 범위를 명시하는 수정이다.

## 1. 수정이 필요한 연결 계약

### FR01 · P1 — CPU baker의 재질 평가 경로가 정의되지 않음

**위치·근거.** §14.6 표 1021줄은 MaterialCompiler를 `typed DAG 검증·IR·HLSL` 경로로 정의한다. §14.7 1057줄의 생성 대상은 Deferred/Forward·depth/shadow/velocity이다. §14.10 1106줄은 M4에 **CPU 기준 baker**를 필수로 두고 GPU baker보다 CPU/headless 재현성을 선행한다. 1108줄의 BakeScene에는 material 값이 포함되지만, CPU가 재질을 평가하는 형식·지원 범위·오류 정책은 없다. 1120줄 LT01에는 alpha geometry 검증이 있다. §33.6 AAA03의 RayMaterialProfile은 M8–M9 RT/cache용이어서 M4 CPU baker의 계약을 대신하지 않는다.

**실패 반례.** 기본 그래프만 사용해도 UV 변환 뒤 texture mask로 잎의 구멍을 만들거나 texture emissive를 사용할 수 있다. CPU baker에 scalar parameter만 넘기고 HLSL을 평가하지 않으면 잎이 불투명한 그림자를 만들고 발광 분포가 달라진다. 이를 GPU raster/readback으로만 해결하면 GPU 없는 CPU baker가 실행되지 않는다. 선택 Custom HLSL은 일반적인 CPU 실행 가능성을 보장하지 않는다.

**최소 교정.** M3의 artifact 계약에 `BakeMaterialProfile`을 추가한다. 기본 PBR·opacity/mask·emissive·필요 normal/UV 연산의 지원 IR 부분집합을 CPU evaluator 또는 명시적인 CPU 소비용 surface 표현으로 변환한다. texture/instance/time 입력은 BakeScene revision에 고정한다. derivative·view-dependent·Custom HLSL·지원 밖 node는 정확/근사/proxy/미지원 상태를 선언하며, 승인된 bake proxy가 없으면 원인 위치가 있는 bake 오류를 반환한다. 자동으로 불투명/평균색을 대입하여 성공시키지 않는다. GPU 평가를 사용하는 별도 profile은 CPU-only 완료 기준으로 사용하지 않는다. M8의 RayMaterialProfile과 표현을 재사용할 수 있지만 M4 계약을 뒤로 미루지 않는다.

**수용 기준.** LT01/LT06에 UV 변환·masked texture·emissive·static switch·material function fixture를 넣는다. 고정 입력의 CPU 표면 평가를 기준 값과 대조하고, GPU 없는 환경에서 지원 부분집합의 전체 bake를 수행한다. 미지원 Custom HLSL은 명시 오류 또는 승인 proxy로만 완료한다. material revision 변경·취소 뒤 오래된 bake 게시가 0건이어야 한다.

### FR02 · P1 — reduced-mip 교체 시 논리 mip·texture 크기 의미가 바뀔 수 있음

**위치·근거.** §9.5 676줄은 실제 메모리 회수를 위해 `reduced-mip allocation 교체`와 minLOD/SRV·descriptor snapshot 갱신을 요구한다. §14.7 1057줄은 IR에 texture 의미를 기록하지만, 원본 texture의 논리 크기·논리 mip과 물리적으로 축소된 allocation의 변환 계약은 없다.

**실패 반례.** 4096 texture의 논리 mip 3부터만 상주시키기 위해 512 크기의 새 allocation을 만든다. 원본의 `SampleLevel(..., 4)` 의미를 그대로 물리 sampler에 전달하면 논리 mip 7을 선택한다. texture 크기 node가 원본 4096 대신 물리 512를 반환하면 texel 기반 UV·WPO·필터 간격도 streaming에 따라 변한다. minLOD clamp만으로 explicit LOD와 dimension query의 의미를 보존할 수 없다. HLSL에서 SampleLevel의 LOD는 mip을 선택하고 GetDimensions는 지정 mip의 크기를 반환하므로, backend wrapper의 변환이 필요하다. [Microsoft SampleLevel](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplelevel), [Microsoft GetDimensions](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-getdimensions)

**최소 교정.** texture binding snapshot에 `logicalExtent`, `logicalMipCount`, `residentBaseMip`, `residentMipCount`를 포함하고 descriptor와 같은 generation으로 게시한다. Material IR의 일반 크기 query는 논리 크기이며 resident 크기 query는 별도 명시 기능이다. explicit LOD는 논리 LOD를 resident 구간으로 clamp하고 물리 base를 보정한다. implicit derivative sampling의 일관성도 정의한다. integer `Load`/texel 좌표 접근은 동일한 논리 의미를 구현하거나 해당 사용 texture를 full-resident로 제한하고 지원 제약을 진단한다. 단순히 모든 sampler 입력에 bias를 더하는 공통 처리로 대체하지 않는다.

**수용 기준.** TX02/TX05/API02에 mip마다 색이 다른 비정방형 texture·explicit LOD·dimension 기반 UV/WPO를 추가한다. 승격·축소 전후 같은 이용 가능 논리 mip의 결과를 대조한다. descriptor와 metadata의 서로 다른 generation 조합이 0건이어야 하며, 두 렌더 경로와 각 backend의 해당 지원 profile에서 수행한다.

### FR03 · P1 — 공통 GPU handle/fence의 device incarnation 범위가 불명확

**위치·근거.** §11.2 804줄은 `GpuFencePoint { QueueId queue; uint64 value; }`, 822줄은 `ResourceHandle은 slot+generation`으로 정의한다. §9.5 678줄은 upload 요청에 device generation을 포함하고, §9.6 697/703/705줄은 builtin texture handle·생성·DeviceLost를 device generation으로 관리한다. 이 규약이 일반 resource·descriptor·recording packet·fence에도 동일하게 적용되는지는 명시되지 않았다.

**실패 반례.** 장치 재생성 후 queue 0/fence 42와 resource slot 12/generation 1이 재사용된다. 구 장치의 지연 submit/회수 callback이 이 값을 신장치의 registry에 적용하면 새 resource를 찾아 사용하거나, 새 fence 42의 완료를 구 resource 완료로 오인할 수 있다. 두 device가 동시에 존재하는 경우도 같다. builtin만 device generation을 검사해도 일반 PSO/upload/descriptor에는 충분하지 않다.

**최소 교정.** GPU 식별자는 소유 device와 incarnation으로 scope한다. `DeviceId/DeviceEpoch` 필드를 명시하거나 동등한 전역 고유 registry domain을 증명한다. fence 의미는 `(device incarnation, queue, value)`이며 resource/descriptor/packet/vendor context에도 같은 소유 domain을 기록한다. lease 획득·graph import·Submit·Retire·비동기 완료에서 domain을 검사한다. 구 incarnation의 불확실한 완료는 그 recovery domain에서만 처리하며 새 device의 fence나 pool로 충당하지 않는다. 필드를 모든 작은 handle에 중복 저장해야 한다는 요구는 아니다.

**수용 기준.** API04/BR04에 device A/B가 같은 slot·queue value를 갖는 fixture, recreation 뒤 구 upload·compile·recording·fence callback의 지연 도착을 추가한다. 구 handle의 새 자원 resolve·조기 retire·새 BuiltinReady 덮어쓰기가 0건이어야 한다. 종료만 지원하는 backend는 해당 정책을 검증하고, 복구 미지원 자체를 오류로 보지 않는다.

### FR04 · P1 — Editor/PIE의 ‘동일 RenderScene 공유’ 범위가 session 격리와 충돌 가능

**위치·근거.** §11.1 767줄은 `에디터 viewport와 PIE가 다른 경로를 사용해도 동일 RenderScene을 공유`하도록 요구한다. §22.6 1512줄은 EditorWorld와 PlaySession의 가변 상태를 분리하고 불변 asset만 공유한다. 1514줄은 render proxy의 session owner를 요구한다. §11.4 842줄의 scene 공유·shadow cache key·ViewId별 history는 world/session namespace를 명시하지 않는다.

**실패 반례.** 편집 viewport는 정지한 entity, PIE는 이동한 복제 entity를 표시한다. 동일한 가변 scene에 두 proxy가 들어가고 view가 session별 가시 집합을 구분하지 않으면 양쪽에서 중복 표시된다. cache key의 object/light revision 또는 ViewId가 session 내에서만 유일하면 한 session의 shadow/history가 다른 session에 적용될 수 있다. owner 추적만으로 그리기·cache lookup 격리가 자동 보장되지는 않는다.

**최소 교정.** ‘동일 RenderScene 공유’는 **같은 World/session을 보는 복수 view가 Deferred/Forward를 다르게 선택하는 경우**로 한정한다. EditorWorld/각 PIE는 별도 논리 RenderScene을 가지거나 `(SessionId, WorldEpoch)` namespace와 가시성 격리를 제공한다. GPU arena·불변 mesh/texture·pipeline cache 공유와 가변 scene 공유를 구분한다. RenderInstanceId·scene snapshot·shadow/GI cache·temporal view key는 소유 world/session 또는 동등한 전역 고유 generation을 포함한다. ViewId 재사용에는 generation reset을 요구한다.

**수용 기준.** PI03/PI05에 같은 asset·같은 좌표에서 서로 다른 transform/material/light를 갖는 EditorWorld+PIE 2개를 구성한다. 한 session만 움직임/stop/seek하고 다른 영상·history·resource가 유지되는지 확인한다. ID 재사용과 지연 GPU submit을 겹쳐 타 session 표시·history 오염·lease 회수 0건을 검증한다.

## 2. P2 검증·표현 보완

### FR05 · P2 — memory pressure에서 축소 작업의 진행 조건을 명시

**위치.** §9.5 676/680/688줄은 resident·temporary peak·pin admission·fallback과 TX03을 갖추고 있다. 다만 예산을 모두 사용한 상태에서 reduced-mip allocation을 새로 만들어야 하는 축소 경로의 진행 규칙은 없다.

**반례.** resident cap을 이미 채웠는데 작은 대체 texture도 구 allocation보다 먼저 생성해야 한다. 모든 축소 요청이 temporary budget admission에 실패하고 재시도만 하면 사용량을 줄일 수 없다. 현재 실패 fallback 문구로 해결 가능한 구현은 존재하므로 P1 모순으로 분류하지 않았다.

**최소 교정·시험.** 일반 admission이 소비하지 않는 전환 여유분 또는 이미 상주한 typed builtin으로 교체→구 lease/fence 종료→구 allocation 해제→tail 재생성의 압박 경로 중 하나를 명시한다. pin·cinematic 요구는 실패/품질 저하 상태를 노출하며 무한 retry하지 않는다. TX03에 cap 100%·강제 budget 감소·구 recording 유지·allocation 실패를 넣어, 상한을 넘지 않으면서 fence 해소 후 eviction이 진행되거나 명시 종료하는지 확인한다.

### FR06 · P2 — 신규 backend 범위와 오래된 예시를 맞춤

1. §3.2 217줄 `Engine/RHI/{Null,D3D12,Vulkan}`에 D3D11/Metal/OpenGL을 추가한다. 디렉터리 예시이지만 최초 5 backend 범위와 맞추어 초기 작업 분해의 누락을 방지한다.
2. §14.3 983줄 `DXR 장치가 없으면 software/probe`는 `선택 backend/profile의 hardware-ray-tracing capability가 없으면`으로 바꾼다. DXR은 D3D12 경로의 예시로 둔다. Vulkan/Metal 장치를 DXR 부재만으로 software로 강등시키는 해석을 피한다.
3. §9.6 699줄 `fallback mesh shader`는 하드웨어 Mesh Shader 기능과 혼동되지 않게 `기본 mesh 렌더링용 fallback vertex/pixel shader(또는 backend 동등 단계)`로 명확히 한다. §11.5 858줄의 mesh shader 선택 capability는 유지한다.

이는 기능 추가가 아니다. API manifest·module 목록·builtin baseline profile을 대조하여 5개 backend의 기본 profile이 고급 mesh/ray capability를 필수로 요구하지 않는지 확인한다.

## 3. 반영이 유지된 요구와 기존 수정

| 검토 항목 | 판정·근거 |
|---|---|
| Core/Object 첫 작업 | 유지. §9.5/§11.5는 GPU 실구현을 Foundation 뒤로 분리하며 §30은 전문 기능을 기반 완료의 선행 조건으로 만들지 않는다. |
| 5 API와 Deferred/Forward | 유지. §2.5/§11.5는 target와 compiler host를 구분하고 두 경로의 기본 profile을 모두 필수로 한다. 모든 OS에서 모든 API가 작동한다는 약속은 없다. Mac Studio도 정식 개발 환경이다. |
| Shader toolchain·runtime 예외 | 유지. D3D11 DXBC와 D3D12 DXIL을 구분한다. §14.7은 OpenGL의 target GLSL compile/link 및 driver binary fallback을 원본 material graph/HLSL 도구 포함과 구별한다. |
| Material Editor와 Blueprint 제외 | 유지. §14.5–14.9의 편집·함수·레이어·미리보기·Undo·진단·compile revision은 필수 단계가 있다. 전문 재질 그래프를 범용 게임플레이 시각 언어로 확대하지 않으며 게임플레이 커스텀 언어 원칙이 유지된다. |
| Streaming·builtin bootstrap | 핵심 유지. 실제 backing memory 회수·구/신 peak·취소 세대·lease/fence·typed fallback·pin carveout·DDC 독립 bootstrap·CPU-only GPU 금지가 명시되었다. FR02/FR05는 이 경로의 추가 경계다. |
| 베이크·프로브 의미 | 범위 정의 유지. baked direct/indirect와 Mixed runtime 항이 분리되며, baked probe를 움직이는 객체가 샘플링하는 것·scenario blend·runtime GI 재계산을 구별한다. FR01은 이 기능들의 첫 실행 경로를 연결한다. |
| 수학·명명·SIMD | 유지. §3.3은 MS식 명명 불호와 라이브러리 성능을 혼동하지 않는다. 공개 Rocks storage/API, f32/f64, 후보 미선정·benchmark 미실행, 채택 library adapter를 명시한다. 작은 연산별 virtual/function-pointer dispatch를 금지하고 batch dispatch·SSE2/선택 AVX2/FMA의 OS 상태 검사·ARM64 NEON 검증을 요구한다. |
| Inline·의존성 절충 | 유지. §3.3 253줄은 vendor header 완전 비공개와 모든 작은 연산 무비용 inline의 동시 보장을 하지 않는다. detail inline 의존과 private TU/LTO의 비용을 측정하도록 규정한다. |
| GPU recording/retirement | 기존 수정 유지. §11.2는 Retire 입력 fence를 하한으로 보고 내부 last-use를 권위로 둔다. 늦은 Submit의 fence 등록과 lease 이관이 원자 경계 안에 있으며, 제출 불확실성은 recovery record에 보존한다. FR03은 그 기록의 device domain을 명확히 한다. |
| HZB·화면 공간 효과 | 기존 수정 유지. §11.4는 이전 HZB reject를 같은 frame의 현재 HZB로 재검증하며 SSR/SSGI의 current lit color 순서를 보존한다. 이중 indirect 합성과 불완전 false-cull을 완료 기준에서 배제한다. |
| Upscaling·post·UI | 요구 유지. §12/§21/§26은 temporal input·motion·reactive·history, Forward/MSAA 지원표, output 해상도 UI·HDR 밝기, 고급 post catalog와 기능별 검증 단계를 갖춘다. 효과의 연속 parameter 변경마다 history를 무조건 지우는 회귀는 발견하지 않았다. |
| Cutscene·PIE 종료 | 기존 수정 유지. §25는 제어권의 AlwaysRestore/자동 release와 명시 property Keep 정책을 구분하고, seek/skip·offline capture·side-effect ledger를 갖춘다. §22.6은 Stop 후 task/IO/GPU/GC drain을 요구한다. FR04는 동시에 살아 있는 scene의 렌더 격리 범위를 보완한다. |
| AAA 비교 범위 | 유지. §33.6은 Snowdrop/IW 관련 게임 기술/RAGE의 공개 자료와 Rocks의 후속 설계를 구분하며, proprietary 내부 동등성이나 일반 오픈소스 비교로 확대하지 않는다. |

검토 범위에서 사용자 요구의 대분류가 새로 빠진 항목은 발견하지 않았다. 위 수정은 이미 필수인 재질·베이크·streaming·PIE·다중 backend가 함께 작동하도록 계약을 닫는 작업이다. 우선 FR01–FR04를 해당 M3/M4 설계 gate에 반영하고 FR05/FR06을 같은 변경에서 정리하는 것을 권고한다.

## 4. 수정 반영 확인

2026-11-10 후속 확인: 현 `Docs/Design/EngineImplementationPlan.ko.md`의 FR01–FR06 관련 수정과 인접 계약만 재검토했다. 아래 줄 번호는 최초 snapshot이 아닌 수정된 본문 기준이다. **6건 모두 문서 계약의 반례가 해소되었다.** 이는 계획의 closure이며 구현 시험 PASS를 뜻하지 않는다. 이 변경에서 새로운 필수 수정 사항은 발견하지 않았다.

| 항목 | 판정 | 확인 근거 |
|---|---|---|
| FR01 | PASS | §14.10 1186줄에 M3 CPU 소비용 BakeMaterialProfile, M4 지원 부분집합, offline texture sampling·고정 time/instance, 미지원/proxy 진단, GPU raster/readback 비의존, LT01/LT06 fixture가 추가되었다. M8 RayMaterialProfile과 독립된 초기 계약이다. |
| FR02 | PASS | §9.5 734줄에 논리 extent/mip과 resident base/range, descriptor 동시 generation 게시, logical explicit LOD 변환, dimension query 의미, Load의 논리 좌표 또는 full-resident 제한, 두 경로/backend 시험이 추가되었다. |
| FR03 | PASS | §11.2 865–870줄의 GpuFencePoint가 DeviceId/deviceEpoch를 포함한다. 893줄에서 일반 resource·descriptor·packet·vendor context·registry domain·비동기 완료에 소유 device incarnation을 적용하며 구 recovery와 신장치 pool을 분리한다. 기존 recording lease/fence 원자 이관 규약도 885–887줄에 유지된다. |
| FR04 | PASS | §11.1 817줄은 RenderScene snapshot 공유를 같은 World/session view로 한정한다. EditorWorld/PIE의 논리 scene 또는 namespace/가시성/proxy 격리가 명시되었다. §11.4 915줄은 scene/cache/history key·ViewId generation과 PI03/PI05의 타 session 오염 반례를 함께 다룬다. |
| FR05 | PASS | §9.5 736줄에 전환 여유분과 이미 상주한 builtin으로 교체 후 구 recording/fence drain→backing 해제→tail 재생성 경로가 추가되었다. pin/cinematic 품질 저하·실패 노출 및 TX03의 포화·강제 감소·실패 시험이 있어 무한 재시도를 정상 처리로 볼 수 없다. |
| FR06 | PASS | §3.2 221줄의 RHI 폴더 예시는 5 backend+Null을 포함한다. §14.3 1056줄은 backend/profile의 hardware ray tracing capability를 검사한다. §9.6 742줄의 fallback은 기본 mesh용 vertex/pixel shader 또는 backend 동등 단계로 명확해졌다. |

추가된 §3.4의 4-space·고정 formatter·vendor 원본 비재포맷 정책은 기존 수학 adapter/공개 명명 정책과 충돌하지 않는다. formatter 검증과 엔진 컴파일·기능 검증을 별개로 취급하는 문구도 유지된다. 이번 확인에서는 formatter 실행을 중복 수행하거나 새 전체 검토로 범위를 넓히지 않았다.
