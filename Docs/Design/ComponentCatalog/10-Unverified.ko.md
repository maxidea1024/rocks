# 엔진 컴포넌트 도입 — 10. 확인하지 못한 것

[목차](README.ko.md)

## 10. 확인하지 못한 것

- UE 컴포넌트 속성 전체 대조표(필드 단위)는 아직 만들지 않았다. 각 묶음 착수 때 그 묶음의 UE 헤더를 필드 단위로 대조해 이 문서에 붙인다(5절 완료 조건 5).
- `UInstancedSkinnedMeshComponent`·`UHeterogeneousVolumeComponent`의 UE 5.8 성숙도(실험 기능 여부)는 헤더만으로 판정하지 못했다(헤더에 실험 표지는 없다).
- Cascade의 공식 deprecated 표지는 `UVectorFieldComponent`·`UParticleSystemComponent` 헤더에 없다. 제외 근거는 사용처(구 FX 시스템 전용)다.
- 3부 수집(9.1절)은 `class … : public [ns::]U…` 선언을 정규식으로 찾는 방식이다. 매크로로 만든 선언은 놓칠 수 있다. 리뷰에서 namespace 한정 부모(`MeshPartition::UModifierComponent`) 때문에 빠진 26종을 찾아 고쳤고, 2부 재대조에서는 `UE_DEPRECATED(…)`가 끼인 선언 4건을 찾았다.
- 베타 플러그인(ModularGameplay·AudioGameplay·MotionWarping 등)의 UE 안 성숙도 변화는 추적하지 않는다. 착수 때 `.uplugin` 표지를 다시 본다.
