# 노드 그래프(NodeGraph) 설계

## 1. 현재 상태

| 항목 | 현재 Rocks | 문제 |
|---|---|---|
| 코드 | 그래프 모델·편집 명령·그래프 화면 코드가 없다. EditorBackend 코드도 없다 | — |
| 머티리얼 | [머티리얼 시스템](../MaterialSystem/README.ko.md)에 그래프 형식이 있다: 5.2절(노드·핀·간선), 5.5절(`NodeId`·semantic hash), 14절(E1–E14 에디터 요구). 계획 14.6절에 `MaterialNodeDescriptor`·`RegisterMaterialNode` 초안이 있다 | 노드·핀·간선·undo 단위·복사 형식이 머티리얼 전용이다. 다른 도메인이 같은 것을 다시 만든다 |
| UI | [UI 툴킷](../UiToolkit/README.ko.md) 6절 표에 한 줄이 있다: "그래프 편집 `GraphView` · node·pin 시각 등록, 그래프 데이터와 schema는 각 도구 소유". UT6에 "그래프 Element"가 있다 | 화면과 데이터의 경계, 갱신 방식, 명령 범위가 정해지지 않았다 |
| 편집 계약 | 계획 22.1절: 화면 → EditorBackend typed 명령(expected revision·stable ID·thread affinity 검증). 22.2절: Transaction이 stable ID·FieldId·old/new·asset revision을 기록하고, 객체 생성·삭제·reparent는 graph patch로 저장하며, undo 기간 snapshot 메모리는 budget으로 제한한다. drag는 release 때 undo 1개다 | 22.2절의 graph patch는 객체 계층용이다. 노드 그래프 편집(노드 추가·연결·붙여넣기·접기)의 patch 형식이 없다 |
| 다른 그래프 도메인 | 오디오: `SoundGraph`(AU10·M7 — Sound Cue 대응은 두지 않는다, [오디오 시스템](../AudioSystem/02-Terms.ko.md) 2절). 애니메이션: blend tree·state machine·graph compiler(계획 15절 1522행). PCG graph(계획 24.3절). VFX(계획 21절). MetaSounds형 DSP graph(계획 24.3절) | 모두 "전용 데이터 편집기"로만 적혀 있고 공통 기반이 없다 |
| 정책 | Blueprint·범용 gameplay node graph 제외: 계획 14.5·22.2·24.3절, [레벨 스크립트](../LevelScript.ko.md) LD1·LD2, [게임플레이 작업 안내](../../Guides/GameplayWorkflowGuide.ko.md) 512행 | 공통 그래프 층이 실행 흐름·VM으로 커지지 않게 경계를 문서로 막아야 한다 |
| Core 기반 | `Guid`·`TryNewGuid`(`Core/Source/Runtime/Core/Public/Core/Guid.h`). JSON 읽기(Strict·Json5)·쓰기(`JsonWriter`는 늘 엄격한 Canonical, `Core/Source/Runtime/Core/Public/Core/Text/Json.h:136–142`). 리플렉션 Record와 InstancedStruct wire 형식(`Core/Source/Runtime/Serialization/Public/Serialization/InstancedStructWire.h`, struct ID는 Guid). 리플렉션 B9의 `InstancedStruct`·`OrderedMap`은 구현 완료지만 ObjectRuntime에 있다 | `OrderedMap`은 삽입 순서를 저장·비교에 넣는다(`OrderedMap.h:3–14`). `InstancedStruct`는 TypeLease를 쥐고, 등록되지 않은 struct는 decode에서 거부·비움만 할 수 있다([리플렉션 확장](../ReflectionExtensions.ko.md) 11절). 2D 공간 색인이 없다([공간 색인](../SpatialIndex.ko.md): 해시·2D 격자는 후속) |
