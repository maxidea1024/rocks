# 노드 그래프(NodeGraph) 설계

## 10. 노드 등록·버전

**등록.** `RegisterNodeType(DomainId, const NodeTypeDescriptor&, ModuleLease)`. 등록 때 다음을 검사한다.
- 핀 키 중복
- 타입 색인 범위
- 속성 struct ID가 리플렉션에 등록되어 있는지(도메인 쪽 등록 도우미가 확인)
- migration 함수가 현재 버전까지 이어지는지

**열 때 migration.**
- 노드의 `typeSchemaVersion`이 현재보다 낮으면 `migrate`를 버전마다 차례로 적용해 `MigrateNode` 명령 목록을 만든다.
- 이 목록은 문서 열기 단계에서 적용한다. undo 이력에 넣지 않으며, 열기 직후의 문서가 undo 이력의 시작점이다.
- 결과는 진단 정보 수준으로 보고한다(머티리얼 시스템 E13).
- migration이 핀 키를 바꾸면 간선·기본값을 새 키로 옮긴다. 옮길 곳이 없으면 고아 핀으로 보존한다.
- 저장하지 않고 닫으면 파일은 옛 버전 그대로다. 쿠커는 8.4절대로 같은 migration을 메모리에서 거친다.

**노드 교체.** migration은 다른 `NodeTypeId`를 돌려줄 수 있다(1:1). 여러 노드 → 하나(MetaSound `INodeUpdateTransform`)는 NG8에서 정한다.

**해제.** 문서와 patch는 속성을 바이트로 가지므로(GD23) 노드 종류 lease를 쥐지 않는다. `Unregister` 순서는 다음과 같다.
1. 메뉴와 새 해석 요청에서 그 종류를 뺀다.
2. 진행 중인 해석·컴파일 작업을 drain한다.
3. lease를 놓는다.
4. `NodeTypesChanged`를 보낸다. 열린 문서의 그 종류 노드는 자리 노드로 표시된다. 문서 바이트와 revision은 그대로다.

다시 등록되면 같은 통지로 원래 표시로 돌아간다.
