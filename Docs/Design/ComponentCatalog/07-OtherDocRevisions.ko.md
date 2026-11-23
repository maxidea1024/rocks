# 엔진 컴포넌트 도입 — 7. 다른 문서 개정

[목차](README.ko.md)

## 7. 다른 문서 개정

개정 2(2026-11-23)에서 모두 반영했다. 요구의 정본은 여전히 이 문서 4절이고, 각 문서에는 요구 요약과 링크만 넣었다.

| 문서 | 고친 것 |
|---|---|
| ER 7.3·8절 | 7.3절에 보강 예정 항목(CR1 시스템 상태 hook, CR2 SceneComponent 보강), 8절에 Tick 단계 대조 미결(CR8a) |
| EIP 20.2절 | tick group 문장을 구현된 ER 8절 단계 기준으로 고치고, 물리 결과 반영·애니메이션 마무리 지점은 CR8a에서 정한다고 적음 |
| RN 3.2·4.2절 | `WorldIntegration` 행에 이 문서 3절 링크, 4.2절에 "디버그·편집 표시"(CR23). 처음 표의 RenderBase 대상은 잘못이었다 — UE 디버그 선은 RenderCore가 아니라 렌더러의 동적 메시 경로다 |
| AU 3.15.2절 | 영역 형상을 공용 `VolumeShape`로(CR17) |
| AssetSystem 5.7절 4항 | 객체 단위 EditorOnly 활성 시점을 컴포넌트 도입(CR5)으로 연결 |
| ER 3절 | `Gameplay` 행에 미착수와 입력·햅틱 컴포넌트 선행(CR26) |
| PhysicsJoltAdapter | 관련 문서에 컴포넌트 쪽 요구(CR16) 링크 |
