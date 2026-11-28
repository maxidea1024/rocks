# 렌더러(Renderer) 설계 — 16. 성능 예산과 계측

[목차](README.ko.md)

## 16. 성능 예산과 계측

### 16.1 예산(계획 29.2절, 모두 미측정 초기 목표)

| 시험 | workload | 렌더러 기준 |
|---|---|---|
| B2 | 10만 render instance·2,000만 source 삼각형·광원 100·그림자 caster 8 | Deferred·Forward+ 각각 native 1080p GPU p95 ≤ 14 ms·p99 ≤ 16.67 ms |
| B8 | 1080p·1440p·4K 출력, 동적 render scale | 두 경로 지원 matrix 통과, cut 잔상 0 프레임 |
| B9 | dense city·실내외 GI·야간 emissive·스트리밍 | GI artifact 기준·기능별 영상 승인 |

Runtime60 GPU 14 ms 배분(계획 29.2절)을 pass 묶음에 대응시킨다.

| 예산 항목 | ms | 렌더러 pass(6.2절) |
|---|---|---|
| geometry/depth | 3.0 | F2·F4·F5·F8a·F9(Deferred GBuffer 쓰기)·F10 |
| shadow | 2.0 | F7 |
| lighting/GI | 3.0 | F3·F6·F8·F9(Forward+ 조명)·F11·F12 |
| transparency/VFX/fog | 1.5 | F13·F14·PC2·PC5 |
| upscale | 1.5 | PC3·PC10 |
| output post/UI | 1.5 | PC1·PC4·PC6–PC9·PC11·F16 |
| 여유 | 1.5 | — |

- Forward+ base pass는 geometry와 lighting에 걸치므로 나누지 않고 "F9 전체"를 lighting/GI에 넣는다. Deferred F9(GBuffer 쓰기)는 geometry/depth다. 두 경로를 비교할 때는 예산 항목 합계가 아니라 F4–F12 합계로 대조한다.
- F1(CPU)과 F3(sky LUT, 작은 compute)은 각각 CPU 예산·lighting/GI에 둔다.

### 16.2 계측

- **`GpuScope`**(RenderBase 13절): 6.2절 F1–F16과 11.2절 PC1–PC11 이름 그대로의 구간. RG1 뒤에는 그래프 pass 자동 구간이다. 예산 항목 이름을 구간 묶음으로 둔다.
- **계수**: view별 가시 인스턴스·제출 그리기·dispatch·삼각형, light grid 셀당 광원 수 histogram과 overflow, atlas 사용률과 그림자 해상도 강등·생략 수, 보류 집합 크기와 재검사 복구 수(V4), 정적·동적 GI 낮춤 결과, 지원표 거부 수, 기본 머티리얼 대체 수(14.7절 MRD8).
- **CPU**: 장면 변경 적용·가시성·그리기 항목 생성·그래프 구성 시간을 CP5 trace 구간으로(RenderBase 13.2절).
- **측정**:

| ID | 측정 | 결과가 정하는 것 |
|---|---|---|
| MR1 | B2에서 grid 조명 pass와 광원별 pass 시제품의 GPU 시간·대역폭(D3D12·Vulkan·GLES) | 결정 RN1 확정 |
| MR2 | 광원·그림자 직접 표본 비용(그림자 광원 0·8·32) | 9.2절 직접 표본 유지 또는 mask 도입 |
| MR3 | light grid 셀 크기·Z 단 수 대비 overflow·시간 | 8.1절 기본값 |
| MR4 | GPU Scene typed buffer 읽기 비용(structured 대비, D3D12·Vulkan) | 결정 RN9 재검토 |
| MR5 | Deferred·Forward+ 같은 장면 GPU 시간 | profile 기본 경로 권고 |
